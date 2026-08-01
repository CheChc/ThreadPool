#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/prctl.h>

/* 单次扩容/缩容的步长（线程数） */
#define ADJUST_STEP 2

/* 管理线程的检测间隔（秒） */
#define MANAGE_INTERVAL 2

typedef struct Task {
    void (*function)(void* arg);
    void* arg;
} Task;

struct ThreadPool {
    Task* taskQ;            /* 任务队列（环形缓冲） */
    int queueCapacity;      /* 队列容量 */
    int queueSize;          /* 队列当前任务数 */
    int queueFront;         /* 队头下标 */
    int queueRear;          /* 队尾下标 */

    pthread_t managerID;    /* 管理线程 ID */
    pthread_t* threadIDs;   /* worker 线程 ID 数组，0 表示空槽位 */
    int minNum;             /* 最小（初始）线程数 */
    int maxNum;             /* 最大线程数 */
    int busyNum;            /* 忙线程数（受 mutexbusy 保护） */
    int liveNum;            /* 存活线程数 */
    int exitNum;            /* 待退出线程数 */

    pthread_mutex_t mutexpool;  /* 保护池状态：队列、线程数、shutdown */
    pthread_mutex_t mutexbusy;  /* 保护 busyNum */
    pthread_cond_t notFull;     /* 队列未满（生产者等待） */
    pthread_cond_t notEmpty;    /* 队列非空（消费者等待） */
    pthread_cond_t manageWait;  /* 管理线程定时等待（destroy 时唤醒，避免干等 2 秒） */

    int shutdown;           /* 1 = 开始销毁 */
};

/*
 * 当前线程退出：把自己的槽位置 0 后退出。
 * 调用前不得持有任何锁（内部会加锁，避免退出时锁永久不释放）。
 */
static void threadExit(ThreadPool* pool)
{
    pthread_t tid = pthread_self();

    pthread_mutex_lock(&pool->mutexpool);
    for (int i = 0; i < pool->maxNum; ++i) {
        if (pool->threadIDs[i] != 0 && pthread_equal(pool->threadIDs[i], tid)) {
            pool->threadIDs[i] = 0;
            pool->liveNum--;
            break;
        }
    }
    pthread_mutex_unlock(&pool->mutexpool);

    pthread_exit(NULL);
}

/* 消费者线程：从队列取任务执行 */
static void* worker(void* arg)
{
    ThreadPool* pool = (ThreadPool*)arg;
    prctl(PR_SET_NAME, "tp-worker", 0, 0, 0);

    for (;;) {
        pthread_mutex_lock(&pool->mutexpool);

        /* 队列空且未关闭：阻塞等待；被缩容信号唤醒时也跳出 */
        while (pool->queueSize == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->notEmpty, &pool->mutexpool);
            if (pool->exitNum > 0)
                break;
        }

        /* 缩容：队列已空时退出 */
        if (pool->exitNum > 0 && pool->queueSize == 0) {
            pool->exitNum--;
            pthread_mutex_unlock(&pool->mutexpool);
            threadExit(pool);
        }

        /* 关闭且队列已空：正常退出（队列非空则继续消费完，优雅停机） */
        if (pool->shutdown && pool->queueSize == 0) {
            pthread_mutex_unlock(&pool->mutexpool);
            threadExit(pool);
        }

        /* 取出队头任务 */
        Task task = pool->taskQ[pool->queueFront];
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
        pool->queueSize--;

        pthread_cond_signal(&pool->notFull);
        pthread_mutex_unlock(&pool->mutexpool);

        /* 执行任务（busyNum 用独立锁，不阻塞其他消费者取任务） */
        pthread_mutex_lock(&pool->mutexbusy);
        pool->busyNum++;
        pthread_mutex_unlock(&pool->mutexbusy);

        task.function(task.arg);

        pthread_mutex_lock(&pool->mutexbusy);
        pool->busyNum--;
        pthread_mutex_unlock(&pool->mutexbusy);
    }
    return NULL;
}

/* 管理线程：周期性检查负载，动态扩缩容。退出时不清理资源（由 Destroy 统一清理） */
static void* manager(void* arg)
{
    ThreadPool* pool = (ThreadPool*)arg;
    prctl(PR_SET_NAME, "tp-manager", 0, 0, 0);

    for (;;) {
        /* 定时等待（可被 destroy 提前唤醒），避免 destroy 时干等整个间隔 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += MANAGE_INTERVAL;

        pthread_mutex_lock(&pool->mutexpool);
        int woken = pthread_cond_timedwait(&pool->manageWait, &pool->mutexpool, &ts);
        int shutdown = pool->shutdown;
        int queueSize = pool->queueSize;
        int liveNum = pool->liveNum;
        pthread_mutex_unlock(&pool->mutexpool);

        if (shutdown)
            break;
        if (woken != ETIMEDOUT)
            continue;   /* 非超时唤醒（destroy 广播），shutdown 已检查 */

        pthread_mutex_lock(&pool->mutexbusy);
        int busyNum = pool->busyNum;
        pthread_mutex_unlock(&pool->mutexbusy);

        int idleNum = liveNum - busyNum;    /* 空闲线程数 */

        /* 扩容：队列里有排队任务且线程数未达上限 */
        if (queueSize > idleNum && liveNum < pool->maxNum) {
            pthread_mutex_lock(&pool->mutexpool);
            int toCreate = pool->maxNum - pool->liveNum;
            if (toCreate > ADJUST_STEP)
                toCreate = ADJUST_STEP;
            for (int i = 0, created = 0;
                 i < pool->maxNum && created < toCreate; ++i) {
                if (pool->threadIDs[i] == 0) {
                    pthread_t tid;
                    if (pthread_create(&tid, NULL, worker, pool) == 0) {
                        pool->threadIDs[i] = tid;
                        pool->liveNum++;
                        created++;
                    }
                }
            }
            pthread_mutex_unlock(&pool->mutexpool);
        }

        /* 缩容：空闲线程比最小线程数多出至少一步长 */
        if (idleNum - pool->minNum >= ADJUST_STEP) {
            int toExit = idleNum - pool->minNum;
            if (toExit > ADJUST_STEP)
                toExit = ADJUST_STEP;

            pthread_mutex_lock(&pool->mutexpool);
            pool->exitNum += toExit;
            /* 锁内广播，确保所有空闲 worker 都能被唤醒并退出 */
            pthread_cond_broadcast(&pool->notEmpty);
            pthread_mutex_unlock(&pool->mutexpool);
        }
    }
    return NULL;
}

ThreadPool* threadPoolCreate(int min, int max, int queueSize)
{
    if (min <= 0 || max < min || queueSize <= 0)
        return NULL;

    ThreadPool* pool = calloc(1, sizeof(ThreadPool));
    if (pool == NULL)
        return NULL;

    pool->threadIDs = calloc(max, sizeof(pthread_t));   /* calloc 保证空槽位为 0 */
    pool->taskQ = calloc(queueSize, sizeof(Task));
    if (pool->threadIDs == NULL || pool->taskQ == NULL) {
        free(pool->threadIDs);
        free(pool->taskQ);
        free(pool);
        return NULL;
    }

    pool->minNum = min;
    pool->maxNum = max;
    pool->queueCapacity = queueSize;
    pool->shutdown = 0;

    if (pthread_mutex_init(&pool->mutexpool, NULL) != 0 ||
        pthread_mutex_init(&pool->mutexbusy, NULL) != 0 ||
        pthread_cond_init(&pool->notFull, NULL) != 0 ||
        pthread_cond_init(&pool->notEmpty, NULL) != 0 ||
        pthread_cond_init(&pool->manageWait, NULL) != 0) {
        free(pool->threadIDs);
        free(pool->taskQ);
        free(pool);
        return NULL;
    }

    if (pthread_create(&pool->managerID, NULL, manager, pool) != 0) {
        pthread_mutex_destroy(&pool->mutexpool);
        pthread_mutex_destroy(&pool->mutexbusy);
        pthread_cond_destroy(&pool->notFull);
        pthread_cond_destroy(&pool->notEmpty);
        pthread_cond_destroy(&pool->manageWait);
        free(pool->threadIDs);
        free(pool->taskQ);
        free(pool);
        return NULL;
    }

    /* 创建初始 worker（部分失败不致命，管理线程会按需补齐） */
    for (int i = 0; i < min; ++i) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, worker, pool) == 0) {
            pool->threadIDs[i] = tid;
            pool->liveNum++;
        }
    }
    return pool;
}

int threadPoolAdd(ThreadPool* pool, void (*func)(void*), void* arg)
{
    if (pool == NULL || func == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexpool);

    /* 队列满且未关闭：阻塞等待消费者腾出空间 */
    while (pool->queueSize == pool->queueCapacity && !pool->shutdown) {
        pthread_cond_wait(&pool->notFull, &pool->mutexpool);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutexpool);
        return -1;
    }

    pool->taskQ[pool->queueRear].function = func;
    pool->taskQ[pool->queueRear].arg = arg;
    pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
    pool->queueSize++;

    pthread_cond_signal(&pool->notEmpty);
    pthread_mutex_unlock(&pool->mutexpool);
    return 0;
}

int threadPoolBusyNum(ThreadPool* pool)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexbusy);
    int busyNum = pool->busyNum;
    pthread_mutex_unlock(&pool->mutexbusy);
    return busyNum;
}

int threadPoolAliveNum(ThreadPool* pool)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexpool);
    int aliveNum = pool->liveNum;
    pthread_mutex_unlock(&pool->mutexpool);
    return aliveNum;
}

int threadPoolDestroy(ThreadPool* pool)
{
    if (pool == NULL)
        return -1;

    /* 1. 置关闭标志，唤醒所有阻塞的 worker 和生产者 */
    pthread_mutex_lock(&pool->mutexpool);
    if (pool->shutdown) {               /* 防止重复销毁 */
        pthread_mutex_unlock(&pool->mutexpool);
        return -1;
    }
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->notEmpty);    /* 唤醒 worker */
    pthread_cond_broadcast(&pool->notFull);     /* 唤醒阻塞在 Add 的生产者 */
    pthread_cond_broadcast(&pool->manageWait);  /* 唤醒 manager，立即退出 */
    pthread_mutex_unlock(&pool->mutexpool);

    /* 2. 回收管理线程（它自己退出，不清理资源） */
    pthread_join(pool->managerID, NULL);

    /* 3. 回收所有 worker（优雅停机：队列中剩余任务会被消费完） */
    for (int i = 0; i < pool->maxNum; ++i) {
        if (pool->threadIDs[i] != 0)
            pthread_join(pool->threadIDs[i], NULL);
    }

    /* 4. 释放资源（此时已无任何线程访问 pool） */
    free(pool->taskQ);
    free(pool->threadIDs);
    pthread_mutex_destroy(&pool->mutexpool);
    pthread_mutex_destroy(&pool->mutexbusy);
    pthread_cond_destroy(&pool->notFull);
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->manageWait);
    free(pool);
    return 0;
}

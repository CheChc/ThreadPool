#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

/* 单次扩容/缩容的步长（线程数） */
#define ADJUST_STEP 2

/* 管理线程的检测间隔（秒） */
#define MANAGE_INTERVAL 2

typedef struct Task {
    void (*function)(void* arg);
    void* arg;
} Task;

/* worker 线程入参：携带池指针与自身在 threadIDs 中的槽位，退出时 O(1) 置零 */
typedef struct WorkerArg {
    ThreadPool* pool;
    pthread_t* slot;
} WorkerArg;

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
    atomic_int busyNum;     /* 忙线程数（原子计数，无需独立锁） */
    int liveNum;            /* 存活线程数 */
    int exitNum;            /* 待退出线程数 */

    int pending;            /* 待处理任务数 = 队列中 + 运行中（受 mutexpool 保护） */
    int producerCount;      /* 正在执行 Add/TryAdd 的调用者数（受 mutexpool 保护） */

    int closed;             /* 1 = threadPoolShutdown 已调用：拒绝新任务 */
    int shutdown;           /* 1 = threadPoolDestroy 已调用：全部线程即将退出 */
    int monotonic;          /* 1 = manageWait 使用 CLOCK_MONOTONIC */

    pthread_mutex_t mutexpool;  /* 保护池状态：队列、线程数、pending、closed/shutdown */
    pthread_cond_t notFull;     /* 队列未满（生产者等待） */
    pthread_cond_t notEmpty;    /* 队列非空（消费者等待） */
    pthread_cond_t manageWait;  /* 管理线程定时等待（destroy 时唤醒，避免干等 2 秒） */
    pthread_cond_t allDone;     /* pending == 0（唤醒 WaitAll） */
    pthread_cond_t noProducer;  /* producerCount == 0（唤醒 Destroy） */
};

/*
 * 当前线程退出：把自己的槽位置 0 后退出。
 * 调用前不得持有任何锁（内部会加锁，避免退出时锁永久不释放）。
 * slot 指向创建时分配的槽位（threadIDs 中的元素），O(1) 置零，无需扫描。
 */
static void threadExit(ThreadPool* pool, pthread_t* slot)
{
    pthread_mutex_lock(&pool->mutexpool);
    *slot = 0;
    pool->liveNum--;
    pthread_mutex_unlock(&pool->mutexpool);

    pthread_exit(NULL);
}

/* 消费者线程：从队列取任务执行 */
static void* worker(void* arg)
{
    WorkerArg* wa = (WorkerArg*)arg;
    ThreadPool* pool = wa->pool;
    pthread_t* selfSlot = wa->slot;
#ifdef __linux__
    prctl(PR_SET_NAME, "tp-worker", 0, 0, 0);
#elif defined(__APPLE__)
    pthread_setname_np("tp-worker");
#endif

    for (;;) {
        pthread_mutex_lock(&pool->mutexpool);

        /*
         * 队列空且未关闭：阻塞等待。
         * exitNum == 0 并入等待条件：空闲 worker 被标记缩容后主动退出，
         * 不再依赖外部 signal 唤醒（避免缩容信号丢失导致线程卡死）。
         */
        while (pool->queueSize == 0 && !pool->shutdown && pool->exitNum == 0)
            pthread_cond_wait(&pool->notEmpty, &pool->mutexpool);

        /* 缩容：队列已空时退出 */
        if (pool->exitNum > 0 && pool->queueSize == 0) {
            pool->exitNum--;
            pthread_mutex_unlock(&pool->mutexpool);
            free(wa);
            threadExit(pool, selfSlot);
        }

        /* 关闭且队列已空：正常退出（队列非空则继续消费完，优雅停机） */
        if (pool->shutdown && pool->queueSize == 0) {
            pthread_mutex_unlock(&pool->mutexpool);
            free(wa);
            threadExit(pool, selfSlot);
        }

        /* 取出队头任务 */
        Task task = pool->taskQ[pool->queueFront];
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
        pool->queueSize--;

        pthread_cond_signal(&pool->notFull);
        pthread_mutex_unlock(&pool->mutexpool);

        /* 执行任务（busyNum 原子计数，不阻塞其他消费者取任务） */
        atomic_fetch_add(&pool->busyNum, 1);

        task.function(task.arg);

        atomic_fetch_sub(&pool->busyNum, 1);

        /* 任务完成会计：pending 归零时广播 allDone，唤醒 WaitAll */
        pthread_mutex_lock(&pool->mutexpool);
        pool->pending--;
        if (pool->pending == 0)
            pthread_cond_broadcast(&pool->allDone);
        pthread_mutex_unlock(&pool->mutexpool);
    }
    return NULL;
}

/* 管理线程：周期性检查负载，动态扩缩容。退出时不清理资源（由 Destroy 统一清理） */
static void* manager(void* arg)
{
    ThreadPool* pool = (ThreadPool*)arg;
#ifdef __linux__
    prctl(PR_SET_NAME, "tp-manager", 0, 0, 0);
#elif defined(__APPLE__)
    pthread_setname_np("tp-manager");
#endif

    clockid_t clk = pool->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME;

    for (;;) {
        /*
         * 定时等待（可被 destroy 提前唤醒），避免 destroy 时干等整个间隔。
         * 使用单调时钟（创建时已通过 condattr 设置），避免系统时间
         * 被 NTP/手动调整时，等待时间意外变长或立即超时。
         */
        struct timespec ts;
        clock_gettime(clk, &ts);
        ts.tv_sec += MANAGE_INTERVAL;

        pthread_mutex_lock(&pool->mutexpool);
        /* 进入等待前先检查关闭标志，避免 destroy 的广播"丢失"导致 join 多等 2 秒 */
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutexpool);
            break;
        }
        int woken = pthread_cond_timedwait(&pool->manageWait, &pool->mutexpool, &ts);
        int shutdown = pool->shutdown;
        int closed = pool->closed;
        int queueSize = pool->queueSize;
        int liveNum = pool->liveNum;
        pthread_mutex_unlock(&pool->mutexpool);

        if (shutdown)
            break;
        if (woken != ETIMEDOUT)
            continue;   /* 非超时唤醒（destroy 广播），shutdown 已检查 */

        int busyNum = atomic_load(&pool->busyNum);

        int idleNum = liveNum - busyNum;    /* 空闲线程数（快照，仅作启发式） */

        /* 扩容：有排队任务、未停收、线程数未达上限 */
        if (!closed && queueSize > idleNum && liveNum < pool->maxNum) {
            pthread_mutex_lock(&pool->mutexpool);
            int toCreate = pool->maxNum - pool->liveNum;
            if (toCreate > ADJUST_STEP)
                toCreate = ADJUST_STEP;
            for (int i = 0, created = 0;
                 i < pool->maxNum && created < toCreate; ++i) {
                if (pool->threadIDs[i] == 0) {
                    WorkerArg* wa = malloc(sizeof(WorkerArg));
                    if (wa == NULL)
                        continue;
                    wa->pool = pool;
                    wa->slot = &pool->threadIDs[i];
                    pthread_t tid;
                    if (pthread_create(&tid, NULL, worker, wa) == 0) {
                        pool->threadIDs[i] = tid;
                        pool->liveNum++;
                        created++;
                    } else {
                        free(wa);
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
    if (min <= 0 || max < min || queueSize <= 0) {
        errno = EINVAL;
        return NULL;
    }

    ThreadPool* pool = calloc(1, sizeof(ThreadPool));
    if (pool == NULL)
        return NULL;

    pool->threadIDs = calloc((size_t)max, sizeof(pthread_t));   /* calloc 保证空槽位为 0 */
    pool->taskQ = calloc((size_t)queueSize, sizeof(Task));
    if (pool->threadIDs == NULL || pool->taskQ == NULL) {
        free(pool->threadIDs);
        free(pool->taskQ);
        free(pool);
        return NULL;
    }

    pool->minNum = min;
    pool->maxNum = max;
    pool->queueCapacity = queueSize;
    atomic_init(&pool->busyNum, 0);

    /* manageWait 优先使用单调时钟；pthread_condattr_setclock 为 Linux 专属，
     * 其他平台（macOS/MinGW）退回默认时钟（CLOCK_REALTIME） */
    pthread_condattr_t cattr;
    int haveAttr = 0;
    int mono = 0;
#ifdef __linux__
    if (pthread_condattr_init(&cattr) == 0) {
        haveAttr = 1;
        mono = (pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) == 0);
    }
#endif
    pool->monotonic = mono;

    /* 分步初始化，失败时精确清理已成功的部分，避免资源泄漏 */
    int have_pool_mutex = 0;
    int have_notfull = 0, have_notempty = 0, have_mgmt = 0;
    int have_alldone = 0, have_noprod = 0;

    if (pthread_mutex_init(&pool->mutexpool, NULL) != 0) goto init_fail;
    have_pool_mutex = 1;
    if (pthread_cond_init(&pool->notFull, NULL) != 0) goto init_fail;
    have_notfull = 1;
    if (pthread_cond_init(&pool->notEmpty, NULL) != 0) goto init_fail;
    have_notempty = 1;
    if (pthread_cond_init(&pool->manageWait, mono ? &cattr : NULL) != 0) goto init_fail;
    have_mgmt = 1;
    if (pthread_cond_init(&pool->allDone, NULL) != 0) goto init_fail;
    have_alldone = 1;
    if (pthread_cond_init(&pool->noProducer, NULL) != 0) goto init_fail;
    have_noprod = 1;
    if (haveAttr)
        pthread_condattr_destroy(&cattr);

    if (pthread_create(&pool->managerID, NULL, manager, pool) != 0) {
        pthread_cond_destroy(&pool->noProducer);
        pthread_cond_destroy(&pool->allDone);
        pthread_cond_destroy(&pool->manageWait);
        pthread_cond_destroy(&pool->notEmpty);
        pthread_cond_destroy(&pool->notFull);
        pthread_mutex_destroy(&pool->mutexpool);
        free(pool->threadIDs);
        free(pool->taskQ);
        free(pool);
        return NULL;
    }

    /* 创建初始 worker（部分失败不致命，管理线程会按需补齐） */
    for (int i = 0; i < min; ++i) {
        WorkerArg* wa = malloc(sizeof(WorkerArg));
        if (wa == NULL)
            break;
        wa->pool = pool;
        wa->slot = &pool->threadIDs[i];
        pthread_t tid;
        if (pthread_create(&tid, NULL, worker, wa) == 0) {
            pool->threadIDs[i] = tid;
            pool->liveNum++;
        } else {
            free(wa);
        }
    }
    return pool;

init_fail:
    if (have_noprod) pthread_cond_destroy(&pool->noProducer);
    if (have_alldone) pthread_cond_destroy(&pool->allDone);
    if (have_mgmt) pthread_cond_destroy(&pool->manageWait);
    if (have_notempty) pthread_cond_destroy(&pool->notEmpty);
    if (have_notfull) pthread_cond_destroy(&pool->notFull);
    if (have_pool_mutex) pthread_mutex_destroy(&pool->mutexpool);
    if (haveAttr) pthread_condattr_destroy(&cattr);
    free(pool->threadIDs);
    free(pool->taskQ);
    free(pool);
    return NULL;
}

int threadPoolAdd(ThreadPool* pool, void (*func)(void*), void* arg)
{
    if (pool == NULL || func == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->mutexpool);
    pool->producerCount++;   /* 告诉 Destroy：有生产者正在提交，先别释放 */

    /* 队列满且未关闭：阻塞等待消费者腾出空间 */
    while (pool->queueSize == pool->queueCapacity &&
           !pool->closed && !pool->shutdown) {
        pthread_cond_wait(&pool->notFull, &pool->mutexpool);
    }

    int rc = -1;
    if (pool->closed || pool->shutdown) {
        errno = EPERM;
    } else {
        pool->taskQ[pool->queueRear].function = func;
        pool->taskQ[pool->queueRear].arg = arg;
        pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
        pool->queueSize++;
        pool->pending++;
        pthread_cond_signal(&pool->notEmpty);
        rc = 0;
    }

    if (--pool->producerCount == 0)
        pthread_cond_broadcast(&pool->noProducer);
    pthread_mutex_unlock(&pool->mutexpool);
    return rc;
}

int threadPoolTryAdd(ThreadPool* pool, void (*func)(void*), void* arg)
{
    if (pool == NULL || func == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->mutexpool);
    pool->producerCount++;

    int rc = -1;
    if (pool->closed || pool->shutdown) {
        errno = EPERM;
    } else if (pool->queueSize == pool->queueCapacity) {
        errno = EBUSY;
    } else {
        pool->taskQ[pool->queueRear].function = func;
        pool->taskQ[pool->queueRear].arg = arg;
        pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
        pool->queueSize++;
        pool->pending++;
        pthread_cond_signal(&pool->notEmpty);
        rc = 0;
    }

    if (--pool->producerCount == 0)
        pthread_cond_broadcast(&pool->noProducer);
    pthread_mutex_unlock(&pool->mutexpool);
    return rc;
}

int threadPoolWaitAll(ThreadPool* pool)
{
    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->mutexpool);
    while (pool->pending > 0 && !pool->shutdown)
        pthread_cond_wait(&pool->allDone, &pool->mutexpool);
    int rc = pool->shutdown ? -1 : 0;
    if (rc != 0)
        errno = EPERM;
    pthread_mutex_unlock(&pool->mutexpool);
    return rc;
}

int threadPoolBusyNum(ThreadPool* pool)
{
    if (pool == NULL)
        return -1;

    return atomic_load(&pool->busyNum);
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

int threadPoolPendingCount(ThreadPool* pool)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexpool);
    int pending = pool->pending;
    pthread_mutex_unlock(&pool->mutexpool);
    return pending;
}

int threadPoolShutdown(ThreadPool* pool)
{
    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->mutexpool);
    if (pool->shutdown) {               /* 已在销毁 */
        pthread_mutex_unlock(&pool->mutexpool);
        errno = EPERM;
        return -1;
    }
    pool->closed = 1;
    pthread_mutex_unlock(&pool->mutexpool);
    return 0;
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
    pool->closed = 1;
    pthread_cond_broadcast(&pool->notEmpty);    /* 唤醒 worker */
    pthread_cond_broadcast(&pool->notFull);     /* 唤醒阻塞在 Add 的生产者 */
    pthread_cond_broadcast(&pool->manageWait);  /* 唤醒 manager，立即退出 */
    pthread_cond_broadcast(&pool->allDone);     /* 唤醒 WaitAll */

    /*
     * 2. 等待所有正在 Add/TryAdd 的调用者离开。
     *    修复：原实现在这里直接去 join 线程并 free(pool)，如果某个生产者
     *    阻塞在"队列满"的 cond_wait 上且唤醒被延迟，它会在 pool 释放后
     *    才醒来，触发 use-after-free。现在用 producerCount 计数保证
     *    Destroy 返回前没有任何生产者还停留在 Add/TryAdd 内部。
     */
    while (pool->producerCount > 0)
        pthread_cond_wait(&pool->noProducer, &pool->mutexpool);
    pthread_mutex_unlock(&pool->mutexpool);

    /* 3. 回收管理线程（它自己退出，不清理资源） */
    pthread_join(pool->managerID, NULL);

    /*
     * 4. 回收所有 worker（优雅停机：队列中剩余任务会被消费完）。
     *    锁内快照线程槽位后再 join：原实现无锁读 threadIDs，与 threadExit
     *    加锁置零形成数据竞争（C11 UB，pthread_t 非标量平台上可能读到撕裂值）。
     *    join 必须在锁外进行，否则 worker 退出时需要拿同一把锁而卡死。
     *    快照用定长栈数组（maxNum 固定），避免 OOM 时回退到无锁读。
     */
    pthread_t joinList[pool->maxNum];
    int n = 0;
    pthread_mutex_lock(&pool->mutexpool);
    for (int i = 0; i < pool->maxNum; ++i)
        if (pool->threadIDs[i] != 0)
            joinList[n++] = pool->threadIDs[i];
    pthread_mutex_unlock(&pool->mutexpool);

    for (int j = 0; j < n; ++j)
        pthread_join(joinList[j], NULL);

    /* 5. 释放资源（此时已无任何线程访问 pool） */
    free(pool->taskQ);
    free(pool->threadIDs);
    pthread_mutex_destroy(&pool->mutexpool);
    pthread_cond_destroy(&pool->notFull);
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->manageWait);
    pthread_cond_destroy(&pool->allDone);
    pthread_cond_destroy(&pool->noProducer);
    free(pool);
    return 0;
}

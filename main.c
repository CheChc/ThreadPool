#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

#include "threadpool.h"

/* 任务函数：arg 指向 malloc 出来的 int，由任务函数自己释放
 * （线程池不负责释放 arg，这是 API 约定） */
static void taskFunc(void* arg)
{
    int num = *(int*)arg;
    printf("thread %lu working, number = %d\n",
           (unsigned long)pthread_self(), num);
    free(arg);
    usleep(200000);     /* 模拟耗时任务，制造排队让管理线程扩容 */
}

/* 轻量任务：原子计数，用于 TryAdd / WaitAll 演示 */
static void quickFunc(void* arg)
{
    atomic_fetch_add((atomic_int*)arg, 1);
}

int main(void)
{
    /* ---------- 场景 1：批量提交 + 动态扩缩容 ---------- */
    printf("== 场景 1：批量任务 + 动态扩缩容 ==\n");
    ThreadPool* pool = threadPoolCreate(2, 8, 64);
    if (pool == NULL) {
        fprintf(stderr, "threadPoolCreate failed\n");
        return 1;
    }

    /* 提交 100 个任务：队列容量只有 64，Add 会阻塞等待消费 */
    for (int i = 0; i < 100; ++i) {
        int* num = malloc(sizeof(int));
        if (num == NULL)
            break;
        *num = i + 100;
        if (threadPoolAdd(pool, taskFunc, num) != 0) {
            free(num);
            fprintf(stderr, "add task %d failed (pool closing?)\n", i);
            break;
        }
    }

    /* 给管理线程几秒时间观察负载并扩缩容 */
    for (int i = 0; i < 5; ++i) {
        sleep(2);
        printf("alive=%d busy=%d pending=%d\n",
               threadPoolAliveNum(pool), threadPoolBusyNum(pool),
               threadPoolPendingCount(pool));
    }

    /* ---------- 场景 2：WaitAll 同步屏障 ---------- */
    printf("== 场景 2：threadPoolWaitAll ==\n");
    atomic_int counter = 0;
    for (int i = 0; i < 1000; ++i)
        threadPoolAdd(pool, quickFunc, &counter);
    threadPoolWaitAll(pool);        /* 阻塞直到 1000 个任务全部完成 */
    printf("waitAll done: counter=%d (应为 1000)\n",
           atomic_load(&counter));

    /* ---------- 场景 3：TryAdd 非阻塞提交 ---------- */
    printf("== 场景 3：threadPoolTryAdd ==\n");
    ThreadPool* tiny = threadPoolCreate(1, 1, 4);
    if (tiny == NULL)
        return 1;
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 10; ++i) {
        if (threadPoolTryAdd(tiny, quickFunc, &counter) == 0)
            accepted++;
        else
            rejected++;     /* 队列容量只有 4，必然有失败 */
    }
    printf("tryAdd: accepted=%d rejected=%d (errno=%s)\n",
           accepted, rejected, strerror(errno));
    threadPoolWaitAll(tiny);
    threadPoolDestroy(tiny);

    /* ---------- 场景 4：Shutdown 优雅停收 ---------- */
    printf("== 场景 4：threadPoolShutdown ==\n");
    for (int i = 0; i < 10; ++i)
        threadPoolAdd(pool, quickFunc, &counter);
    threadPoolShutdown(pool);       /* 之后不再接受新任务 */
    int* num = malloc(sizeof(int));
    *num = 0;
    if (threadPoolAdd(pool, taskFunc, num) != 0) {
        free(num);
        printf("add after shutdown rejected, errno=%s\n", strerror(errno));
    }
    threadPoolWaitAll(pool);        /* 已提交的任务仍会全部执行完 */
    printf("shutdown drain done: counter=%d\n", atomic_load(&counter));

    if (threadPoolDestroy(pool) != 0) {
        fprintf(stderr, "destroy failed\n");
        return 1;
    }
    printf("thread pool destroyed\n");
    return 0;
}

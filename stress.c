/*
 * 压力测试：覆盖线程池最容易出问题的场景
 *   S1 批量任务 + 销毁（正常流程，跑多次）
 *   S2 小池大负载（扩容极限）
 *   S3 提交后立即销毁（worker 还在跑就销毁）
 *   S4 反复创建/销毁（资源泄漏检测）
 *   S5 生产者线程持续 Add 与主线程 Destroy 并发（关闭竞态 / UAF）
 *   S6 多生产者 + WaitAll 同步屏障（任务数精确校验）
 *   S7 TryAdd 非阻塞提交（队列满拒绝 + 已接受任务全部执行）
 *   S8 Shutdown 优雅停收（拒绝新任务 + 存量任务排空）
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

#include "threadpool.h"

static atomic_int g_tasks_done = 0;

static void taskFunc(void* arg)
{
    int n = *(int*)arg;
    free(arg);
    if (n % 3 == 0)
        usleep(1000);   /* 部分任务稍慢 */
    atomic_fetch_add(&g_tasks_done, 1);
}

static void noopFunc(void* arg)
{
    (void)arg;
}

static void s1_batch(void)
{
    ThreadPool* pool = threadPoolCreate(2, 8, 16);
    if (!pool) { fprintf(stderr, "S1 create fail\n"); exit(1); }
    for (int i = 0; i < 200; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        if (threadPoolAdd(pool, taskFunc, p) != 0) { free(p); break; }
    }
    /* 不等任务跑完就销毁（销毁必须等队列消费完） */
    threadPoolDestroy(pool);
}

static void s2_small_pool_big_load(void)
{
    ThreadPool* pool = threadPoolCreate(1, 3, 4);
    if (!pool) { fprintf(stderr, "S2 create fail\n"); exit(1); }
    for (int i = 0; i < 500; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        if (threadPoolAdd(pool, taskFunc, p) != 0) { free(p); break; }
    }
    threadPoolDestroy(pool);
}

static void s3_destroy_immediately(void)
{
    ThreadPool* pool = threadPoolCreate(4, 4, 8);
    if (!pool) { fprintf(stderr, "S3 create fail\n"); exit(1); }
    for (int i = 0; i < 8; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        threadPoolAdd(pool, taskFunc, p);
    }
    threadPoolDestroy(pool);    /* 任务可能正在执行 */
}

static void s4_create_destroy_loop(void)
{
    for (int i = 0; i < 100; ++i) {
        ThreadPool* pool = threadPoolCreate(2, 5, 8);
        if (!pool) { fprintf(stderr, "S4 create fail at %d\n", i); exit(1); }
        for (int j = 0; j < 10; ++j) {
            int* p = malloc(sizeof(int));
            *p = j;
            threadPoolAdd(pool, taskFunc, p);
        }
        threadPoolDestroy(pool);
    }
}

static void* producer(void* arg)
{
    ThreadPool* pool = (ThreadPool*)arg;
    for (int i = 0; i < 1000; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        if (threadPoolAdd(pool, taskFunc, p) != 0) {
            free(p);
            break;      /* 池已关闭，正常 */
        }
        if (i % 10 == 0)
            usleep(50);
    }
    return NULL;
}

static void s5_concurrent_add_destroy(void)
{
    for (int round = 0; round < 20; ++round) {
        ThreadPool* pool = threadPoolCreate(2, 6, 16);
        if (!pool) { fprintf(stderr, "S5 create fail\n"); exit(1); }
        pthread_t t;
        pthread_create(&t, NULL, producer, pool);
        usleep(200 + round * 37);   /* 随机时机销毁 */
        threadPoolDestroy(pool);
        pthread_join(t, NULL);
    }
}

/* S6：多生产者 + WaitAll。每个生产者提交 N 个任务；WaitAll 返回后，
 * 任务计数必须精确等于提交总数（证明没有任务丢失、没有重复执行）。 */
#define S6_PRODUCERS 4
#define S6_PER_PRODUCER 250

typedef struct {
    ThreadPool* pool;
    int id;
} S6Arg;

static void* s6_producer(void* arg)
{
    S6Arg* a = (S6Arg*)arg;
    (void)a->id;
    for (int i = 0; i < S6_PER_PRODUCER; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        threadPoolAdd(a->pool, taskFunc, p);
    }
    return NULL;
}

static void s6_waitall(void)
{
    int before = atomic_load(&g_tasks_done);
    ThreadPool* pool = threadPoolCreate(2, 6, 16);
    if (!pool) { fprintf(stderr, "S6 create fail\n"); exit(1); }

    pthread_t tids[S6_PRODUCERS];
    S6Arg args[S6_PRODUCERS];
    for (int i = 0; i < S6_PRODUCERS; ++i) {
        args[i].pool = pool;
        args[i].id = i;
        pthread_create(&tids[i], NULL, s6_producer, &args[i]);
    }
    for (int i = 0; i < S6_PRODUCERS; ++i)
        pthread_join(tids[i], NULL);

    if (threadPoolWaitAll(pool) != 0) {
        fprintf(stderr, "S6 waitAll failed\n");
        exit(1);
    }
    int done = atomic_load(&g_tasks_done) - before;
    int expect = S6_PRODUCERS * S6_PER_PRODUCER;
    if (done != expect) {
        fprintf(stderr, "S6 FAIL: done=%d expect=%d\n", done, expect);
        exit(1);
    }
    threadPoolDestroy(pool);
}

/* S7：TryAdd。容量为 4 的小池塞 10 个任务：
 * 恰好 4 个被接受，其余拒绝（errno=EBUSY），且被接受的任务全部执行完毕。 */
static void s7_tryadd_overflow(void)
{
    int before = atomic_load(&g_tasks_done);
    ThreadPool* pool = threadPoolCreate(1, 1, 4);
    if (!pool) { fprintf(stderr, "S7 create fail\n"); exit(1); }
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 10; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        if (threadPoolTryAdd(pool, taskFunc, p) == 0) {
            accepted++;
        } else {
            rejected++;
            free(p);
            if (errno != EBUSY) {
                fprintf(stderr, "S7 FAIL: errno=%d expect EBUSY(%d)\n",
                        errno, EBUSY);
                exit(1);
            }
        }
    }
    if (accepted != 4 || rejected != 6) {
        fprintf(stderr, "S7 FAIL: accepted=%d rejected=%d\n",
                accepted, rejected);
        exit(1);
    }
    threadPoolWaitAll(pool);
    int done = atomic_load(&g_tasks_done) - before;
    if (done != 4) {
        fprintf(stderr, "S7 FAIL: done=%d expect 4\n", done);
        exit(1);
    }
    threadPoolDestroy(pool);
}

/* S8：Shutdown 优雅停收。shutdown 后 Add/TryAdd 必须被拒绝，
 * 已提交任务必须全部执行完，WaitAll 正常返回。 */
static void s8_shutdown_drain(void)
{
    int before = atomic_load(&g_tasks_done);
    ThreadPool* pool = threadPoolCreate(2, 4, 16);
    if (!pool) { fprintf(stderr, "S8 create fail\n"); exit(1); }
    for (int i = 0; i < 50; ++i) {
        int* p = malloc(sizeof(int));
        *p = i;
        threadPoolAdd(pool, taskFunc, p);
    }
    if (threadPoolShutdown(pool) != 0) {
        fprintf(stderr, "S8 shutdown fail\n");
        exit(1);
    }
    if (threadPoolAdd(pool, noopFunc, NULL) == 0) {
        fprintf(stderr, "S8 FAIL: add after shutdown accepted\n");
        exit(1);
    }
    if (threadPoolTryAdd(pool, noopFunc, NULL) == 0) {
        fprintf(stderr, "S8 FAIL: tryAdd after shutdown accepted\n");
        exit(1);
    }
    if (threadPoolWaitAll(pool) != 0) {
        fprintf(stderr, "S8 FAIL: waitAll after shutdown\n");
        exit(1);
    }
    int done = atomic_load(&g_tasks_done) - before;
    if (done != 50) {
        fprintf(stderr, "S8 FAIL: done=%d expect 50\n", done);
        exit(1);
    }
    if (threadPoolDestroy(pool) != 0) {
        fprintf(stderr, "S8 FAIL: destroy\n");
        exit(1);
    }
}

int main(void)
{
    s1_batch();
    printf("S1 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s2_small_pool_big_load();
    printf("S2 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s3_destroy_immediately();
    printf("S3 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s4_create_destroy_loop();
    printf("S4 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s5_concurrent_add_destroy();
    printf("S5 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s6_waitall();
    printf("S6 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s7_tryadd_overflow();
    printf("S7 ok (done=%d)\n", atomic_load(&g_tasks_done));
    s8_shutdown_drain();
    printf("S8 ok (done=%d)\n", atomic_load(&g_tasks_done));
    printf("ALL STRESS TESTS PASSED\n");
    return 0;
}

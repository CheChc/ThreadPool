/*
 * 压力测试：覆盖线程池最容易出问题的场景
 *   S1 批量任务 + 销毁（正常流程，跑多次）
 *   S2 小池大负载（扩容极限）
 *   S3 提交后立即销毁（worker 还在跑就销毁）
 *   S4 反复创建/销毁（资源泄漏检测）
 *   S5 生产者线程持续 Add 与主线程 Destroy 并发（关闭竞态）
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "threadpool.h"

static int g_tasks_done = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void taskFunc(void* arg)
{
    int n = *(int*)arg;
    free(arg);
    if (n % 3 == 0)
        usleep(1000);   /* 部分任务稍慢 */
    pthread_mutex_lock(&g_lock);
    g_tasks_done++;
    pthread_mutex_unlock(&g_lock);
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

int main(void)
{
    s1_batch();
    printf("S1 ok (done=%d)\n", g_tasks_done);
    s2_small_pool_big_load();
    printf("S2 ok (done=%d)\n", g_tasks_done);
    s3_destroy_immediately();
    printf("S3 ok (done=%d)\n", g_tasks_done);
    s4_create_destroy_loop();
    printf("S4 ok (done=%d)\n", g_tasks_done);
    s5_concurrent_add_destroy();
    printf("S5 ok (done=%d)\n", g_tasks_done);
    printf("ALL STRESS TESTS PASSED\n");
    return 0;
}

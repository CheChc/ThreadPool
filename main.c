#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "threadpool.h"

/*
 * 任务函数：arg 指向 malloc 出来的 int，由任务函数自己释放
 * （线程池不负责释放 arg，这是 API 约定）
 */
static void taskFunc(void* arg)
{
    int num = *(int*)arg;
    printf("thread %lu working, number = %d\n", (unsigned long)pthread_self(), num);
    free(arg);
    usleep(200000);     /* 模拟耗时任务，制造排队让管理线程扩容 */
}

int main(void)
{
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
        printf("alive=%d busy=%d\n", threadPoolAliveNum(pool), threadPoolBusyNum(pool));
    }

    if (threadPoolDestroy(pool) != 0) {
        fprintf(stderr, "destroy failed\n");
        return 1;
    }
    printf("thread pool destroyed\n");
    return 0;
}

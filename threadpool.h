#ifndef THREADPOOL_H
#define THREADPOOL_H

/*
 * 基于 Linux pthread 的线程池
 *
 * 特性：
 *   - 环形任务队列，生产者-消费者模型
 *   - 管理线程周期性检测负载，动态扩缩容
 *   - 优雅销毁：停止接收新任务 -> 执行完队列剩余任务 -> 回收全部线程 -> 释放资源
 *
 * 线程安全：所有公开 API 均可从任意线程调用。
 */

#include <pthread.h>   /* pthread_t */

typedef struct ThreadPool ThreadPool;

/*
 * 创建线程池。
 *   min       初始线程数（也是缩容下限）
 *   max       最大线程数（扩容上限）
 *   queueSize 任务队列容量（环形缓冲）
 * 成功返回池指针；参数非法或资源不足返回 NULL。
 */
ThreadPool* threadPoolCreate(int min, int max, int queueSize);

/*
 * 销毁线程池。阻塞直到所有线程退出、资源释放完毕。
 * 返回 0 成功；-1 参数非法或重复销毁。
 */
int threadPoolDestroy(ThreadPool* pool);

/*
 * 提交任务。func 为任务函数，arg 为透传给 func 的参数。
 * 队列满时阻塞等待；线程池已关闭时立即返回 -1。
 * 注意：arg 指向的内存由任务函数自行管理，线程池不会释放它。
 * 返回 0 成功；-1 失败（池已关闭或参数非法）。
 */
int threadPoolAdd(ThreadPool* pool, void (*func)(void*), void* arg);

/* 查询当前忙线程数（正在执行任务的 worker 数量），失败返回 -1 */
int threadPoolBusyNum(ThreadPool* pool);

/* 查询当前存活线程数，失败返回 -1 */
int threadPoolAliveNum(ThreadPool* pool);

/* 兼容旧版拼写（threadPoolDestory -> threadPoolDestroy） */
#define threadPoolDestory threadPoolDestroy

#endif /* THREADPOOL_H */

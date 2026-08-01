#ifndef THREADPOOL_H
#define THREADPOOL_H

/*
 * 基于 pthread 的 C11 线程池（Linux / macOS / MinGW）
 *
 * 特性：
 *   - 环形任务队列，生产者-消费者模型
 *   - 管理线程周期性检测负载，动态扩缩容（每次 ±2 个线程）
 *   - 优雅销毁：停止接收新任务 -> 执行完队列剩余任务 -> 回收全部线程 -> 释放资源
 *   - 优雅停收（threadPoolShutdown）：拒绝新任务，但让已提交任务全部执行完
 *   - 非阻塞提交（threadPoolTryAdd）与同步屏障（threadPoolWaitAll）
 *   - 销毁与"正在阻塞提交"的生产者无竞态（不会 use-after-free）
 *
 * 线程安全：除 threadPoolDestroy 外所有公开 API 均可从任意线程调用。
 * Destroy 只能调用一次；调用方需保证 Destroy 返回后不再调用任何 API。
 */

#include <pthread.h>

typedef struct ThreadPool ThreadPool;

/*
 * 创建线程池。
 *   min       初始线程数（也是缩容下限，必须 > 0）
 *   max       最大线程数（扩容上限，必须 >= min）
 *   queueSize 任务队列容量（环形缓冲，必须 > 0）
 * 成功返回池指针；参数非法（errno=EINVAL）或资源不足返回 NULL。
 */
ThreadPool* threadPoolCreate(int min, int max, int queueSize);

/*
 * 优雅停收：不再接受新任务，已提交任务继续执行完。
 * 之后只允许调用查询接口 / threadPoolWaitAll / threadPoolDestroy。
 * 返回 0 成功；-1 参数非法或线程池已在销毁（errno=EPERM）。
 */
int threadPoolShutdown(ThreadPool* pool);

/*
 * 销毁线程池。阻塞直到所有线程退出、资源释放完毕。
 * 会先优雅排空队列中的剩余任务，并等待所有正在 Add/TryAdd
 * 的生产者离开后才释放资源，因此不会 use-after-free。
 * 返回 0 成功；-1 参数非法或重复销毁。
 */
int threadPoolDestroy(ThreadPool* pool);

/*
 * 提交任务（阻塞式）。func 为任务函数，arg 为透传给 func 的参数。
 * 队列满时阻塞等待；线程池已关闭（Shutdown/Destroy 之后）立即返回 -1。
 * 注意：arg 指向的内存由任务函数自行管理，线程池不会释放它。
 * 返回 0 成功；-1 失败（池已关闭 errno=EPERM，或参数非法 errno=EINVAL）。
 */
int threadPoolAdd(ThreadPool* pool, void (*func)(void*), void* arg);

/*
 * 提交任务（非阻塞式）。队列满立即返回，不会等待。
 * 返回 0 成功；-1 失败：
 *   - 队列满        errno = EBUSY
 *   - 线程池已关闭  errno = EPERM
 *   - 参数非法      errno = EINVAL
 */
int threadPoolTryAdd(ThreadPool* pool, void (*func)(void*), void* arg);

/*
 * 阻塞直到所有已提交任务执行完毕（含正在运行的）。
 * 正常时返回 0；线程池正在销毁时返回 -1（errno=EPERM）。
 * 注意：不要与 threadPoolDestroy 并发调用；Destroy 返回后不能再调用本函数。
 */
int threadPoolWaitAll(ThreadPool* pool);

/* 查询当前忙线程数（正在执行任务的 worker 数量），失败返回 -1 */
int threadPoolBusyNum(ThreadPool* pool);

/* 查询当前存活线程数，失败返回 -1 */
int threadPoolAliveNum(ThreadPool* pool);

/* 查询待处理任务数（队列中 + 运行中），失败返回 -1 */
int threadPoolPendingCount(ThreadPool* pool);

/* 兼容旧版拼写（threadPoolDestory -> threadPoolDestroy） */
#define threadPoolDestory threadPoolDestroy

#endif /* THREADPOOL_H */

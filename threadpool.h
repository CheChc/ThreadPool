#ifndef _THREADPOOL_H
#define _THREADPOOL_Htypedef 

typedef struct ThreadPool ThreadPool;

ThreadPool* threadPoolCreate (int min,int max,int queueSize);//创建线程池

//销毁线程池
int threadPoolDestory(ThreadPool* pool);

//给线程池添加任务
void threadPoolAdd(ThreadPool* pool,void (*func)(void*),void* arg);

//获取工作的线程的个数
int threadPolBusyNum(ThreadPool* pool);

//获取存活线程的个数
int threadPolAliveNum(ThreadPool* pool);

//消费者线程
void* worker(void *arg);

//管理者线程
void* manager(void* arg);

//单个线程退出
void threadExit(ThreadPool* pool);
#endif
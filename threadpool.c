#include "threadpool.h"
#include<pthread.h>
#include<stdlib.h>
#include<string.h>
#include<stdio.h>
#include <unistd.h>

const int NUMBER = 2;
// Task
typedef struct Task
{
    void (*function)(void* arg);
    void* arg;
}Task;
struct ThreadPool
{
    Task* taskQ;
    int queueCapacity;//容量
    int queueSize;//当前个数
    int queueFront;//队头
    int queueRear;//队尾

    pthread_t managerID;//管理者线程ID
    pthread_t* threadIDs;//工作的线程ID
    int minNum;//最小线程数
    int maxNum;//最大线程数
    int busyNum;//忙的线程格式
    int liveNum;//存货线程个数
    int exitNum;//要销毁的线程个数
    pthread_mutex_t mutexpool;//锁整个线程池
    pthread_mutex_t mutexbusy;//锁busyNum
    pthread_cond_t notFull;//任务队列是不是满了
    pthread_cond_t notEmpty;//任务队列是不是空了 
    int shutdown;//是否需要销毁线程 1为关闭，0为不关闭
};
ThreadPool* threadPoolCreate (int min,int max,int queueSize)
{
    ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
    do
    {
        if(pool == NULL)
    {
        printf("malloc threadpoll fail...\n");
        break;
    }
    pool->threadIDs =(pthread_t*)malloc(sizeof(pthread_t)*max);
    if(pool->threadIDs == NULL)
    {
        printf("malloc threadids fail...");
        break;
    }
    memset(pool->threadIDs,0,sizeof(pthread_t )*max);
    pool -> maxNum=max;
    pool -> minNum=min;
    pool -> busyNum=0;
    pool -> liveNum=min;
    pool -> exitNum=0;
    if(pthread_mutex_init(&pool -> mutexpool,NULL)!=0 || 
    pthread_mutex_init(&pool -> mutexbusy,NULL)!=0||
    pthread_cond_init(&pool -> notFull,NULL)!=0||
    pthread_cond_init(&pool -> notEmpty,NULL)!=0)
    {
        printf("mutex or condition init fail...");
        break;
    }//四个锁的检测
    pool -> taskQ = (Task*)malloc(sizeof(Task)*queueSize);
    pool -> queueCapacity=queueSize;
    pool -> queueSize=0;
    pool -> queueFront=0;
    pool -> queueRear=0;

    pool -> shutdown=0;

//创建线程
    pthread_create(&pool->managerID, NULL, manager, pool);
    for (int i = 0; i < min;i++)
    {
        pthread_create(&pool->threadIDs[i], NULL, worker,pool);
    }
    return pool;
    } while (0);
    if(pool&&pool->threadIDs)
        free(pool->threadIDs);
    if(pool&&pool->taskQ)
        free(pool->taskQ);
    if(pool)
        free (pool);
    
    return NULL;
}
void* worker(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    while(1)
    {
        pthread_mutex_lock(&pool->mutexpool);
        while(pool->queueSize==0&&!pool->shutdown)
        {
            //任务队列空且未关闭：阻塞
            pthread_cond_wait(&pool->notEmpty, &pool->mutexpool);
            if(pool->exitNum>0)
            {
                pool->exitNum--;
                pthread_mutex_unlock(&pool->mutexpool);
                threadExit(pool);
            }
        }
        if(pool->shutdown)//若线程池关闭，打开锁防止死锁，退出
        {
            pthread_mutex_unlock(&pool->mutexpool);
            threadExit(pool);
        }

        //从任务队列中取出一个任务
        Task task;
        task.function = pool->taskQ[pool->queueFront].function;
        task.arg = pool->taskQ[pool->queueFront].arg;
        //移动头节点
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;//数组转化为循环队列，通过求余可以实现循环功能
        pool->queueSize--;//容量减一

        pthread_cond_signal(&pool->notFull);//生产者的唤醒，和消费者对应

        pthread_mutex_unlock(&pool->mutexpool);//执行完毕，打开锁
        printf("thread %ld start working...",pthread_self());

        pthread_mutex_lock(&pool->mutexbusy);
        pool->busyNum++;
        pthread_mutex_unlock(&pool->mutexbusy);

        task.function(task.arg);
        printf("thread %ld end working....",pthread_self());
        free(task.arg);
        task.arg=NULL;
        pthread_mutex_lock(&pool->mutexbusy);
        pool->busyNum--;
        pthread_mutex_unlock(&pool->mutexbusy);
    }
    return NULL;
}
void* manager(void* arg)
{
    ThreadPool *pool=(ThreadPool*)arg;
    while (!pool->shutdown)
    {
        sleep(3);//每三秒检测一次
        //取出线程池中任务的数量和当前任务
        pthread_mutex_lock(&pool->mutexpool);
        int queueSize=pool->queueSize;
        int liveNum=pool->liveNum;
        pthread_mutex_unlock(&pool->mutexpool);
        //取出忙的线程
        pthread_mutex_lock(&pool->mutexbusy);//用专用的锁，提高工作效率，不需要锁整个线程池
        int busyNum=pool->busyNum;
        pthread_mutex_unlock(&pool->mutexbusy);
        //添加线程
        //任务个数＞存活线程数 &&存活线程数＜最大线程数
        if(queueSize>liveNum&&liveNum<pool->maxNum)
        {
            int counter=0;
            pthread_mutex_lock(&pool->mutexpool);
            for(int i=0;i<pool->maxNum&&counter<NUMBER&&pool->liveNum<pool->maxNum;i++)
            {   
                if(pool->threadIDs[i]==0)
                pthread_create(pool->threadIDs[i],NULL,worker,pool);
                counter++;
                pool->liveNum++;
            }
            pthread_mutex_unlock(&pool->mutexpool);
        }
        //销毁线程
        //忙的线程*2＜存活线程数&&存活线程数＞最小线程数
        if(queueSize*2<liveNum&&liveNum>pool->minNum)
        {
            pthread_mutex_lock(&pool->mutexpool);
            pool->exitNum=NUMBER;
            pthread_mutex_unlock(&pool->mutexpool);
            for(int i=0;i<NUMBER;i++)
            {
                pthread_cond_signal(&pool->notEmpty);
            }
        }
    }
    if(pool->taskQ)
    {
        free(pool->taskQ);
    }
    if(pool->threadIDs)
    {
        free(pool->threadIDs);
    }
    

    pthread_mutex_destroy(&pool->mutexpool);
    pthread_mutex_destroy(&pool->mutexbusy);
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->notFull);
    free(pool);
    pool=NULL;
    return NULL;
}
void threadPoolAdd(ThreadPool* pool,void (*func)(void*), void* arg)
{
    pthread_mutex_lock(&pool->mutexpool);
    while(pool->queueSize == pool->queueCapacity&&!pool->shutdown)
    {
        //阻塞生产者线程
        pthread_cond_wait(&pool->notFull,&pool->mutexpool);
    }
    //如果关闭，则解锁并退出
    if(pool->shutdown)
    {
        pthread_mutex_unlock(&pool->mutexpool);
        return;
    }
    //添加任务
    pool->taskQ[pool->queueRear].function=func;
    pool->taskQ[pool->queueRear].arg=arg;
    pool->queueRear=(pool->queueRear+1)%pool->queueCapacity;
    pool->queueSize++;

    pthread_cond_signal(&pool->notEmpty);
    pthread_mutex_unlock(&pool->mutexpool);
	return ;
}
int threadPolBusyNum(ThreadPool* pool)
{
    pthread_mutex_lock(&pool->busyNum);
    int busyNum=pool->busyNum;
    pthread_mutex_unlock(&pool->busyNum);
    return busyNum;
}
int threadPolAliveNum(ThreadPool* pool)
{
    pthread_mutex_lock(&pool->mutexpool);
    int aliveNum=pool->liveNum;
    pthread_mutex_unlock(&pool->mutexpool);
    return aliveNum;
}
int threadPoolDestory(ThreadPool* pool)
{
    if(pool == NULL)
    return -1;
    //第一步，关掉线程池
    pool->shutdown=1;
    //阻塞回收管理者
    pthread_join(pool->managerID,NULL);
    //唤醒阻塞的消费者
    for(int i=0;i<pool->liveNum;i++)
    {
        pthread_cond_signal(&pool->notEmpty);
    }
	return 1;
}
void threadExit(ThreadPool* pool)
{
    pthread_t tid = pthread_self();
    for (int i = 0; i < pool->maxNum; ++i)
    {
        if (pool->threadIDs[i] == tid)
        {
            pool->threadIDs[i] = 0;
            printf("threadExit() called, %ld exiting...\n", tid);
            break;
        }
    }
    pthread_exit(NULL);
}
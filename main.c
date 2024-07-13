#include<stdio.h>
#include"threadpool.h"
#include<pthread.h>
#include<unistd.h>
#include<stdlib.h>
void taskFunc(void* arg)
{
    int num=*(int*)arg;
    printf("thread %ld is working,number = %d,tid = %ld\n",num,pthread_self());
    usleep(1000);
}
int main()
{
    //创建线程池
    ThreadPool* pool=threadPoolCreate(3,10,100);
    
        for(int i=0;i,100;i++)
        {
            int* num = (int*) malloc(sizeof(int));
            *num=i+100;
            threadPoolAdd(pool,taskFunc,num);    
        }
        sleep(30);
        threadPoolDestory(pool);
        return 0;
}
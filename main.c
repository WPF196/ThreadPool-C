#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include "threadpool.h"


// 任务事件
void taskFunc(void *arg) {
    int num = *(int*)arg;
    printf("thread %ld is working, number = %d\n", 
        pthread_self(), num);
    usleep(1000);
}

int main(){
    // 创建线程池
    ThreadPool* pool = threadPoolCreate(3, 10, 100);
    // 调用线程池api，将所有的任务加入线程池中的任务队列
    for (int i = 0; i < 100; ++i) {
        int* num = (int*)malloc(sizeof(int));
        *num = i + 100;
        threadPoolAdd(pool, taskFunc, num);
    }
    sleep(30);  // 让主线程休眠一段时间，等待子线程任务处理完毕
    threadPoolDestroy(pool);
    return 0;
}
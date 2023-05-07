#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "threadpool.h"

const int NUMBER = 2;

// 任务结构体
typedef struct Task {
	void (*function)(void* arg);	// 函数指针，即任务函数的地址（泛型）
	void* arg;						// 任务函数参数的地址
}Task;

// 线程池结构体
struct ThreadPool{
	// 任务队列
	Task* taskQ;
	int queueCapacity;		// 容量
	int queueSize;			// 当前任务个数
	int queueFront;			// 队头
	int queueRear;			// 队尾

	// 线程相关参数
	pthread_t managerID;		// 管理员线程
	pthread_t* threadIDs;		// 工作的线程
	int minNum;					// 最小线程数量
	int maxNum;					// 最大线程数量
	int busyNum;				// 工作中的线程的个数
	int liveNum;				// 存活的线程的个数
	int exitNum;				// 要销毁的线程个数
	pthread_mutex_t mutexPool;  // 互斥锁，锁整个的线程池
	pthread_mutex_t mutexBusy;  // 互斥锁，锁busyNum变量
	pthread_cond_t notFull;     // 条件变量，任务队列是否已满
	pthread_cond_t notEmpty;    // 条件变量，任务队列是否为空

	int shutdown;				// 是不是要销毁线程池, 销毁为1, 不销毁为0
};

/* 创建线程池 */
ThreadPool* threadPoolCreate(int min, int max, int queueSize){
    ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
    
    // 放在do里，方便调用break。如果直接return，终止函数调用，不方便释放分配的内存
    do{
        if (pool == NULL){
            printf("malloc threadpool fail...\n");
            break;
        }

        // 存储工作的线程地址（最大数量）
        pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * max);
        if (pool->threadIDs == NULL){
            printf("malloc threadIDs fail...\n");
            break;
        }
        memset(pool->threadIDs, 0, sizeof(pthread_t) * max);
        pool->minNum = min;
        pool->maxNum = max;
        pool->busyNum = 0;
        pool->liveNum = min;    // 和最小个数相等
        pool->exitNum = 0;

        // 初始化互斥量和条件变量
        if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 ||
            pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
            pthread_cond_init(&pool->notEmpty, NULL) != 0 ||
            pthread_cond_init(&pool->notFull, NULL) != 0){
            printf("mutex or condition init fail...\n");
            break;
        }

        // 任务队列，最多queueSize个任务
        pool->taskQ = (Task*)malloc(sizeof(Task) * queueSize);
        pool->queueCapacity = queueSize;
        pool->queueSize = 0;
        pool->queueFront = 0;
        pool->queueRear = 0;

        pool->shutdown = 0;

        // 管理员线程
        pthread_create(&pool->managerID, NULL, manager, pool);
        // 工作的线程（先创建min个）
        for (int i = 0; i < min; ++i){
            pthread_create(&pool->threadIDs[i], NULL, worker, pool);
        }
        return pool;    // 操作成功返回线程池
    } while (0);

    // 释放资源
    if (pool && pool->threadIDs) free(pool->threadIDs);
    if (pool && pool->taskQ) free(pool->taskQ);
    if (pool) free(pool);

    return NULL;
}

/* 销毁线程池 */
int threadPoolDestroy(ThreadPool* pool){
    if (pool == NULL)   return -1;

    // 关闭线程池
    pool->shutdown = 1;
    // 阻塞回收管理者线程
    pthread_join(pool->managerID, NULL);
    // 唤醒阻塞的消费者线程（销毁）
    for (int i = 0; i < pool->liveNum; ++i)
        pthread_cond_signal(&pool->notEmpty);
    
    // 释放堆内存
    if (pool->taskQ)  free(pool->taskQ);
    if (pool->threadIDs)  free(pool->threadIDs);
    

    pthread_mutex_destroy(&pool->mutexPool);
    pthread_mutex_destroy(&pool->mutexBusy);
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->notFull);

    free(pool);
    pool = NULL;

    return 0;
}

/* 向任务队列添加任务 */
void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg){
    pthread_mutex_lock(&pool->mutexPool);
    while (pool->queueSize == pool->queueCapacity && !pool->shutdown)
        // 阻塞生产者线程
        pthread_cond_wait(&pool->notFull, &pool->mutexPool);
    
    if (pool->shutdown){
        pthread_mutex_unlock(&pool->mutexPool);
        return;
    }
    // 添加任务
    pool->taskQ[pool->queueRear].function = func;
    pool->taskQ[pool->queueRear].arg = arg;
    pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
    pool->queueSize++;

    // 添加了任务，就可以唤醒阻塞的线程了
    pthread_cond_signal(&pool->notEmpty);
    pthread_mutex_unlock(&pool->mutexPool);
}

/* 获取工作的线程数 */
int threadPoolBusyNum(ThreadPool* pool){
    pthread_mutex_lock(&pool->mutexBusy);
    int busyNum = pool->busyNum;
    pthread_mutex_unlock(&pool->mutexBusy);
    return busyNum;
}

/* 获取存活的线程数 */
int threadPoolAliveNum(ThreadPool* pool){
    pthread_mutex_lock(&pool->mutexPool);
    int aliveNum = pool->liveNum;
    pthread_mutex_unlock(&pool->mutexPool);
    return aliveNum;
}

/* 工作线程函数 */
void* worker(void* arg){
    // 需要在worker内访问到taskQ里面保存的回调函数，所以传入pool
    ThreadPool* pool = (ThreadPool*)arg;

    while (1){
        pthread_mutex_lock(&pool->mutexPool);
        // 当前任务队列已空 且 未销毁
        while (pool->queueSize == 0 && !pool->shutdown){
            // 阻塞工作线程
            pthread_cond_wait(&pool->notEmpty, &pool->mutexPool);

            // 判断是不是要销毁线程
            if (pool->exitNum > 0){
                pool->exitNum--;
                if (pool->liveNum > pool->minNum){
                    pool->liveNum--;
                    pthread_mutex_unlock(&pool->mutexPool);
                    threadExit(pool);
                }
            }
        }

        // 判断线程池是否被关闭了
        if (pool->shutdown){
            pthread_mutex_unlock(&pool->mutexPool); // 避免死锁
            threadExit(pool);
        }

        // 从任务队列中取出一个任务
        Task task;
        task.function = pool->taskQ[pool->queueFront].function;
        task.arg = pool->taskQ[pool->queueFront].arg;
        // 移动头结点
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
        pool->queueSize--;
        // 解锁
        pthread_cond_signal(&pool->notFull);
        pthread_mutex_unlock(&pool->mutexPool);

        printf("thread %ld start working...\n", pthread_self());
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum++;
        pthread_mutex_unlock(&pool->mutexBusy);
        task.function(task.arg);
        // 释放操作
        free(task.arg);
        task.arg = NULL;

        // 子线程任务结束                                                                                                      
        printf("thread %ld end working...\n", pthread_self());
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum--;
        pthread_mutex_unlock(&pool->mutexBusy);
    }
    return NULL;
}

/* 管理线程函数 */
void* manager(void* arg){
    ThreadPool* pool = (ThreadPool*)arg;
    while (!pool->shutdown){        // 当线程池未销毁
        // 每隔3s检测一次
        sleep(3);

        // 取出线程池中任务的数量和当前线程的数量
        pthread_mutex_lock(&pool->mutexPool);
        int queueSize = pool->queueSize;
        int liveNum = pool->liveNum;
        pthread_mutex_unlock(&pool->mutexPool);

        // 取出忙的线程的数量
        pthread_mutex_lock(&pool->mutexBusy);
        int busyNum = pool->busyNum;
        pthread_mutex_unlock(&pool->mutexBusy);

        // 添加线程
        // 任务的个数>存活的线程个数 && 存活的线程数<最大线程数
        if (queueSize > liveNum && liveNum < pool->maxNum){
            pthread_mutex_lock(&pool->mutexPool);
            // 每次创建两个线程
            int counter = 0;
            for (int i = 0; i < pool->maxNum && counter < NUMBER 
                && pool->liveNum < pool->maxNum; ++i){
                if (pool->threadIDs[i] == 0){
                    pthread_create(&pool->threadIDs[i], NULL, worker, pool);
                    counter++;
                    pool->liveNum++;
                }
            }
            pthread_mutex_unlock(&pool->mutexPool);
        }
        // 销毁线程
        // 忙的线程*2 < 存活的线程数 && 存活的线程>最小线程数
        if (busyNum * 2 < liveNum && liveNum > pool->minNum){
            pthread_mutex_lock(&pool->mutexPool);
            pool->exitNum = NUMBER;     // 一次性销毁两个
            pthread_mutex_unlock(&pool->mutexPool);
            // 让工作的线程自杀，即唤醒阻塞线程，每次杀死两个
            for (int i = 0; i < NUMBER; ++i){
                pthread_cond_signal(&pool->notEmpty);
            }
        }
    }
    return NULL;
}

/* 退出当前线程 */
void threadExit(ThreadPool* pool){
    pthread_t tid = pthread_self();     // 获取当前线程的线程id
    for (int i = 0; i < pool->maxNum; ++i){
        if (pool->threadIDs[i] == tid){     //当前要退出的线程是tid
            pool->threadIDs[i] = 0;
            printf("threadExit() called, %ld exiting...\n", tid);
            break;
        }
    }
    pthread_exit(NULL);
}

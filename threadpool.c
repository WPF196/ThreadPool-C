#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "threadpool.h"

const int NUMBER = 2;

// ����ṹ��
typedef struct Task {
	void (*function)(void* arg);	// ����ָ�룬���������ĵ�ַ�����ͣ�
	void* arg;						// �����������ĵ�ַ
}Task;

// �̳߳ؽṹ��
struct ThreadPool{
	// �������
	Task* taskQ;
	int queueCapacity;		// ����
	int queueSize;			// ��ǰ�������
	int queueFront;			// ��ͷ
	int queueRear;			// ��β

	// �߳���ز���
	pthread_t managerID;		// ����Ա�߳�
	pthread_t* threadIDs;		// �������߳�
	int minNum;					// ��С�߳�����
	int maxNum;					// ����߳�����
	int busyNum;				// �����е��̵߳ĸ���
	int liveNum;				// �����̵߳ĸ���
	int exitNum;				// Ҫ���ٵ��̸߳���
	pthread_mutex_t mutexPool;  // �����������������̳߳�
	pthread_mutex_t mutexBusy;  // ����������busyNum����
	pthread_cond_t notFull;     // ������������������Ƿ�����
	pthread_cond_t notEmpty;    // ������������������Ƿ�Ϊ��

	int shutdown;				// �ǲ���Ҫ�����̳߳�, ����Ϊ1, ������Ϊ0
};

/* �����̳߳� */
ThreadPool* threadPoolCreate(int min, int max, int queueSize){
    ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
    
    // ����do��������break�����ֱ��return����ֹ�������ã��������ͷŷ�����ڴ�
    do{
        if (pool == NULL){
            printf("malloc threadpool fail...\n");
            break;
        }

        // �洢�������̵߳�ַ�����������
        pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * max);
        if (pool->threadIDs == NULL){
            printf("malloc threadIDs fail...\n");
            break;
        }
        memset(pool->threadIDs, 0, sizeof(pthread_t) * max);
        pool->minNum = min;
        pool->maxNum = max;
        pool->busyNum = 0;
        pool->liveNum = min;    // ����С�������
        pool->exitNum = 0;

        // ��ʼ������������������
        if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 ||
            pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
            pthread_cond_init(&pool->notEmpty, NULL) != 0 ||
            pthread_cond_init(&pool->notFull, NULL) != 0){
            printf("mutex or condition init fail...\n");
            break;
        }

        // ������У����queueSize������
        pool->taskQ = (Task*)malloc(sizeof(Task) * queueSize);
        pool->queueCapacity = queueSize;
        pool->queueSize = 0;
        pool->queueFront = 0;
        pool->queueRear = 0;

        pool->shutdown = 0;

        // ����Ա�߳�
        pthread_create(&pool->managerID, NULL, manager, pool);
        // �������̣߳��ȴ���min����
        for (int i = 0; i < min; ++i){
            pthread_create(&pool->threadIDs[i], NULL, worker, pool);
        }
        return pool;    // �����ɹ������̳߳�
    } while (0);

    // �ͷ���Դ
    if (pool && pool->threadIDs) free(pool->threadIDs);
    if (pool && pool->taskQ) free(pool->taskQ);
    if (pool) free(pool);

    return NULL;
}

/* �����̳߳� */
int threadPoolDestroy(ThreadPool* pool){
    if (pool == NULL)   return -1;

    // �ر��̳߳�
    pool->shutdown = 1;
    // �������չ������߳�
    pthread_join(pool->managerID, NULL);
    // �����������������̣߳����٣�
    for (int i = 0; i < pool->liveNum; ++i)
        pthread_cond_signal(&pool->notEmpty);
    
    // �ͷŶ��ڴ�
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

/* ���������������� */
void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg){
    pthread_mutex_lock(&pool->mutexPool);
    while (pool->queueSize == pool->queueCapacity && !pool->shutdown)
        // �����������߳�
        pthread_cond_wait(&pool->notFull, &pool->mutexPool);
    
    if (pool->shutdown){
        pthread_mutex_unlock(&pool->mutexPool);
        return;
    }
    // �������
    pool->taskQ[pool->queueRear].function = func;
    pool->taskQ[pool->queueRear].arg = arg;
    pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
    pool->queueSize++;

    // ��������񣬾Ϳ��Ի����������߳���
    pthread_cond_signal(&pool->notEmpty);
    pthread_mutex_unlock(&pool->mutexPool);
}

/* ��ȡ�������߳��� */
int threadPoolBusyNum(ThreadPool* pool){
    pthread_mutex_lock(&pool->mutexBusy);
    int busyNum = pool->busyNum;
    pthread_mutex_unlock(&pool->mutexBusy);
    return busyNum;
}

/* ��ȡ�����߳��� */
int threadPoolAliveNum(ThreadPool* pool){
    pthread_mutex_lock(&pool->mutexPool);
    int aliveNum = pool->liveNum;
    pthread_mutex_unlock(&pool->mutexPool);
    return aliveNum;
}

/* �����̺߳��� */
void* worker(void* arg){
    // ��Ҫ��worker�ڷ��ʵ�taskQ���汣��Ļص����������Դ���pool
    ThreadPool* pool = (ThreadPool*)arg;

    while (1){
        pthread_mutex_lock(&pool->mutexPool);
        // ��ǰ��������ѿ� �� δ����
        while (pool->queueSize == 0 && !pool->shutdown){
            // ���������߳�
            pthread_cond_wait(&pool->notEmpty, &pool->mutexPool);

            // �ж��ǲ���Ҫ�����߳�
            if (pool->exitNum > 0){
                pool->exitNum--;
                if (pool->liveNum > pool->minNum){
                    pool->liveNum--;
                    pthread_mutex_unlock(&pool->mutexPool);
                    threadExit(pool);
                }
            }
        }

        // �ж��̳߳��Ƿ񱻹ر���
        if (pool->shutdown){
            pthread_mutex_unlock(&pool->mutexPool); // ��������
            threadExit(pool);
        }

        // �����������ȡ��һ������
        Task task;
        task.function = pool->taskQ[pool->queueFront].function;
        task.arg = pool->taskQ[pool->queueFront].arg;
        // �ƶ�ͷ���
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
        pool->queueSize--;
        // ����
        pthread_cond_signal(&pool->notFull);
        pthread_mutex_unlock(&pool->mutexPool);

        printf("thread %ld start working...\n", pthread_self());
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum++;
        pthread_mutex_unlock(&pool->mutexBusy);
        task.function(task.arg);
        // �ͷŲ���
        free(task.arg);
        task.arg = NULL;

        // ���߳��������                                                                                                      
        printf("thread %ld end working...\n", pthread_self());
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum--;
        pthread_mutex_unlock(&pool->mutexBusy);
    }
    return NULL;
}

/* �����̺߳��� */
void* manager(void* arg){
    ThreadPool* pool = (ThreadPool*)arg;
    while (!pool->shutdown){        // ���̳߳�δ����
        // ÿ��3s���һ��
        sleep(3);

        // ȡ���̳߳�������������͵�ǰ�̵߳�����
        pthread_mutex_lock(&pool->mutexPool);
        int queueSize = pool->queueSize;
        int liveNum = pool->liveNum;
        pthread_mutex_unlock(&pool->mutexPool);

        // ȡ��æ���̵߳�����
        pthread_mutex_lock(&pool->mutexBusy);
        int busyNum = pool->busyNum;
        pthread_mutex_unlock(&pool->mutexBusy);

        // ����߳�
        // ����ĸ���>�����̸߳��� && �����߳���<����߳���
        if (queueSize > liveNum && liveNum < pool->maxNum){
            pthread_mutex_lock(&pool->mutexPool);
            // ÿ�δ��������߳�
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
        // �����߳�
        // æ���߳�*2 < �����߳��� && �����߳�>��С�߳���
        if (busyNum * 2 < liveNum && liveNum > pool->minNum){
            pthread_mutex_lock(&pool->mutexPool);
            pool->exitNum = NUMBER;     // һ������������
            pthread_mutex_unlock(&pool->mutexPool);
            // �ù������߳���ɱ�������������̣߳�ÿ��ɱ������
            for (int i = 0; i < NUMBER; ++i){
                pthread_cond_signal(&pool->notEmpty);
            }
        }
    }
    return NULL;
}

/* �˳���ǰ�߳� */
void threadExit(ThreadPool* pool){
    pthread_t tid = pthread_self();     // ��ȡ��ǰ�̵߳��߳�id
    for (int i = 0; i < pool->maxNum; ++i){
        if (pool->threadIDs[i] == tid){     //��ǰҪ�˳����߳���tid
            pool->threadIDs[i] = 0;
            printf("threadExit() called, %ld exiting...\n", tid);
            break;
        }
    }
    pthread_exit(NULL);
}

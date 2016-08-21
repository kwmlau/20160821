#include <iostream>
#include <string>
#include <error.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>

using namespace std;

#define DEFAULT_TIME 10         //10s检测一次
#define MIN_WAIT_TASK_NUM 10   //如果queue_size>MIN_WAIT_TASK_NUM, 添加新的线程到线程池中
#define DEFAULT_THREAD_VARY 10  //每次创建和销毁现成的数量

//任务结构体
typedef struct {
    void *(*function)(void *);//函数指针,回调函数
    void *arg;//上面函数的参数
} threadpool_task_t;

//线程池结构体
struct threadpool_t {
    pthread_mutex_t lock;//用于锁住当前这个结构体
    pthread_mutex_t thread_counter;//记录忙状态线程个数
    pthread_cond_t queue_not_full;//
    pthread_cond_t queue_not_empty;//任务队列不为空时,通知等待获取任务的线程
    pthread_t *threads;//保存工作线程tid的数组
    pthread_t adjust_tid;//管理线程tid
    threadpool_task_t *task_queue;//任务队列
    int min_thr_num;//线程组内默认最小线程数
    int max_thr_num;//线程组内默认最大线程数
    int live_thr_num;//当前存活线程个数
    int busy_thr_num;//忙状态线程个数
    int wait_exit_thr_num;//要销毁的线程个数
    //task queue parameter
    int queue_front;//队头索引下标
    int queue_rear;//队尾索引下标
    int queue_size;//队列中元素个数
    int queue_max_size;//队列中最大荣哪个属
    int shutdown;//线程池状态,true或false
};

void *threadpool_thread(void *threadpool);
void *adjust_thread(void *threadpool);
bool is_thread_alive(pthread_t tid);
int threadpool_free(threadpool_t *pool);

/*
 * @function: threadpool_create
 * @desc: create a threadpool_t object
 * @param: thr_num - thread number
 * @param: queue_max_size - size of queue
 * @return: a newly created pool of NULL
 */
threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size) {
    threadpool_t *pool = NULL;
    do {
        if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL){
            cout << "malloc threadpool error!"<< endl;
            break;
        }
        pool->min_thr_num = min_thr_num;
        pool->max_thr_num = max_thr_num;
        pool->busy_thr_num = 0;
        pool->live_thr_num = 0;
        //task queue
        pool->queue_size = 0;
        pool->queue_max_size = queue_max_size;
        pool->queue_front = 0;
        pool->queue_rear = 0;
        pool->shutdown = false;

        pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * max_thr_num);
        if(pool->threads == NULL){
            cout << "malloc threads array error!" << endl;
            break;
        }
        memset(pool->threads, 0, sizeof(pool->threads));

        pool->task_queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t) * max_thr_num);
        if(pool->task_queue == NULL){
            cout << "malloc task_queue error!" << endl;
            break;
        }

        if(pthread_mutex_init(&(pool->lock), NULL) != 0
                || pthread_mutex_init(&(pool->thread_counter), NULL) != 0
                || pthread_cond_init(&(pool->queue_not_empty), NULL) != 0){
            cout << "init the lock or cond fail!" << endl;
            break;
        }

        for(int i = 0; i < min_thr_num; i++){
            pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void*)pool);
            cout << "start thread 0x" << (unsigned int)pool->threads[i] << "..." << endl;
        }

        pthread_create(&(pool->adjust_tid), NULL, adjust_thread, (void *)pool);

        return pool;
    } while(0);

    threadpool_free(pool);
    return NULL;
}

/*
 * @function: threadpool_add
 * @desc: add s new task in the queue of a thread pool
 * @param: pool - thread pool to which add the task
 * @param: fcuntion - pointer to the function that will perform the task
 * @param: arg - arguments to be passed the the fcuntion
 * @return: 0 if goes well, else -1;
 */
int threadpool_add(threadpool_t *pool, void*(*function)(void *arg), void *arg){
    pthread_mutex_lock(&(pool->lock));//加锁
    while((pool->queue_size == pool->queue_max_size) && !(pool->shutdown))
        //queue full, wait...
        pthread_cond_wait(&(pool->queue_not_full), &(pool->lock));
    if(pool->shutdown)
        pthread_mutex_unlock(&(pool->lock));
    //add a task to queue
    if(pool->task_queue[pool->queue_rear].arg != NULL){
        free(pool->task_queue[pool->queue_rear].arg);
        pool->task_queue[pool->queue_rear].arg = NULL;
    }
    pool->task_queue[pool->queue_rear].function = function;
    pool->task_queue[pool->queue_rear].arg = arg;
    pool->queue_rear = (pool->queue_rear + 1) % pool->queue_max_size;
    pool->queue_size++;

    //queue not empty
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));//解锁

    return 0;
}

/*
 * @funtion: void *threadpool_thread(void *threadpool)
 * @desc: the worker thread
 * @param: threadpool - the pool which own the thread
 */
void *threadpool_thread(void *threadpool){
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;
    while(1){
        //刚创建出线程,等待任务队列里有任务;否则阻塞等待任务队列里有任务后唤醒接收任务
        pthread_mutex_lock(&(pool->lock));
        while((pool->queue_size == 0) && (!pool->shutdown)){
            cout << "thread 0x" << (unsigned int)pthread_self() << " is waiting..." << endl;
            pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));
            //清楚制定数目的空闲线程,如果要结束的线程个数大于0,结束线程
            if(pool->wait_exit_thr_num > 0){
                pool->wait_exit_thr_num--;
                //如果线程池中线程的个数大于最小值时,可以结束当前线程
                if(pool->live_thr_num > pool->min_thr_num){
                    cout << "thread 0x" << (unsigned int)pthread_self() << " is exiting..." << endl;
                    pool->live_thr_num--;
                    pthread_mutex_unlock(&(pool->lock));
                    pthread_exit(NULL);
                }
            }
        }
        //如果指定了true, 要关闭线程池里的每个线程,自行退出处理
        if(pool->shutdown){
            pthread_mutex_lock(&(pool->lock));
            cout << "thread 0x" << (unsigned int)pthread_self() << " is exiting..." << endl;
            pthread_exit(NULL);
        }
        //从任务队列中获得任务
        task.function = pool->task_queue[pool->queue_front].function;
        task.arg = pool->task_queue[pool->queue_front].arg;
        pool->queue_front = (pool->queue_front + 1) % pool->queue_max_size;
        pool->queue_size--;

        //通知可以又新的任务添加进来
        pthread_cond_broadcast(&(pool->queue_not_full));
        pthread_mutex_unlock(&(pool->lock));
        //执行任务
        cout << "thread 0x" << (unsigned int)pthread_self() << " start  working..." << endl;
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thr_num++;//忙状态线程数+1
        pthread_mutex_unlock(&(pool->thread_counter));
        (*(task.function))(task.arg);//执行回调函数
        //任务结束处理
        cout << "thread 0x" << (unsigned int)pthread_self() << " end  working..." << endl;
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thr_num--;//忙态数减一
        pthread_mutex_unlock(&(pool->thread_counter));
    }

    pthread_exit(NULL);
    return NULL;
}

/*
 * @function: void *adjust_thread(void *threadpool)
 * @desc: manager thread
 * @param: threadpool - the pool which own thr thread
 */
void *adjust_thread(void *threadpool){
    threadpool_t *pool = (threadpool_t *)threadpool;
    while(pool->shutdown){
        sleep(DEFAULT_TIME);
        pthread_mutex_lock(&(pool->lock));
        int queue_size = pool->queue_size;
        int live_thr_num = pool->live_thr_num;
        pthread_mutex_unlock(&(pool->lock));

        pthread_mutex_lock(&(pool->thread_counter));
        int busy_thr_num = pool->busy_thr_num;
        pthread_mutex_unlock(&(pool->thread_counter));

        //任务数大于最小线程池个数且存活的线程数少于最大线程个数时,创建新线程
        if(queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thr_num){
            pthread_mutex_lock(&(pool->lock));
            int add = 0;
            //一次增加DEFAULT_THREAD个线程
            for(int i = 0; i < pool->max_thr_num && add < DEFAULT_THREAD_VARY
                    && pool-> live_thr_num < pool->max_thr_num; i++){
                if(pool->threads[i] == 0 || !is_thread_alive(pool->threads[i])){
                    pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool);
                    add++;
                    pool->live_thr_num++;
                }
            }
            pthread_mutex_unlock(&(pool->lock));
        }

        //销毁多余的线程
        if((busy_thr_num * 2) < live_thr_num
                && live_thr_num > pool->min_thr_num){
            //一次销毁DEFAUL_THREAD个线程
            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;
            pthread_mutex_unlock(&(pool->lock));

            for(int i = 0; i < DEFAULT_THREAD_VARY; i++){
                pthread_cond_signal(&(pool->queue_not_empty));
            }
        }
    }
}

/*
 * @function: threadpool_destroy
 * @desc: stop and destroy a thread pool
 * @param: pool - thread pool to destroy
 * @return: 0 if destroy succes, else -1
 */
int threadpool_destroy(threadpool_t *pool){
    if(pool == NULL)
        return -1;
    pool->shutdown = true;
    //先销毁管理线程
    pthread_join(pool->adjust_tid, NULL);
    //再同志所有空闲线程
    pthread_cond_broadcast(&(pool->queue_not_empty));
    for(int i = 0; i < pool->min_thr_num; i++){
        pthread_join(pool->threads[i], NULL);
    }
    threadpool_free(pool);

    return 0;
}

int threadpool_free(threadpool_t *pool){
    if(pool == NULL)
        return -1;
    if(pool->task_queue)
        free(pool->task_queue);
    if(pool->threads){
        free(pool->threads);
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_mutex_lock(&(pool->thread_counter));
        pthread_mutex_destroy(&(pool->thread_counter));
        pthread_cond_destroy(&(pool->queue_not_empty));
        pthread_cond_destroy(&(pool->queue_not_full));
    }
    free(pool);
    pool = NULL;

    return 0;
}

/*
 * @function: threadpool_all_threadnum
 * @desc: get the thread number
 * @papram: pool - threadpool
 * @return; # of the threads
 */
int threadpool_all_threadnum(threadpool_t *pool){
    int all_threadnum = -1;
    pthread_mutex_lock((&pool->lock));
    all_threadnum = pool->live_thr_num;
    pthread_mutex_unlock(&(pool->lock));

    return all_threadnum;
}

int threadpool_busy_threadnum(threadpool_t *pool){
    int busy_threadnum = -1;
    pthread_mutex_lock((&pool->thread_counter));
    busy_threadnum = pool->busy_thr_num;
    pthread_mutex_unlock(&(pool->thread_counter));

    return busy_threadnum;
}

/*
 * check whether a thread is alive
 */
bool is_thread_alive(pthread_t tid){
    int kiil_rc = pthread_kill(tid, 0);
    if(kiil_rc == ESRCH)
        return false;
    return true;
}

#if 1
void *process(void *arg){
    cout << "thread 0x" << (unsigned int)pthread_self() << " is working on task" << *(int *)arg << endl;
    sleep(1);
    cout << "task " << *(int *)arg << " is ending..." << endl;

    return NULL;
}

int main() {
    threadpool_t *thp = threadpool_create(3,100,12);
    cout << "pool initied..." << endl;

    int *num = (int *)malloc(sizeof(int) * 20);
    for(int i = 0; i < 10; i++){
        num[i] = i;
        cout << "adding task " << i << endl;
        threadpool_add(thp, process, (void *)&num[i]);
    }
    sleep(10);
    threadpool_destroy(thp);

    return 0;
}
#endif
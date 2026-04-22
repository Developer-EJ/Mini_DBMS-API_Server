#ifndef SERVER_THREADPOOL_H
#define SERVER_THREADPOOL_H

typedef struct {
    int min_workers;
    int max_workers;      /* phase 1: min_workers == max_workers */
    int queue_capacity;
    int reject_when_full; /* return THREADPOOL_QUEUE_FULL instead of blocking */
} ThreadPoolConfig;

typedef void (*task_fn)(void *arg);

typedef struct {
    task_fn fn;
    void *arg;
} Task;

typedef struct ThreadPool ThreadPool;

#define THREADPOOL_OK          0
#define THREADPOOL_ERR        -1
#define THREADPOOL_QUEUE_FULL -2

ThreadPool *threadpool_create(const ThreadPoolConfig *cfg);
int threadpool_submit(ThreadPool *tp, task_fn fn, void *arg);
void threadpool_shutdown(ThreadPool *tp, int drain);
void threadpool_destroy(ThreadPool *tp);

#endif

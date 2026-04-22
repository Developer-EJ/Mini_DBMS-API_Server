#include "server/threadpool.h"

#include <pthread.h>
#include <stdlib.h>

struct ThreadPool {
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    Task *queue;
    int cap;
    int head;
    int tail;
    int size;

    pthread_t *workers;
    int n_workers;
    int shutdown;
    ThreadPoolConfig cfg;
};

static void *worker_loop(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;

    for (;;) {
        pthread_mutex_lock(&tp->mtx);

        while (tp->size == 0 && tp->shutdown == 0)
            pthread_cond_wait(&tp->not_empty, &tp->mtx);

        if (tp->size == 0 && tp->shutdown != 0) {
            pthread_mutex_unlock(&tp->mtx);
            return NULL;
        }

        Task task = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->size--;

        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mtx);

        task.fn(task.arg);
    }
}

ThreadPool *threadpool_create(const ThreadPoolConfig *cfg) {
    if (!cfg || cfg->min_workers <= 0 || cfg->queue_capacity <= 0)
        return NULL;
    if (cfg->min_workers != cfg->max_workers)
        return NULL;

    ThreadPool *tp = (ThreadPool *)calloc(1, sizeof(ThreadPool));
    if (!tp) return NULL;

    tp->cfg = *cfg;
    tp->cap = cfg->queue_capacity;
    tp->n_workers = cfg->min_workers;
    tp->queue = (Task *)calloc((size_t)tp->cap, sizeof(Task));
    tp->workers = (pthread_t *)calloc((size_t)tp->n_workers, sizeof(pthread_t));

    if (!tp->queue || !tp->workers) {
        free(tp->queue);
        free(tp->workers);
        free(tp);
        return NULL;
    }

    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);

    for (int i = 0; i < tp->n_workers; i++) {
        if (pthread_create(&tp->workers[i], NULL, worker_loop, tp) != 0) {
            tp->n_workers = i;
            threadpool_shutdown(tp, 1);
            threadpool_destroy(tp);
            return NULL;
        }
    }

    return tp;
}

int threadpool_submit(ThreadPool *tp, task_fn fn, void *arg) {
    if (!tp || !fn) return THREADPOOL_ERR;

    pthread_mutex_lock(&tp->mtx);

    if (tp->size == tp->cap && tp->cfg.reject_when_full) {
        pthread_mutex_unlock(&tp->mtx);
        return THREADPOOL_QUEUE_FULL;
    }

    while (tp->size == tp->cap && tp->shutdown == 0)
        pthread_cond_wait(&tp->not_full, &tp->mtx);

    if (tp->shutdown != 0) {
        pthread_mutex_unlock(&tp->mtx);
        return THREADPOOL_ERR;
    }

    tp->queue[tp->tail].fn = fn;
    tp->queue[tp->tail].arg = arg;
    tp->tail = (tp->tail + 1) % tp->cap;
    tp->size++;

    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mtx);
    return THREADPOOL_OK;
}

void threadpool_shutdown(ThreadPool *tp, int drain) {
    if (!tp) return;
    (void)drain;

    pthread_mutex_lock(&tp->mtx);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_cond_broadcast(&tp->not_full);
    pthread_mutex_unlock(&tp->mtx);

    for (int i = 0; i < tp->n_workers; i++)
        pthread_join(tp->workers[i], NULL);
}

void threadpool_destroy(ThreadPool *tp) {
    if (!tp) return;

    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);
    free(tp->queue);
    free(tp->workers);
    free(tp);
}

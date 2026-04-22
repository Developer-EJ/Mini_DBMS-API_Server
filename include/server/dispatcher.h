#ifndef SERVER_DISPATCHER_H
#define SERVER_DISPATCHER_H

#include "engine_adapter.h"
#include "threadpool.h"

typedef struct {
    ThreadPool *pool;
    EngineAdapter *engine;
} DispatchDeps;

typedef struct {
    int client_fd;
    EngineAdapter *engine;
} DispatchCtx;

void dispatcher_on_accept(int client_fd, void *ctx_opaque);
int dispatcher_handle_client(int client_fd, EngineAdapter *engine);
void dispatcher_handle_task(void *arg);

#endif

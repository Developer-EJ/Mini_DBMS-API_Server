#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "server/dispatcher.h"
#include "server/engine_adapter.h"
#include "server/server.h"
#include "server/threadpool.h"

static Server *g_server = NULL;

static void handle_signal(int signo) {
    (void)signo;
    if (g_server)
        server_stop(g_server);
}

static int parse_positive(const char *text, int fallback) {
    char *end = NULL;
    long value = 0;

    if (!text) return fallback;
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value <= 0 || value > 65535)
        return fallback;
    return (int)value;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <port> [workers] [queue_capacity]\n",
            argv0);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        print_usage(argv[0]);
        return 1;
    }

    int port = parse_positive(argv[1], -1);
    if (port <= 0 || port > 65535) {
        print_usage(argv[0]);
        return 1;
    }

    int workers = parse_positive(argc >= 3 ? argv[2] : NULL, 4);
    int queue_capacity = parse_positive(argc >= 4 ? argv[3] : NULL, 128);

    EngineAdapter *engine = engine_adapter_create();
    if (!engine) {
        fprintf(stderr, "failed to create engine adapter\n");
        return 1;
    }

    ThreadPoolConfig pool_cfg = {
        .min_workers = workers,
        .max_workers = workers,
        .queue_capacity = queue_capacity,
        .reject_when_full = 1
    };
    ThreadPool *pool = threadpool_create(&pool_cfg);
    if (!pool) {
        fprintf(stderr, "failed to create thread pool\n");
        engine_adapter_destroy(engine);
        return 1;
    }

    ServerConfig server_cfg = {
        .port = port,
        .backlog = 1024
    };
    Server *server = server_create(&server_cfg);
    if (!server) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        threadpool_shutdown(pool, 1);
        threadpool_destroy(pool);
        engine_adapter_destroy(engine);
        return 1;
    }

    DispatchDeps deps = {
        .pool = pool,
        .engine = engine
    };

    g_server = server;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("MiniDBMS API server listening on port %d "
           "(workers=%d, queue_capacity=%d)\n",
           port, workers, queue_capacity);

    int rc = server_run(server, dispatcher_on_accept, &deps);

    threadpool_shutdown(pool, 1);
    threadpool_destroy(pool);
    server_destroy(server);
    engine_adapter_destroy(engine);
    g_server = NULL;

    return rc == 0 ? 0 : 1;
}

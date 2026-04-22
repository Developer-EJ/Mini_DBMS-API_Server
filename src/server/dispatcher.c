#include "server/dispatcher.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server/http_parser.h"
#include "server/response.h"

#define DISPATCHER_SQL_MAX 65536

static int is_sql_route(const char *path) {
    return strcmp(path, "/query") == 0 || strcmp(path, "/sql") == 0;
}

int dispatcher_handle_client(int client_fd, EngineAdapter *engine) {
    HttpRequest req;
    char *sql = NULL;
    EngineResult result;

    if (http_parser_read(client_fd, &req) != 0) {
        response_write_error(client_fd, 400, "invalid HTTP request");
        return -1;
    }

    if (req.payload_too_large) {
        response_write_error(client_fd, 413, "request body is too large");
        http_request_free(&req);
        return -1;
    }

    if (req.bad_request) {
        response_write_error(client_fd, 400, "malformed HTTP request");
        http_request_free(&req);
        return -1;
    }

    if (req.method == HTTP_METHOD_OTHER) {
        response_write_error(client_fd, 405, "only GET and POST are supported");
        http_request_free(&req);
        return -1;
    }

    if (!is_sql_route(req.path)) {
        response_write_error(client_fd, 404, "route not found");
        http_request_free(&req);
        return -1;
    }

    sql = (char *)calloc(DISPATCHER_SQL_MAX + 1, 1);
    if (!sql) {
        response_write_error(client_fd, 500, "out of memory");
        http_request_free(&req);
        return -1;
    }

    if (http_parser_extract_sql(&req, sql, DISPATCHER_SQL_MAX + 1) != 0) {
        response_write_error(client_fd, 400, "missing SQL statement");
        free(sql);
        http_request_free(&req);
        return -1;
    }

    if (engine_adapter_execute(engine, sql, &result) != 0) {
        response_write_error(client_fd, 500, "engine adapter failed");
        free(sql);
        http_request_free(&req);
        return -1;
    }

    response_write_engine_result(client_fd, &result);
    engine_result_free(&result);
    free(sql);
    http_request_free(&req);
    return 0;
}

void dispatcher_on_accept(int client_fd, void *ctx_opaque) {
    DispatchDeps *deps = (DispatchDeps *)ctx_opaque;

    if (!deps || !deps->pool || !deps->engine) {
        response_write_error(client_fd, 503, "server is not ready");
        close(client_fd);
        return;
    }

    DispatchCtx *ctx = (DispatchCtx *)calloc(1, sizeof(DispatchCtx));
    if (!ctx) {
        response_write_error(client_fd, 503, "server is overloaded");
        close(client_fd);
        return;
    }

    ctx->client_fd = client_fd;
    ctx->engine = deps->engine;

    int rc = threadpool_submit(deps->pool, dispatcher_handle_task, ctx);
    if (rc != THREADPOOL_OK) {
        if (rc == THREADPOOL_QUEUE_FULL)
            response_write_error(client_fd, 503, "request queue is full");
        else
            response_write_error(client_fd, 503, "server is shutting down");
        close(client_fd);
        free(ctx);
    }
}

void dispatcher_handle_task(void *arg) {
    DispatchCtx *ctx = (DispatchCtx *)arg;
    if (!ctx) return;

    dispatcher_handle_client(ctx->client_fd, ctx->engine);
    close(ctx->client_fd);
    free(ctx);
}

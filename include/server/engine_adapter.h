#ifndef SERVER_ENGINE_ADAPTER_H
#define SERVER_ENGINE_ADAPTER_H

#include "../interface.h"

typedef enum {
    ENGINE_OK = 0,
    ENGINE_ERR_PARSE,
    ENGINE_ERR_SCHEMA,
    ENGINE_ERR_VALIDATE,
    ENGINE_ERR_UNSUPPORTED,
    ENGINE_ERR_EXEC,
    ENGINE_ERR_INTERNAL
} EngineStatus;

typedef enum {
    SQL_KIND_UNKNOWN = 0,
    SQL_KIND_SELECT,
    SQL_KIND_INSERT,
    SQL_KIND_WRITE_UNSUPPORTED
} SqlKind;

typedef struct {
    EngineStatus status;
    SqlKind kind;
    int is_select;
    int affected_rows;
    ResultSet *rows;
    char error[512];
} EngineResult;

typedef struct EngineAdapter EngineAdapter;

EngineAdapter *engine_adapter_create(void);
void engine_adapter_destroy(EngineAdapter *ea);
int engine_adapter_execute(EngineAdapter *ea, const char *sql,
                           EngineResult *out);
void engine_result_free(EngineResult *r);

#endif

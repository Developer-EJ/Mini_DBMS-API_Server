#include "server/engine_adapter.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index_manager.h"

struct EngineAdapter {
    pthread_rwlock_t lock;
};

static void result_init(EngineResult *out) {
    memset(out, 0, sizeof(*out));
    out->status = ENGINE_ERR_INTERNAL;
    out->kind = SQL_KIND_UNKNOWN;
}

static void set_error(EngineResult *out, EngineStatus status,
                      const char *message) {
    out->status = status;
    if (message)
        snprintf(out->error, sizeof(out->error), "%s", message);
}

static const char *skip_sql_noise(const char *p) {
    for (;;) {
        while (*p && isspace((unsigned char)*p))
            p++;

        if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p && *p != '\n')
                p++;
            continue;
        }

        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p[0] && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p[0]) p += 2;
            continue;
        }

        return p;
    }
}

static SqlKind classify_sql(const char *sql) {
    char word[32];
    size_t len = 0;
    const char *p = skip_sql_noise(sql);

    while (p[len] &&
           (isalpha((unsigned char)p[len]) || p[len] == '_') &&
           len + 1 < sizeof(word)) {
        word[len] = (char)toupper((unsigned char)p[len]);
        len++;
    }
    word[len] = '\0';

    if (strcmp(word, "SELECT") == 0) return SQL_KIND_SELECT;
    if (strcmp(word, "INSERT") == 0) return SQL_KIND_INSERT;
    if (strcmp(word, "UPDATE") == 0 ||
        strcmp(word, "DELETE") == 0 ||
        strcmp(word, "CREATE") == 0 ||
        strcmp(word, "DROP") == 0 ||
        strcmp(word, "ALTER") == 0)
        return SQL_KIND_WRITE_UNSUPPORTED;

    return SQL_KIND_UNKNOWN;
}

static char *copy_first_statement(const char *sql) {
    const char *start = skip_sql_noise(sql);
    const char *end = start;
    int in_quote = 0;

    while (*end) {
        if (*end == '\'')
            in_quote = !in_quote;
        else if (*end == ';' && !in_quote)
            break;
        end++;
    }

    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    if (end == start)
        return NULL;

    size_t len = (size_t)(end - start);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;

    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

EngineAdapter *engine_adapter_create(void) {
    EngineAdapter *ea = (EngineAdapter *)calloc(1, sizeof(EngineAdapter));
    if (!ea) return NULL;

    if (pthread_rwlock_init(&ea->lock, NULL) != 0) {
        free(ea);
        return NULL;
    }
    return ea;
}

void engine_adapter_destroy(EngineAdapter *ea) {
    if (!ea) return;
    index_cleanup();
    pthread_rwlock_destroy(&ea->lock);
    free(ea);
}

int engine_adapter_execute(EngineAdapter *ea, const char *sql,
                           EngineResult *out) {
    char *stmt = NULL;
    TokenList *tokens = NULL;
    ASTNode *ast = NULL;
    TableSchema *schema = NULL;
    int locked = 0;

    if (!out) return -1;
    result_init(out);

    if (!ea || !sql) {
        set_error(out, ENGINE_ERR_INTERNAL, "engine adapter is not ready");
        return 0;
    }

    stmt = copy_first_statement(sql);
    if (!stmt) {
        set_error(out, ENGINE_ERR_PARSE, "empty SQL statement");
        return 0;
    }

    out->kind = classify_sql(stmt);
    if (out->kind == SQL_KIND_WRITE_UNSUPPORTED) {
        set_error(out, ENGINE_ERR_UNSUPPORTED,
                  "this SQL statement is not supported by the current engine");
        free(stmt);
        return 0;
    }
    if (out->kind == SQL_KIND_UNKNOWN) {
        set_error(out, ENGINE_ERR_PARSE, "unknown SQL statement");
        free(stmt);
        return 0;
    }

    /*
     * The adapter keeps an rwlock so the API boundary is ready for a future
     * SELECT read-lock path. The current engine's SELECT still mutates shared
     * index/stat fields during index_init/search, so phase 1 uses the write
     * side for every engine call to preserve the single-threaded invariant.
     */
    if (pthread_rwlock_wrlock(&ea->lock) != 0) {
        set_error(out, ENGINE_ERR_INTERNAL, "failed to acquire engine lock");
        free(stmt);
        return 0;
    }
    locked = 1;

    tokens = lexer_tokenize(stmt);
    if (!tokens) {
        set_error(out, ENGINE_ERR_PARSE, "SQL tokenization failed");
        goto done;
    }

    ast = parser_parse(tokens);
    if (!ast) {
        set_error(out, ENGINE_ERR_PARSE, "SQL parsing failed");
        goto done;
    }

    const char *table = (ast->type == STMT_SELECT)
                        ? ast->select.table
                        : ast->insert.table;

    schema = schema_load(table);
    if (!schema) {
        set_error(out, ENGINE_ERR_SCHEMA, "schema not found");
        goto done;
    }

    if (schema_validate(ast, schema) != SQL_OK) {
        set_error(out, ENGINE_ERR_VALIDATE, "schema validation failed");
        goto done;
    }

    if (index_init(table, IDX_ORDER_DEFAULT, IDX_ORDER_DEFAULT) != 0) {
        set_error(out, ENGINE_ERR_INTERNAL, "index initialization failed");
        goto done;
    }

    if (ast->type == STMT_SELECT) {
        ResultSet *rs = db_select(&ast->select, schema);
        if (!rs) {
            set_error(out, ENGINE_ERR_EXEC, "select execution failed");
            goto done;
        }
        out->rows = rs;
        out->is_select = 1;
        out->status = ENGINE_OK;
    } else {
        if (db_insert(&ast->insert, schema) != SQL_OK) {
            set_error(out, ENGINE_ERR_EXEC, "insert execution failed");
            goto done;
        }
        out->affected_rows = 1;
        out->status = ENGINE_OK;
    }

done:
    if (schema) schema_free(schema);
    if (ast) parser_free(ast);
    if (tokens) lexer_free(tokens);
    if (locked) pthread_rwlock_unlock(&ea->lock);
    free(stmt);
    return 0;
}

void engine_result_free(EngineResult *r) {
    if (!r) return;
    result_free(r->rows);
    r->rows = NULL;
}

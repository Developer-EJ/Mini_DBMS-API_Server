#include "server/response.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

static const char *status_text_for(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

static const char *status_code_name(int status_code) {
    switch (status_code) {
        case 400: return "BAD_REQUEST";
        case 404: return "NOT_FOUND";
        case 405: return "METHOD_NOT_ALLOWED";
        case 413: return "PAYLOAD_TOO_LARGE";
        case 501: return "NOT_IMPLEMENTED";
        case 503: return "SERVICE_UNAVAILABLE";
        default: return "INTERNAL_ERROR";
    }
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }

    return 0;
}

static int response_write_body(int fd, int status_code,
                               const char *status_text,
                               const char *content_type,
                               const char *body);

static int jb_reserve(JsonBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap)
        return 0;

    size_t next = b->cap ? b->cap * 2 : 256;
    while (next < b->len + extra + 1)
        next *= 2;

    char *data = (char *)realloc(b->data, next);
    if (!data) return -1;

    b->data = data;
    b->cap = next;
    return 0;
}

static int jb_append_n(JsonBuf *b, const char *s, size_t n) {
    if (jb_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int jb_append(JsonBuf *b, const char *s) {
    return jb_append_n(b, s, strlen(s));
}

static int jb_appendf(JsonBuf *b, const char *fmt, ...) {
    va_list ap;
    va_list cp;
    int needed;

    va_start(ap, fmt);
    va_copy(cp, ap);
    needed = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }
    if (jb_reserve(b, (size_t)needed) != 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    b->len += (size_t)needed;
    va_end(ap);
    return 0;
}

static int jb_append_json_string(JsonBuf *b, const char *s) {
    if (jb_append(b, "\"") != 0) return -1;
    if (!s) s = "";

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char tmp[8];
        switch (*p) {
            case '"': if (jb_append(b, "\\\"") != 0) return -1; break;
            case '\\': if (jb_append(b, "\\\\") != 0) return -1; break;
            case '\b': if (jb_append(b, "\\b") != 0) return -1; break;
            case '\f': if (jb_append(b, "\\f") != 0) return -1; break;
            case '\n': if (jb_append(b, "\\n") != 0) return -1; break;
            case '\r': if (jb_append(b, "\\r") != 0) return -1; break;
            case '\t': if (jb_append(b, "\\t") != 0) return -1; break;
            default:
                if (*p < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    if (jb_append(b, tmp) != 0) return -1;
                } else {
                    if (jb_append_n(b, (const char *)p, 1) != 0) return -1;
                }
                break;
        }
    }

    return jb_append(b, "\"");
}

int response_write_json(int fd, int status_code,
                        const char *status_text,
                        const char *json_body) {
    return response_write_body(fd, status_code, status_text,
                               "application/json; charset=utf-8",
                               json_body);
}

static int response_write_body(int fd, int status_code,
                               const char *status_text,
                               const char *content_type,
                               const char *body) {
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    const char *reason = status_text ? status_text : status_text_for(status_code);

    int n = snprintf(header, sizeof(header),
                     "HTTP/1.0 %d %s\r\n"
                     "Server: MiniDBMS API Server\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status_code, reason,
                     content_type ? content_type : "text/plain; charset=utf-8",
                     body_len);
    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;

    if (write_all(fd, header, (size_t)n) != 0)
        return -1;
    if (body_len > 0 && write_all(fd, body, body_len) != 0)
        return -1;
    return 0;
}

static int response_write_error_code(int fd, int status_code,
                                     const char *code,
                                     const char *msg) {
    JsonBuf b = {0};
    int rc = -1;

    if (jb_append(&b, "{\"ok\":false,\"error\":{\"code\":") != 0) goto done;
    if (jb_append_json_string(&b, code ? code : status_code_name(status_code)) != 0) goto done;
    if (jb_append(&b, ",\"message\":") != 0) goto done;
    if (jb_append_json_string(&b, msg ? msg : status_text_for(status_code)) != 0) goto done;
    if (jb_append(&b, "}}") != 0) goto done;

    rc = response_write_json(fd, status_code, status_text_for(status_code), b.data);

done:
    free(b.data);
    return rc;
}

int response_write_error(int fd, int status_code, const char *msg) {
    return response_write_error_code(fd, status_code,
                                     status_code_name(status_code), msg);
}

static int status_from_engine(const EngineResult *r) {
    switch (r->status) {
        case ENGINE_OK: return 200;
        case ENGINE_ERR_PARSE: return 400;
        case ENGINE_ERR_SCHEMA: return 404;
        case ENGINE_ERR_VALIDATE: return 400;
        case ENGINE_ERR_UNSUPPORTED: return 501;
        case ENGINE_ERR_EXEC: return 500;
        case ENGINE_ERR_INTERNAL: return 500;
        default: return 500;
    }
}

static const char *engine_error_code(const EngineResult *r) {
    switch (r->status) {
        case ENGINE_ERR_PARSE: return "BAD_SQL";
        case ENGINE_ERR_SCHEMA: return "SCHEMA_NOT_FOUND";
        case ENGINE_ERR_VALIDATE: return "SCHEMA_VALIDATION_FAILED";
        case ENGINE_ERR_UNSUPPORTED: return "SQL_NOT_IMPLEMENTED";
        case ENGINE_ERR_EXEC: return "EXECUTION_FAILED";
        case ENGINE_ERR_INTERNAL: return "INTERNAL_ERROR";
        default: return "INTERNAL_ERROR";
    }
}

static int display_width(const char *s) {
    int width = 0;
    const unsigned char *p = (const unsigned char *)(s ? s : "");

    while (*p) {
        if ((*p & 0x80) == 0x00) {
            width += 1;
            p += 1;
        } else if ((*p & 0xE0) == 0xC0) {
            width += 2;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            width += 2;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            width += 2;
            p += 4;
        } else {
            width += 1;
            p += 1;
        }
    }

    return width;
}

static int append_spaces(JsonBuf *b, int count) {
    for (int i = 0; i < count; i++) {
        if (jb_append(b, " ") != 0) return -1;
    }
    return 0;
}

static int append_table_text(JsonBuf *b, const char *s) {
    if (!s) s = "";

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char ch = (char)*p;
        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
        if (jb_append_n(b, &ch, 1) != 0)
            return -1;
    }

    return 0;
}

static int append_separator(JsonBuf *b, const int *widths, int col_count) {
    for (int c = 0; c < col_count; c++) {
        if (jb_append(b, "+") != 0) return -1;
        for (int i = 0; i < widths[c] + 2; i++) {
            if (jb_append(b, "-") != 0) return -1;
        }
    }
    return jb_append(b, "+\n");
}

static int append_cell(JsonBuf *b, const char *text, int width) {
    int pad = width - display_width(text);
    if (pad < 0) pad = 0;

    if (jb_append(b, "| ") != 0) return -1;
    if (append_table_text(b, text) != 0) return -1;
    if (append_spaces(b, pad + 1) != 0) return -1;
    return 0;
}

static int append_result_table(JsonBuf *b, const ResultSet *rs) {
    if (!rs || rs->col_count <= 0) {
        return jb_append(b, "(0 rows)\n");
    }

    int *widths = (int *)calloc((size_t)rs->col_count, sizeof(int));
    if (!widths) return -1;

    for (int c = 0; c < rs->col_count; c++) {
        widths[c] = display_width(rs->col_names[c]);
        for (int r = 0; r < rs->row_count; r++) {
            if (c < rs->rows[r].count) {
                int w = display_width(rs->rows[r].values[c]);
                if (w > widths[c]) widths[c] = w;
            }
        }
    }

    int rc = -1;
    if (append_separator(b, widths, rs->col_count) != 0) goto done;

    for (int c = 0; c < rs->col_count; c++) {
        if (append_cell(b, rs->col_names[c], widths[c]) != 0) goto done;
    }
    if (jb_append(b, "|\n") != 0) goto done;
    if (append_separator(b, widths, rs->col_count) != 0) goto done;

    for (int r = 0; r < rs->row_count; r++) {
        for (int c = 0; c < rs->col_count; c++) {
            const char *value = "";
            if (c < rs->rows[r].count)
                value = rs->rows[r].values[c];
            if (append_cell(b, value, widths[c]) != 0) goto done;
        }
        if (jb_append(b, "|\n") != 0) goto done;
    }

    if (append_separator(b, widths, rs->col_count) != 0) goto done;
    if (jb_appendf(b, "(%d rows)\n", rs->row_count) != 0) goto done;

    rc = 0;

done:
    free(widths);
    return rc;
}

int response_write_engine_result(int fd, const EngineResult *r) {
    if (!r)
        return response_write_error(fd, 500, "missing engine result");

    if (r->status != ENGINE_OK) {
        return response_write_error_code(fd, status_from_engine(r),
                                         engine_error_code(r), r->error);
    }

    JsonBuf b = {0};
    int rc = -1;

    if (r->is_select) {
        if (append_result_table(&b, r->rows) != 0)
            goto done;
    } else {
        if (jb_appendf(&b,
                       "{\"ok\":true,\"type\":\"insert\","
                       "\"affected_rows\":%d}",
                       r->affected_rows) != 0)
            goto done;
    }

    if (r->is_select)
        rc = response_write_body(fd, 200, "OK",
                                 "text/plain; charset=utf-8", b.data);
    else
        rc = response_write_json(fd, 200, "OK", b.data);

done:
    free(b.data);
    return rc;
}

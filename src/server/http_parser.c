#include "server/http_parser.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#define HTTP_MAX_HEADER 16384
#define HTTP_MAX_BODY   65536

static int copy_bounded(char *dst, size_t dst_size,
                        const char *src, size_t src_len) {
    if (!dst || dst_size == 0 || src_len >= dst_size)
        return -1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return 0;
}

static int parse_uri(HttpRequest *req, const char *uri) {
    const char *q = strchr(uri, '?');
    if (!q)
        return copy_bounded(req->path, sizeof(req->path), uri, strlen(uri));

    if (copy_bounded(req->path, sizeof(req->path),
                     uri, (size_t)(q - uri)) != 0)
        return -1;
    return copy_bounded(req->query, sizeof(req->query), q + 1, strlen(q + 1));
}

static int parse_content_length(const char *value, int *out) {
    long n = 0;

    while (*value && isspace((unsigned char)*value))
        value++;
    if (!isdigit((unsigned char)*value))
        return -1;

    while (*value && isdigit((unsigned char)*value)) {
        n = n * 10 + (*value - '0');
        if (n > HTTP_MAX_BODY) {
            *out = (int)n;
            return 1;
        }
        value++;
    }

    *out = (int)n;
    return 0;
}

static int read_exact(int fd, char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }

    return 0;
}

int http_parser_read(int fd, HttpRequest *out) {
    char header_buf[HTTP_MAX_HEADER + 1];
    size_t used = 0;
    char *header_end = NULL;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    while (used < HTTP_MAX_HEADER) {
        ssize_t n = recv(fd, header_buf + used, HTTP_MAX_HEADER - used, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;

        used += (size_t)n;
        header_buf[used] = '\0';
        header_end = strstr(header_buf, "\r\n\r\n");
        if (header_end) break;
    }

    if (!header_end) {
        out->bad_request = 1;
        return 0;
    }

    size_t header_len = (size_t)(header_end + 4 - header_buf);
    size_t buffered_body = used - header_len;
    *header_end = '\0';

    char *save = NULL;
    char *line = strtok_r(header_buf, "\r\n", &save);
    if (!line) {
        out->bad_request = 1;
        return 0;
    }

    char method[16];
    char uri[1024];
    char version[32];
    if (sscanf(line, "%15s %1023s %31s", method, uri, version) != 3) {
        out->bad_request = 1;
        return 0;
    }

    if (strcmp(method, "GET") == 0)
        out->method = HTTP_GET;
    else if (strcmp(method, "POST") == 0)
        out->method = HTTP_POST;
    else
        out->method = HTTP_METHOD_OTHER;

    if (parse_uri(out, uri) != 0) {
        out->bad_request = 1;
        return 0;
    }

    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        char *colon = strchr(line, ':');
        if (!colon) continue;

        *colon = '\0';
        if (strcasecmp(line, "Content-Length") == 0) {
            int parsed = 0;
            int rc = parse_content_length(colon + 1, &parsed);
            out->content_length = parsed;
            if (rc < 0) {
                out->bad_request = 1;
                return 0;
            }
            if (rc > 0) {
                out->payload_too_large = 1;
                return 0;
            }
        }
    }

    if (out->content_length <= 0)
        return 0;

    out->body = (char *)calloc((size_t)out->content_length + 1, 1);
    if (!out->body) {
        out->bad_request = 1;
        return 0;
    }

    size_t to_copy = buffered_body;
    if (to_copy > (size_t)out->content_length)
        to_copy = (size_t)out->content_length;
    if (to_copy > 0)
        memcpy(out->body, header_buf + header_len, to_copy);

    if (to_copy < (size_t)out->content_length &&
        read_exact(fd, out->body + to_copy,
                   (size_t)out->content_length - to_copy) != 0) {
        out->bad_request = 1;
        return 0;
    }

    out->body_len = (size_t)out->content_length;
    out->body[out->body_len] = '\0';
    return 0;
}

void http_request_free(HttpRequest *req) {
    if (!req) return;
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode_into(const char *src, char *dst, size_t cap) {
    size_t out = 0;

    for (size_t i = 0; src[i]; i++) {
        char ch = src[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            ch = (char)((hi << 4) | lo);
            i += 2;
        }

        if (out + 1 >= cap) return -1;
        dst[out++] = ch;
    }

    dst[out] = '\0';
    return 0;
}

static int extract_query_param(const char *query, char *out, size_t cap) {
    char *copy = NULL;
    char *save = NULL;
    int rc = -1;

    if (!query || !*query) return -1;

    copy = (char *)malloc(strlen(query) + 1);
    if (!copy) return -1;
    strcpy(copy, query);

    for (char *pair = strtok_r(copy, "&", &save);
         pair;
         pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) continue;
        *eq = '\0';

        char key[64];
        if (url_decode_into(pair, key, sizeof(key)) != 0)
            continue;
        if (strcmp(key, "sql") == 0 || strcmp(key, "q") == 0) {
            rc = url_decode_into(eq + 1, out, cap);
            break;
        }
    }

    free(copy);
    return rc;
}

int http_parser_extract_sql(const HttpRequest *req, char *out, size_t cap) {
    if (!req || !out || cap == 0) return -1;

    if (req->method == HTTP_GET)
        return extract_query_param(req->query, out, cap);

    if (req->method == HTTP_POST) {
        if (!req->body || req->body_len == 0)
            return -1;
        if (strncmp(req->body, "sql=", 4) == 0 ||
            strncmp(req->body, "q=", 2) == 0)
            return extract_query_param(req->body, out, cap);
        if (req->body_len >= cap)
            return -1;
        memcpy(out, req->body, req->body_len);
        out[req->body_len] = '\0';
        return 0;
    }

    return -1;
}

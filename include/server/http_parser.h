#ifndef SERVER_HTTP_PARSER_H
#define SERVER_HTTP_PARSER_H

#include <stddef.h>

typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_METHOD_OTHER
} HttpMethod;

typedef struct {
    HttpMethod method;
    char path[256];
    char query[4096];
    char *body;
    size_t body_len;
    int content_length;
    int bad_request;
    int payload_too_large;
} HttpRequest;

int http_parser_read(int fd, HttpRequest *out);
int http_parser_extract_sql(const HttpRequest *req, char *out, size_t cap);
void http_request_free(HttpRequest *req);

#endif

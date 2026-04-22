#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

typedef struct {
    int port;
    int backlog;
} ServerConfig;

typedef struct Server Server;

Server *server_create(const ServerConfig *cfg);
int server_run(Server *s,
               void (*on_accept)(int client_fd, void *ctx),
               void *ctx);
void server_stop(Server *s);
void server_destroy(Server *s);

#endif

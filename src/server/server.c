#include "server/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct Server {
    int listen_fd;
    int stopped;
    ServerConfig cfg;
};

static int open_listen_socket(int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, (socklen_t)sizeof(optval)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

Server *server_create(const ServerConfig *cfg) {
    if (!cfg || cfg->port <= 0 || cfg->port > 65535)
        return NULL;

    Server *s = (Server *)calloc(1, sizeof(Server));
    if (!s) return NULL;

    s->cfg = *cfg;
    if (s->cfg.backlog <= 0)
        s->cfg.backlog = 1024;

    s->listen_fd = open_listen_socket(s->cfg.port, s->cfg.backlog);
    if (s->listen_fd < 0) {
        free(s);
        return NULL;
    }

    return s;
}

int server_run(Server *s,
               void (*on_accept)(int client_fd, void *ctx),
               void *ctx) {
    if (!s || !on_accept) return -1;

    while (!s->stopped) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = (socklen_t)sizeof(client_addr);
        int client_fd = accept(s->listen_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (s->stopped || errno == EBADF) break;
            return -1;
        }

        on_accept(client_fd, ctx);
    }

    return 0;
}

void server_stop(Server *s) {
    if (!s) return;

    s->stopped = 1;
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}

void server_destroy(Server *s) {
    if (!s) return;
    server_stop(s);
    free(s);
}

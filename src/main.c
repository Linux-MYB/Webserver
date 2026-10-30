/*
 * main.c —— 简物 server 入口
 *
 *   一个连接一个线程，简单直接；
 *   SQLite 用 FULLMUTEX + 内部锁，多线程访问安全。
 *
 *   用法: ./jianwu [端口] [数据库路径] [商品库路径]
 */
#include "api.h"
#include "db.h"
#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;
static int g_listen_fd = -1;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
}

static int create_server(int port)
{
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_err("socket 失败: %s", strerror(errno));
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_err("bind %d 失败: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 64) < 0) {
        log_err("listen 失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void *worker(void *arg)
{
    int fd = (int)(intptr_t)arg;
    http_request_t req;
    struct timeval tv;

    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (http_read_request(fd, &req) == 0) {
        if (!api_dispatch(&req)) {
            if (!http_serve_static(&req)) {
                http_text(&req, 404, "404 Not Found");
            }
        }
        http_free_request(&req);
    } else {
        http_request_t bad;
        memset(&bad, 0, sizeof(bad));
        bad.fd = fd;
        http_text(&bad, 400, "Bad Request");
    }

    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *dbpath = "jianwu.db";
    const char *shopdb = "data/123.db";

    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) dbpath = argv[2];
    if (argc > 3) shopdb = argv[3];

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (db_open(dbpath) != 0) {
        log_err("数据库初始化失败");
        return 1;
    }
    db_seed(shopdb);

    g_listen_fd = create_server(port);
    if (g_listen_fd < 0) {
        db_close();
        return 1;
    }

    log_info("简物 server 已启动: http://127.0.0.1:%d  (db=%s)", port, dbpath);

    while (g_run) {
        int fd = accept(g_listen_fd, NULL, NULL);
        pthread_t th;

        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!g_run) break;
            log_warn("accept 失败: %s", strerror(errno));
            continue;
        }

        if (pthread_create(&th, NULL, worker, (void *)(intptr_t)fd) == 0) {
            pthread_detach(th);
        } else {
            log_warn("创建线程失败，丢弃连接");
            close(fd);
        }
    }

    if (g_listen_fd >= 0) close(g_listen_fd);

    db_close();
    log_info("已退出");
    return 0;
}

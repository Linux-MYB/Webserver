#include "api.h"
#include "db.h"
#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define WORKER_NUM 8
#define QUEUE_SIZE 1024
#define MAX_EVENTS 64

static volatile sig_atomic_t g_run = 1;
static int g_listen_fd = -1;
static int g_epfd = -1;

static pthread_t g_workers[WORKER_NUM];
static int g_worker_cnt = 0;

static int g_queue[QUEUE_SIZE];
static int g_head = 0, g_tail = 0, g_count = 0;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_q_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_q_room = PTHREAD_COND_INITIALIZER;

static void queue_push(int fd)
{
    pthread_mutex_lock(&g_q_lock);

    while (g_count == QUEUE_SIZE && g_run) {
        pthread_cond_wait(&g_q_room, &g_q_lock);
    }

    if (!g_run) {
        pthread_mutex_unlock(&g_q_lock);
        close(fd);
        return;
    }

    g_queue[g_tail] = fd;
    g_tail = (g_tail + 1) % QUEUE_SIZE;
    g_count++;

    pthread_cond_signal(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
}

static int queue_pop(void)
{
    int fd;

    pthread_mutex_lock(&g_q_lock);

    while (g_count == 0 && g_run) {
        pthread_cond_wait(&g_q_cond, &g_q_lock);
    }

    if (g_count == 0) {
        pthread_mutex_unlock(&g_q_lock);
        return -1;
    }

    fd = g_queue[g_head];
    g_head = (g_head + 1) % QUEUE_SIZE;
    g_count--;

    pthread_cond_signal(&g_q_room);
    pthread_mutex_unlock(&g_q_lock);
    return fd;
}

static void queue_wake_all(void)
{
    pthread_mutex_lock(&g_q_lock);
    pthread_cond_broadcast(&g_q_cond);
    pthread_cond_broadcast(&g_q_room);
    pthread_mutex_unlock(&g_q_lock);
}

static void handle_conn(int fd)
{
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
}

static void *worker(void *arg)
{
    (void)arg;

    for (;;) {
        int fd = queue_pop();
        if (fd < 0) break;
        handle_conn(fd);
    }

    return NULL;
}

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    queue_wake_all();
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

    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        log_err("设置非阻塞失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_err("bind %d 失败: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        log_err("listen 失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *dbpath = "jianwu.db";
    const char *shopdb = "data/123.db";
    struct epoll_event ev, events[MAX_EVENTS];
    int i;

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

    for (i = 0; i < WORKER_NUM; i++) {
        if (pthread_create(&g_workers[i], NULL, worker, NULL) == 0) {
            g_worker_cnt++;
        } else {
            log_warn("创建工作线程失败");
        }
    }

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) {
        log_err("epoll_create1 失败: %s", strerror(errno));
        close(g_listen_fd);
        db_close();
        return 1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = g_listen_fd;

    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) {
        log_err("epoll_ctl 失败: %s", strerror(errno));
        close(g_epfd);
        close(g_listen_fd);
        db_close();
        return 1;
    }

    log_info("简物 server 已启动: http://127.0.0.1:%d  (db=%s, epoll + %d 线程)",
             port, dbpath, WORKER_NUM);

    while (g_run) {
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, 1000);

        if (n < 0) {
            if (errno == EINTR) continue;
            log_warn("epoll_wait 失败: %s", strerror(errno));
            break;
        }

        for (i = 0; i < n; i++) {
            if (events[i].data.fd != g_listen_fd) continue;

            for (;;) {
                int cfd = accept(g_listen_fd, NULL, NULL);

                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    break;
                }

                queue_push(cfd);
            }
        }
    }

    if (g_epfd >= 0) close(g_epfd);
    if (g_listen_fd >= 0) close(g_listen_fd);

    queue_wake_all();
    for (i = 0; i < g_worker_cnt; i++) {
        pthread_join(g_workers[i], NULL);
    }

    db_close();
    log_info("已退出");
    return 0;
}

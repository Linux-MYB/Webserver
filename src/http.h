/*
 * http.h —— 极简 HTTP/1.1 服务端：请求解析、响应、静态文件
 */
#ifndef JIANWU_HTTP_H
#define JIANWU_HTTP_H

#include <stddef.h>

typedef struct {
    int    fd;
    char   method[8];
    char   path[512];      /* 已 URL 解码，不含 query */
    char   query[1024];
    char   cookie[1024];
    char  *body;           /* 可能为 NULL */
    size_t body_len;
} http_request_t;

/* 读取并解析一个请求。0 成功，-1 失败 */
int  http_read_request(int fd, http_request_t *req);
void http_free_request(http_request_t *req);

/* 取参数：先查 query，再查 body（JSON 或 a=b&c=d） */
int  http_param(http_request_t *req, const char *key, char *out, size_t outsz);
int  http_param_long(http_request_t *req, const char *key, long *out);

/* 取 Cookie 里某个键 */
int  http_cookie(const http_request_t *req, const char *name, char *out, size_t outsz);

/* 响应 */
void http_json(http_request_t *req, int code, const char *msg, const char *data_json);
void http_json_cookie(http_request_t *req, int code, const char *msg,
                      const char *data_json, const char *set_cookie);
void http_text(http_request_t *req, int status, const char *text);

/* 静态文件，成功返回 1 */
int  http_serve_static(http_request_t *req);

#endif /* JIANWU_HTTP_H */

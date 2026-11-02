#ifndef JIANWU_HTTP_H
#define JIANWU_HTTP_H

#include <stddef.h>

typedef struct {
    int    fd;
    char   method[8];
    char   path[512];
    char   query[1024];
    char   cookie[1024];
    char  *body;
    size_t body_len;
} http_request_t;

int  http_read_request(int fd, http_request_t *req);
void http_free_request(http_request_t *req);

int  http_param(http_request_t *req, const char *key, char *out, size_t outsz);
int  http_param_long(http_request_t *req, const char *key, long *out);

int  http_cookie(const http_request_t *req, const char *name, char *out, size_t outsz);

void http_json(http_request_t *req, int code, const char *msg, const char *data_json);
void http_json_cookie(http_request_t *req, int code, const char *msg,
                      const char *data_json, const char *set_cookie);
void http_text(http_request_t *req, int status, const char *text);

int  http_serve_static(http_request_t *req);

#endif

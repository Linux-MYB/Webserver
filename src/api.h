/*
 * api.h —— 业务路由
 */
#ifndef JIANWU_API_H
#define JIANWU_API_H

#include "http.h"

/* 返回 1 表示已处理并已发送响应 */
int api_dispatch(http_request_t *req);

#endif /* JIANWU_API_H */

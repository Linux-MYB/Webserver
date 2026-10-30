#ifndef HEAD_H
#define HEAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>

/* ===== 常量定义 ===== */
#define PORT            8080
#define BUF_SIZE        65536       /* 接收缓冲区大小 */
#define MAX_USERS       1000        /* 最大用户数 */
#define USER_FILE       "users.dat"  /* 用户数据文件 */
#define ACCOUNT_LEN     64          /* 账号最大长度 */
#define PASSWORD_LEN    64          /* 密码最大长度 */
#define HASH_LEN        128          /* 哈希值最大长度 */
#define WEBROOT         "./"         /* 静态文件根目录 */

/* ===== HTTP 请求结构体 ===== */
typedef struct HttpRequest {
    char method[8];              /* 请求方法: GET / POST */
    char url[256];               /* 请求 URL */
    char host[128];              /* Host 头 */
    int  content_length;         /* 请求体长度 */
    char body[BUF_SIZE];         /* 请求体 */
    int  body_len;               /* 实际读到的请求体长度 */
} HttpRequest_t;

/* ===== 用户结构体 ===== */
typedef struct User {
    char account[ACCOUNT_LEN];       /* 账号 */
    char password_hash[HASH_LEN];    /* 密码哈希值 (salt:hash 格式) */
    int  active;                     /* 是否激活: 1=是 0=否 */
} User_t;

/* ===== 函数声明 ===== */

/* 网络相关 */
int  CreateListenSocket(int port);
int  RecvFullHttpRequest(int confd, char *buf, int bufsize);

/* HTTP 请求解析 */
int  ParseHttpRequest(const char *rawbuf, HttpRequest_t *request);

/* JSON 解析 */
int  JsonGetString(const char *json, const char *key, char *output, int outlen);

/* 密码哈希 */
void HashPassword(const char *password, char *output);
int  VerifyPassword(const char *password, const char *stored_hash);

/* 用户存储 */
int  LoadUsers(User_t *users, int *count);
int  SaveUsers(const User_t *users, int count);
User_t *FindUser(User_t *users, int count, const char *account);

/* 业务处理 */
int  HandleRegister(int confd, const HttpRequest_t *request);
int  HandleLogin(int confd, const HttpRequest_t *request);

/* 响应发送 */
void SendJsonResponse(int confd, int code, const char *message);
void SendJsonResponseWithData(int confd, int code, const char *message, const char *key, const char *value);
int  SendFile(int confd, const char *filename);
const char *GetMimeType(const char *filename);

/* 输入校验 */
int  IsValidAccount(const char *account);
int  IsValidPassword(const char *password);

#endif /* HEAD_H */

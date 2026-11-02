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

#define PORT            8080
#define BUF_SIZE        65536
#define MAX_USERS       1000
#define USER_FILE       "users.dat"
#define ACCOUNT_LEN     64
#define PASSWORD_LEN    64
#define HASH_LEN        128
#define WEBROOT         "./"

typedef struct HttpRequest {
    char method[8];
    char url[256];
    char host[128];
    int  content_length;
    char body[BUF_SIZE];
    int  body_len;
} HttpRequest_t;

typedef struct User {
    char account[ACCOUNT_LEN];
    char password_hash[HASH_LEN];
    int  active;
} User_t;

int  CreateListenSocket(int port);
int  RecvFullHttpRequest(int confd, char *buf, int bufsize);

int  ParseHttpRequest(const char *rawbuf, HttpRequest_t *request);

int  JsonGetString(const char *json, const char *key, char *output, int outlen);

void HashPassword(const char *password, char *output);
int  VerifyPassword(const char *password, const char *stored_hash);

int  LoadUsers(User_t *users, int *count);
int  SaveUsers(const User_t *users, int count);
User_t *FindUser(User_t *users, int count, const char *account);

int  HandleRegister(int confd, const HttpRequest_t *request);
int  HandleLogin(int confd, const HttpRequest_t *request);

void SendJsonResponse(int confd, int code, const char *message);
void SendJsonResponseWithData(int confd, int code, const char *message, const char *key, const char *value);
int  SendFile(int confd, const char *filename);
const char *GetMimeType(const char *filename);

int  IsValidAccount(const char *account);
int  IsValidPassword(const char *password);

#endif

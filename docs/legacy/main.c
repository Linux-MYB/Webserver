#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>

typedef struct HttpRequest
{
    char *pmethod;
    char *purl;
} HttpRequest_t;

int CreateListenSocket(int port)
{
    int sockfd;
    int ret;
    int opt = 1;
    struct sockaddr_in seraddr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == sockfd)
    {
        perror("fail to socket");
        return -1;
    }

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    seraddr.sin_family = AF_INET;
    seraddr.sin_port = htons(port);
    seraddr.sin_addr.s_addr = INADDR_ANY;

    ret = bind(sockfd, (struct sockaddr *)&seraddr, sizeof(seraddr));
    if (-1 == ret)
    {
        perror("fail to bind");
        close(sockfd);
        return -1;
    }

    ret = listen(sockfd, 10);
    if (-1 == ret)
    {
        perror("fail to listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int RecvHttpRequest(int confd, char *prevbuf, int len)
{
    ssize_t nret;
    ssize_t total = 0;

    while (total < len - 1)
    {
        nret = recv(confd, prevbuf + total, len - 1 - total, 0);
        if (-1 == nret)
        {
            perror("fail to recv");
            return -1;
        }
        else if (0 == nret)
        {
            break;
        }
        total += nret;
        prevbuf[total] = '\0';
        if (strstr(prevbuf, "\r\n\r\n") != NULL)
        {
            break;
        }
    }
    return total;
}

int SendHttpResponse(int confd, HttpRequest_t *pTmpRequest)
{
    char tmpbuff[4096];
    char filename[256];
    int fd = -1;
    ssize_t nret = 0;
    int header_len = 0;

    if (pTmpRequest->purl == NULL)
    {
        char badreq[] = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 0\r\n\r\n";
        send(confd, badreq, strlen(badreq), 0);
        return -1;
    }

    if (0 == strcmp(pTmpRequest->purl, "/"))
    {
        snprintf(filename, sizeof(filename), "./index.html");
    }
    else
    {
        snprintf(filename, sizeof(filename), ".%s", pTmpRequest->purl);
    }

    if (strstr(filename, "..") != NULL)
    {
        char forbid[] = "HTTP/1.1 403 Forbidden\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
        send(confd, forbid, strlen(forbid), 0);
        return -1;
    }

    fd = open(filename, O_RDONLY);
    if (-1 == fd)
    {
        char notfound[256];
        int body_len;
        body_len = snprintf(NULL, 0, "<html><body>404 Not Found: %s</body></html>", filename);
        header_len = snprintf(tmpbuff, sizeof(tmpbuff),
            "HTTP/1.1 404 Not Found\r\n"
            "Server: TinyServer\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            body_len);
        send(confd, tmpbuff, header_len, 0);
        snprintf(tmpbuff, sizeof(tmpbuff), "<html><body>404 Not Found: %s</body></html>", filename);
        send(confd, tmpbuff, body_len, 0);
        return 0;
    }

    strcpy(tmpbuff, "HTTP/1.1 200 OK\r\n");
    strcat(tmpbuff, "Server: TinyServer\r\n");
    strcat(tmpbuff, "Content-Type: text/html; charset=utf-8\r\n");
    strcat(tmpbuff, "Connection: close\r\n");
    strcat(tmpbuff, "\r\n");

    header_len = strlen(tmpbuff);
    nret = send(confd, tmpbuff, header_len, 0);
    if (-1 == nret)
    {
        perror("fail to send header");
        close(fd);
        return -1;
    }

    while (1)
    {
        nret = read(fd, tmpbuff, sizeof(tmpbuff));
        if (-1 == nret)
        {
            perror("fail to read");
            close(fd);
            return -1;
        }
        else if (0 == nret)
        {
            break;
        }

        nret = send(confd, tmpbuff, nret, 0);
        if (-1 == nret)
        {
            perror("fail to send body");
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

int ParseHttpRequest(char *prevbuff, HttpRequest_t *ptmpquest)
{
    ptmpquest->pmethod = strtok(prevbuff, " ");
    ptmpquest->purl = strtok(NULL, " ");

    if (ptmpquest->pmethod == NULL || ptmpquest->purl == NULL)
    {
        return -1;
    }
    return 0;
}

char *FindBody(char *buf, int bufsize)
{
    int i;
    for (i = 0; i < bufsize - 4; i++)
    {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i + 4;
    }
    return NULL;
}

int JsonGetString(const char *json, const char *key, char *output, int outlen)
{
    char pattern[64];
    const char *p, *end;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (p == NULL)
        return -1;
    p += strlen(pattern);
    while (*p != '\0' && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;
    if (*p != '"')
        return -1;
    p++;
    end = strchr(p, '"');
    if (end == NULL)
        return -1;
    int len = (int)(end - p);
    if (len >= outlen)
        len = outlen - 1;
    strncpy(output, p, len);
    output[len] = '\0';
    return 0;
}

void HashPassword(const char *password, char *output)
{
    int salt = rand();
    char combined[128];
    unsigned long hash = 5381;
    int i = 0;

    snprintf(combined, sizeof(combined), "%d%s", salt, password);
    while (combined[i] != '\0')
    {
        hash = ((hash << 5) + hash) + (unsigned char)combined[i];
        i++;
    }
    snprintf(output, 128, "%d:%lx", salt, hash);
}

int VerifyPassword(const char *password, const char *stored_hash)
{
    char salt_str[32];
    char stored_val[64];
    char combined[128];
    unsigned long hash = 5381;
    int i = 0;
    const char *colon;

    colon = strchr(stored_hash, ':');
    if (colon == NULL)
        return 0;
    int slen = (int)(colon - stored_hash);
    if (slen >= (int)sizeof(salt_str))
        slen = sizeof(salt_str) - 1;
    strncpy(salt_str, stored_hash, slen);
    salt_str[slen] = '\0';
    strncpy(stored_val, colon + 1, sizeof(stored_val) - 1);
    stored_val[sizeof(stored_val) - 1] = '\0';
    snprintf(combined, sizeof(combined), "%s%s", salt_str, password);
    while (combined[i] != '\0')
    {
        hash = ((hash << 5) + hash) + (unsigned char)combined[i];
        i++;
    }
    char computed[64];
    snprintf(computed, sizeof(computed), "%lx", hash);
    return strcmp(computed, stored_val) == 0;
}

void SendJsonResponse(int confd, int code, const char *message)
{
    char body[256];
    char header[256];
    int blen, hlen;

    blen = sprintf(body, "{\"code\":%d,\"message\":\"%s\"}", code, message);
    hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\n"
        "Server: TinyServer\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", blen);
    send(confd, header, hlen, 0);
    send(confd, body, blen, 0);
}

void SendJsonResponseWithData(int confd, int code, const char *message,
                               const char *key, const char *value)
{
    char body[256];
    char header[256];
    int blen, hlen;

    blen = sprintf(body, "{\"code\":%d,\"message\":\"%s\",\"%s\":\"%s\"}",
                   code, message, key, value);
    hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\n"
        "Server: TinyServer\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", blen);
    send(confd, header, hlen, 0);
    send(confd, body, blen, 0);
}

int HandleRegister(int confd, char *recvbuff)
{
    char account[64];
    char password[64];
    char hash[128];
    char *body;
    FILE *fp;
    char line[256];
    char exist_account[64];
    char exist_hash[128];

    body = FindBody(recvbuff, 8192);
    if (body == NULL ||
        JsonGetString(body, "account", account, sizeof(account)) != 0 ||
        JsonGetString(body, "password", password, sizeof(password)) != 0)
    {
        SendJsonResponse(confd, -1, "请求参数错误");
        return -1;
    }

    fp = fopen("users.txt", "r");
    if (fp != NULL)
    {
        while (fgets(line, sizeof(line), fp) != NULL)
        {
            if (sscanf(line, "%63s %127s", exist_account, exist_hash) == 2)
            {
                if (strcmp(exist_account, account) == 0)
                {
                    fclose(fp);
                    SendJsonResponse(confd, -1, "该账号已存在");
                    return -1;
                }
            }
        }
        fclose(fp);
    }

    HashPassword(password, hash);

    fp = fopen("users.txt", "a");
    if (fp == NULL)
    {
        SendJsonResponse(confd, -1, "服务器内部错误");
        return -1;
    }
    fprintf(fp, "%s %s\n", account, hash);
    fclose(fp);

    printf("[注册] 新用户: %s\n", account);
    SendJsonResponse(confd, 0, "注册成功");
    return 0;
}

int HandleLogin(int confd, char *recvbuff)
{
    char account[64];
    char password[64];
    char *body;
    FILE *fp;
    char line[256];
    char exist_account[64];
    char exist_hash[128];
    int found = 0;

    body = FindBody(recvbuff, 8192);
    if (body == NULL ||
        JsonGetString(body, "account", account, sizeof(account)) != 0 ||
        JsonGetString(body, "password", password, sizeof(password)) != 0)
    {
        SendJsonResponse(confd, -1, "请求参数错误");
        return -1;
    }

    fp = fopen("users.txt", "r");
    if (fp == NULL)
    {
        SendJsonResponse(confd, -1, "账号不存在");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (sscanf(line, "%63s %127s", exist_account, exist_hash) == 2)
        {
            if (strcmp(exist_account, account) == 0)
            {
                found = 1;
                if (VerifyPassword(password, exist_hash))
                {
                    fclose(fp);
                    printf("[登录] 用户: %s\n", account);
                    SendJsonResponseWithData(confd, 0, "登录成功", "account", account);
                    return 0;
                }
                else
                {
                    fclose(fp);
                    SendJsonResponse(confd, -1, "密码错误");
                    return -1;
                }
            }
        }
    }
    fclose(fp);

    if (!found)
    {
        SendJsonResponse(confd, -1, "账号不存在");
    }
    return -1;
}

int main(void)
{
    int sockfd;
    int confd;
    char recvbuff[8192];
    HttpRequest_t Tmpquest;

    srand((unsigned int)time(NULL));

    sockfd = CreateListenSocket(8080);
    if (-1 == sockfd)
    {
        return -1;
    }

    while (1)
    {
        memset(recvbuff, 0, sizeof(recvbuff));
        confd = accept(sockfd, NULL, NULL);
        if (-1 == confd)
        {
            perror("fail to accept");
            continue;
        }

        RecvHttpRequest(confd, recvbuff, sizeof(recvbuff));
        printf("==========================recv========================\n");
        printf("recv:%s", recvbuff);
        printf("==========================recv========================\n");

        if (ParseHttpRequest(recvbuff, &Tmpquest) == 0)
        {

            if (strcmp(Tmpquest.pmethod, "POST") == 0 &&
                strcmp(Tmpquest.purl, "/api/register") == 0)
            {
                HandleRegister(confd, recvbuff);
            }

            else if (strcmp(Tmpquest.pmethod, "POST") == 0 &&
                     strcmp(Tmpquest.purl, "/api/login") == 0)
            {
                HandleLogin(confd, recvbuff);
            }

            else
            {
                SendHttpResponse(confd, &Tmpquest);
            }
        }
        else
        {
            char badreq[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            send(confd, badreq, strlen(badreq), 0);
        }

        close(confd);
    }

    close(sockfd);
    return 0;
}

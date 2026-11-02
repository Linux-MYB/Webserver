#include "http.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_HEADER   (32 * 1024)
#define MAX_BODY     (1024 * 1024)

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static char *find_header_end(char *buf, size_t len)
{
    size_t i;

    if (len < 4) return NULL;

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

int http_read_request(int fd, http_request_t *req)
{
    char  *buf = NULL;
    size_t cap = MAX_HEADER;
    size_t len = 0;
    char  *hdrend;
    char  *line, *save = NULL;
    char   target[1024];
    long   content_length = 0;

    memset(req, 0, sizeof(*req));
    req->fd = fd;

    buf = (char *)malloc(cap + 1);
    if (!buf) return -1;

    for (;;) {
        ssize_t n;

        if (len >= cap) {
            free(buf);
            return -1;
        }

        n = recv(fd, buf + len, cap - len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (n == 0) {
            free(buf);
            return -1;
        }

        len += (size_t)n;
        buf[len] = '\0';

        hdrend = find_header_end(buf, len);
        if (hdrend) break;
    }

    {
        size_t header_len = (size_t)(hdrend - buf) + 4;

        char *head = (char *)malloc(header_len + 1);
        if (!head) { free(buf); return -1; }
        memcpy(head, buf, header_len);
        head[header_len] = '\0';

        line = strtok_r(head, "\r\n", &save);
        if (!line || sscanf(line, "%7s %1023s", req->method, target) != 2) {
            free(head);
            free(buf);
            return -1;
        }

        while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
            if (strncasecmp(line, "Content-Length:", 15) == 0) {
                content_length = strtol(line + 15, NULL, 10);
            } else if (strncasecmp(line, "Cookie:", 7) == 0) {
                const char *v = line + 7;
                while (*v == ' ') v++;
                snprintf(req->cookie, sizeof(req->cookie), "%s", v);
            }
        }
        free(head);

        if (content_length > 0) {
            size_t avail = len - header_len;
            size_t need  = (size_t)content_length;

            if (need > MAX_BODY) { free(buf); return -1; }

            if (avail < need) {
                if (header_len + need > cap) {
                    size_t ncap = header_len + need + 1;
                    char *nb = (char *)realloc(buf, ncap);
                    if (!nb) { free(buf); return -1; }
                    buf = nb;
                    cap = ncap;
                }
                while (avail < need) {
                    ssize_t n = recv(fd, buf + header_len + avail, need - avail, 0);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        free(buf);
                        return -1;
                    }
                    if (n == 0) break;
                    avail += (size_t)n;
                }
                len = header_len + avail;
            }

            req->body_len = (avail < need) ? avail : need;
            req->body = (char *)malloc(req->body_len + 1);
            if (!req->body) { free(buf); return -1; }
            memcpy(req->body, buf + header_len, req->body_len);
            req->body[req->body_len] = '\0';
        }
    }

    {
        char *qm = strchr(target, '?');
        char raw_path[512];

        if (qm) {
            *qm = '\0';
            snprintf(req->query, sizeof(req->query), "%s", qm + 1);
        }

        snprintf(raw_path, sizeof(raw_path), "%s", target);
        if (url_decode(raw_path, req->path, sizeof(req->path)) != 0) {
            snprintf(req->path, sizeof(req->path), "%s", raw_path);
        }
    }

    free(buf);
    return 0;
}

void http_free_request(http_request_t *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

static int parse_kv(const char *s, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = s;

    if (!s) return -1;

    while (p && *p) {
        const char *eq = strchr(p, '=');

        if (!eq) break;

        if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *v = eq + 1;
            const char *amp = strchr(v, '&');
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            char tmp[1024];

            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, v, vlen);
            tmp[vlen] = '\0';

            return url_decode(tmp, out, outsz);
        }

        p = strchr(p, '&');
        if (p) p++;
    }
    return -1;
}

int http_param(http_request_t *req, const char *key, char *out, size_t outsz)
{
    if (req->query[0] && parse_kv(req->query, key, out, outsz) == 0) return 0;

    if (req->body && req->body_len > 0) {
        const char *b = req->body;

        while (*b == ' ' || *b == '\t') b++;

        if (*b == '{') return json_get_str(req->body, key, out, outsz);

        if (parse_kv(req->body, key, out, outsz) == 0) return 0;
    }

    return -1;
}

int http_param_long(http_request_t *req, const char *key, long *out)
{
    char tmp[32];

    if (http_param(req, key, tmp, sizeof(tmp)) != 0) return -1;
    *out = strtol(tmp, NULL, 10);
    return 0;
}

int http_cookie(const http_request_t *req, const char *name, char *out, size_t outsz)
{
    const char *p = req->cookie;
    size_t nlen = strlen(name);

    while (p && *p) {
        const char *eq;

        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;

        eq = strchr(p, '=');
        if (!eq) break;

        if ((size_t)(eq - p) == nlen && strncmp(p, name, nlen) == 0) {
            const char *v = eq + 1;
            const char *semi = strchr(v, ';');
            size_t vlen = semi ? (size_t)(semi - v) : strlen(v);

            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }

        p = strchr(p, ';');
        if (p) p++;
    }
    return -1;
}

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

void http_json_cookie(http_request_t *req, int code, const char *msg,
                      const char *data_json, const char *set_cookie)
{
    strbuf_t body;
    strbuf_t head;
    char esc[512];

    sb_init(&body);
    sb_init(&head);

    json_escape(msg ? msg : "", esc, sizeof(esc));

    sb_printf(&body, "{\"code\":%d,\"msg\":\"%s\"", code, esc);
    if (data_json) {
        sb_puts(&body, ",\"data\":");
        sb_puts(&body, data_json);
    }
    sb_puts(&body, "}");

    sb_printf(&head,
              "HTTP/1.1 200 OK\r\n"
              "Server: JianWu\r\n"
              "Content-Type: application/json; charset=utf-8\r\n"
              "Content-Length: %zu\r\n"
              "Cache-Control: no-store\r\n"
              "Connection: close\r\n",
              body.len);

    if (set_cookie && *set_cookie) {
        sb_printf(&head, "Set-Cookie: %s\r\n", set_cookie);
    }
    sb_puts(&head, "\r\n");

    write_all(req->fd, head.buf, head.len);
    write_all(req->fd, body.buf, body.len);

    sb_free(&head);
    sb_free(&body);
}

void http_json(http_request_t *req, int code, const char *msg, const char *data_json)
{
    http_json_cookie(req, code, msg, data_json, NULL);
}

void http_text(http_request_t *req, int status, const char *text)
{
    char head[256];
    int n;
    size_t blen = text ? strlen(text) : 0;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Server: JianWu\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 status, status_text(status), blen);

    write_all(req->fd, head, (size_t)n);
    if (blen) write_all(req->fd, text, blen);
}

int http_serve_static(http_request_t *req)
{
    char filename[512];
    char head[512];
    struct stat st;
    int fd, n;

    if (strcmp(req->path, "/") == 0) {
        snprintf(filename, sizeof(filename), "www/index.html");
    } else {
        snprintf(filename, sizeof(filename), "www%s", req->path);
    }

    if (strstr(filename, "..") != NULL) return 0;

    if (stat(filename, &st) != 0 || !S_ISREG(st.st_mode)) return 0;

    fd = open(filename, O_RDONLY);
    if (fd < 0) return 0;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\n"
                 "Server: JianWu\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lld\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n\r\n",
                 mime_type(filename), (long long)st.st_size);

    write_all(req->fd, head, (size_t)n);

    for (;;) {
        char buf[8192];
        ssize_t r = read(fd, buf, sizeof(buf));

        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;

        if (write_all(req->fd, buf, (size_t)r) != 0) break;
    }

    close(fd);
    return 1;
}

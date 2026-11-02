#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void log_out(const char *lvl, const char *fmt, va_list ap)
{
    char tbuf[32];
    time_t now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(stderr, "[%s] %-5s ", tbuf, lvl);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_out("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_out("WARN", fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_out("ERROR", fmt, ap);
    va_end(ap);
}

void sb_init(strbuf_t *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(strbuf_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_reserve(strbuf_t *sb, size_t need)
{
    size_t ncap;
    char *p;

    if (sb->buf && sb->len + need + 1 <= sb->cap) return 0;

    ncap = sb->cap ? sb->cap : 512;
    while (ncap < sb->len + need + 1) ncap *= 2;

    p = (char *)realloc(sb->buf, ncap);
    if (!p) return -1;

    sb->buf = p;
    sb->cap = ncap;
    return 0;
}

void sb_puts(strbuf_t *sb, const char *s)
{
    size_t n;

    if (!s) return;
    n = strlen(s);
    if (sb_reserve(sb, n) != 0) return;

    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void sb_putc(strbuf_t *sb, char c)
{
    if (sb_reserve(sb, 1) != 0) return;
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

void sb_printf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (sb_reserve(sb, (size_t)n) != 0) return;

    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);

    sb->len += (size_t)n;
}

char *sb_cstr(strbuf_t *sb)
{
    return sb->buf ? sb->buf : (char *)"";
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;

    if (!in || !out || outsz == 0) return -1;

    while (*in) {
        if (o + 1 >= outsz) return -1;

        if (*in == '+') {
            out[o++] = ' ';
            in++;
        } else if (*in == '%' && in[1] && in[2]) {
            int hi = hexval(in[1]);
            int lo = hexval(in[2]);
            if (hi < 0 || lo < 0) return -1;
            out[o++] = (char)((hi << 4) | lo);
            in += 3;
        } else {
            out[o++] = *in++;
        }
    }

    out[o] = '\0';
    return 0;
}

void hex_encode(const unsigned char *in, size_t len, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        out[i * 2]     = HEX[in[i] >> 4];
        out[i * 2 + 1] = HEX[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

int hex_decode(const char *in, unsigned char *out, size_t outsz)
{
    size_t n = strlen(in);
    size_t i;

    if (n % 2 != 0 || n / 2 > outsz) return -1;

    for (i = 0; i < n; i += 2) {
        int hi = hexval(in[i]);
        int lo = hexval(in[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;

    if (!out || outsz == 0) return;
    if (!in) { out[0] = '\0'; return; }

    while (*in && o + 8 < outsz) {
        unsigned char c = (unsigned char)*in++;

        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
            } else {
                out[o++] = (char)c;
            }
            break;
        }
    }

    out[o] = '\0';
}

static const char *find_value(const char *json, const char *key)
{
    char pat[64];
    const char *p;

    if (!json || !key || strlen(key) > 48) return NULL;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = json;

    while ((p = strstr(p, pat)) != NULL) {
        char prev = (p == json) ? '{' : p[-1];

        if (prev == '{' || prev == ',' || isspace((unsigned char)prev)) {
            const char *q = p + strlen(pat);
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == ':') {
                q++;
                while (*q && isspace((unsigned char)*q)) q++;
                return q;
            }
        }
        p++;
    }
    return NULL;
}

int json_get_str(const char *json, const char *key, char *out, size_t outsz)
{
    const char *p;
    size_t o = 0;

    if (!out || outsz == 0) return -1;
    out[0] = '\0';

    p = find_value(json, key);
    if (!p) return -1;

    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            char c = *p++;

            if (c == '\\' && *p) {
                char e = *p++;
                switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case '/': c = '/';  break;
                default: c = e;     break;
                }
            }
            if (o + 1 < outsz) out[o++] = c;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
            if (o + 1 < outsz) out[o++] = *p;
            p++;
        }
    }

    out[o] = '\0';
    return 0;
}

int json_get_long(const char *json, const char *key, long *out)
{
    const char *p = find_value(json, key);
    char *end = NULL;
    long v;

    if (!p || !out) return -1;

    v = strtol(p, &end, 10);
    if (end == p) return -1;

    *out = v;
    return 0;
}

const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (!dot) return "application/octet-stream";

    if (str_ieq(dot, ".html") || str_ieq(dot, ".htm")) return "text/html; charset=utf-8";
    if (str_ieq(dot, ".css"))  return "text/css; charset=utf-8";
    if (str_ieq(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (str_ieq(dot, ".json")) return "application/json; charset=utf-8";
    if (str_ieq(dot, ".svg"))  return "image/svg+xml";
    if (str_ieq(dot, ".png"))  return "image/png";
    if (str_ieq(dot, ".jpg") || str_ieq(dot, ".jpeg")) return "image/jpeg";
    if (str_ieq(dot, ".gif"))  return "image/gif";
    if (str_ieq(dot, ".ico"))  return "image/x-icon";
    if (str_ieq(dot, ".woff2")) return "font/woff2";

    return "application/octet-stream";
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

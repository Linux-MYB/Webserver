#ifndef JIANWU_UTIL_H
#define JIANWU_UTIL_H

#include <stddef.h>

void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err(const char *fmt, ...);

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} strbuf_t;

void  sb_init(strbuf_t *sb);
void  sb_free(strbuf_t *sb);
void  sb_puts(strbuf_t *sb, const char *s);
void  sb_putc(strbuf_t *sb, char c);
void  sb_printf(strbuf_t *sb, const char *fmt, ...);
char *sb_cstr(strbuf_t *sb);

int   url_decode(const char *in, char *out, size_t outsz);
void  hex_encode(const unsigned char *in, size_t len, char *out);
int   hex_decode(const char *in, unsigned char *out, size_t outsz);
void  json_escape(const char *in, char *out, size_t outsz);

int   json_get_str(const char *json, const char *key, char *out, size_t outsz);
int   json_get_long(const char *json, const char *key, long *out);

const char *mime_type(const char *path);
int   str_ieq(const char *a, const char *b);

#endif

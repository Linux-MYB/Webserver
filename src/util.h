/*
 * util.h —— 日志、可变字符串、URL/JSON/十六进制小工具
 */
#ifndef JIANWU_UTIL_H
#define JIANWU_UTIL_H

#include <stddef.h>

/* ---------------- 日志 ---------------- */
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err(const char *fmt, ...);

/* ---------------- 可变字符串 ---------------- */
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
char *sb_cstr(strbuf_t *sb);          /* 空缓冲返回 "" */

/* ---------------- 编码工具 ---------------- */
int   url_decode(const char *in, char *out, size_t outsz);
void  hex_encode(const unsigned char *in, size_t len, char *out);
int   hex_decode(const char *in, unsigned char *out, size_t outsz);
void  json_escape(const char *in, char *out, size_t outsz);

/* ---------------- 极简 JSON 取值 ----------------
 * 只支持扁平对象，够处理 {"account":"x","password":"y"} 这种请求体
 */
int   json_get_str(const char *json, const char *key, char *out, size_t outsz);
int   json_get_long(const char *json, const char *key, long *out);

/* ---------------- 其它 ---------------- */
const char *mime_type(const char *path);
int   str_ieq(const char *a, const char *b);

#endif /* JIANWU_UTIL_H */

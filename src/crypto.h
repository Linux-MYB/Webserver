/*
 * crypto.h —— SHA-256 / HMAC / PBKDF2 / 随机数
 *
 * 不依赖 OpenSSL，纯 C 实现，方便交叉编译到板子。
 */
#ifndef JIANWU_CRYPTO_H
#define JIANWU_CRYPTO_H

#include <stddef.h>

#define SHA256_LEN 32

void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN]);
void hmac_sha256(const void *key, size_t key_len,
                 const void *msg, size_t msg_len,
                 unsigned char out[SHA256_LEN]);
void pbkdf2_sha256(const char *password, const unsigned char *salt, size_t salt_len,
                   int iterations, unsigned char out[SHA256_LEN]);

/* /dev/urandom，失败返回 -1 */
int random_bytes(void *buf, size_t len);
/* 生成 n 字节随机数的十六进制串，out 至少 2n+1 */
int random_hex(size_t nbytes, char *out, size_t outsz);

/*
 * 密码哈希，格式：
 *   pbkdf2-sha256$<迭代次数>$<salt hex>$<hash hex>
 */
int password_hash(const char *password, char *out, size_t outsz);
int password_verify(const char *password, const char *stored);

#endif /* JIANWU_CRYPTO_H */

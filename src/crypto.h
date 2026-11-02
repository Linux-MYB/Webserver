#ifndef JIANWU_CRYPTO_H
#define JIANWU_CRYPTO_H

#include <stddef.h>

#define SHA256_LEN 32

void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN]);

int random_bytes(void *buf, size_t len);

int random_hex(size_t nbytes, char *out, size_t outsz);

int password_hash(const char *password, char *out, size_t outsz);
int password_verify(const char *password, const char *stored);

#endif

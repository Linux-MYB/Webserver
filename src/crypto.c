#include "crypto.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ==================== SHA-256 ==================== */

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

typedef struct {
    uint32_t h[8];
    uint64_t total;          /* 已处理字节数 */
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx_t;

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->total = 0;
    ctx->buflen = 0;
}

static void sha256_block(sha256_ctx_t *ctx, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t s0, s1, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) |
               ((uint32_t)p[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        s1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        t1 = h + s1 + ((e & f) ^ ((~e) & g)) + K256[i] + w[i];
        s0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->total += len;

    if (ctx->buflen) {
        size_t need = 64 - ctx->buflen;
        if (len < need) {
            memcpy(ctx->buf + ctx->buflen, p, len);
            ctx->buflen += len;
            return;
        }
        memcpy(ctx->buf + ctx->buflen, p, need);
        sha256_block(ctx, ctx->buf);
        p += need;
        len -= need;
        ctx->buflen = 0;
    }

    while (len >= 64) {
        sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_LEN])
{
    uint64_t bits = ctx->total * 8u;
    uint8_t pad[72];
    size_t padlen;
    int i;

    /* 0x80 + 0..63 个 0 + 8 字节长度 */
    pad[0] = 0x80;
    memset(pad + 1, 0, sizeof(pad) - 1);

    padlen = (ctx->buflen < 56) ? (56 - ctx->buflen) : (120 - ctx->buflen);
    sha256_update(ctx, pad, padlen);

    for (i = 0; i < 8; i++) {
        pad[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_update(ctx, pad, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN])
{
    sha256_ctx_t ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ==================== HMAC-SHA256 ==================== */

void hmac_sha256(const void *key, size_t key_len,
                 const void *msg, size_t msg_len,
                 unsigned char out[SHA256_LEN])
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[SHA256_LEN];
    sha256_ctx_t ctx;
    size_t i;

    memset(k, 0, sizeof(k));

    if (key_len > 64) {
        sha256(key, key_len, k);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }

    for (i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5C);
    }

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, SHA256_LEN);
    sha256_final(&ctx, out);
}

/* ==================== PBKDF2 ==================== */

void pbkdf2_sha256(const char *password, const unsigned char *salt, size_t salt_len,
                   int iterations, unsigned char out[SHA256_LEN])
{
    uint8_t u[SHA256_LEN];
    uint8_t t[SHA256_LEN];
    uint8_t block[256];
    size_t plen = strlen(password);
    int i, j;

    if (salt_len + 4 > sizeof(block)) salt_len = sizeof(block) - 4;

    /* dkLen = 32 = hLen，所以只需要一块 */
    memcpy(block, salt, salt_len);
    block[salt_len + 0] = 0;
    block[salt_len + 1] = 0;
    block[salt_len + 2] = 0;
    block[salt_len + 3] = 1;

    hmac_sha256(password, plen, block, salt_len + 4, u);
    memcpy(t, u, SHA256_LEN);

    for (i = 1; i < iterations; i++) {
        hmac_sha256(password, plen, u, SHA256_LEN, u);
        for (j = 0; j < SHA256_LEN; j++) t[j] ^= u[j];
    }

    memcpy(out, t, SHA256_LEN);
}

/* ==================== 随机数 ==================== */

int random_bytes(void *buf, size_t len)
{
    int fd;
    size_t got = 0;
    ssize_t n;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;

    while (got < len) {
        n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }

    close(fd);
    return 0;
}

int random_hex(size_t nbytes, char *out, size_t outsz)
{
    unsigned char tmp[64];

    if (nbytes > sizeof(tmp) || outsz < nbytes * 2 + 1) return -1;
    if (random_bytes(tmp, nbytes) != 0) return -1;

    hex_encode(tmp, nbytes, out);
    return 0;
}

/* ==================== 密码 ==================== */

#define PBKDF2_ITER 10000

int password_hash(const char *password, char *out, size_t outsz)
{
    unsigned char salt[16];
    unsigned char dk[SHA256_LEN];
    char salt_hex[33];
    char hash_hex[65];
    int n;

    if (random_bytes(salt, sizeof(salt)) != 0) return -1;

    pbkdf2_sha256(password, salt, sizeof(salt), PBKDF2_ITER, dk);
    hex_encode(salt, sizeof(salt), salt_hex);
    hex_encode(dk, sizeof(dk), hash_hex);

    n = snprintf(out, outsz, "pbkdf2-sha256$%d$%s$%s",
                 PBKDF2_ITER, salt_hex, hash_hex);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

int password_verify(const char *password, const char *stored)
{
    char tmp[256];
    char *save = NULL;
    char *algo, *iter_s, *salt_s, *hash_s;
    unsigned char salt[32];
    unsigned char calc[SHA256_LEN];
    char calc_hex[65];
    int iter, salt_len;
    unsigned int diff = 0;
    size_t i;

    if (!password || !stored || strlen(stored) >= sizeof(tmp)) return 0;
    strcpy(tmp, stored);

    algo   = strtok_r(tmp, "$", &save);
    iter_s = strtok_r(NULL, "$", &save);
    salt_s = strtok_r(NULL, "$", &save);
    hash_s = strtok_r(NULL, "$", &save);

    if (!algo || !iter_s || !salt_s || !hash_s) return 0;
    if (strcmp(algo, "pbkdf2-sha256") != 0) return 0;

    iter = atoi(iter_s);
    if (iter < 1 || iter > 1000000) return 0;

    salt_len = hex_decode(salt_s, salt, sizeof(salt));
    if (salt_len <= 0) return 0;

    if (strlen(hash_s) != 64) return 0;

    pbkdf2_sha256(password, salt, (size_t)salt_len, iter, calc);
    hex_encode(calc, sizeof(calc), calc_hex);

    /* 常数时间比较 */
    for (i = 0; i < 64; i++) {
        diff |= (unsigned int)((unsigned char)calc_hex[i] ^ (unsigned char)hash_s[i]);
    }

    return diff == 0;
}

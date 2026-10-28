/*
 * MD5 (RFC 1321).
 *
 * Vendored rather than pulled from a library because the kernel has no
 * third-party crypto dependency, and because AF_ALG needs the transform to be
 * allocation-free on the socket path. Only AF_ALG consumes this today.
 *
 * Verified against all seven RFC 1321 test vectors plus a streaming-versus-
 * one-shot equivalence check. MD5 is cryptographically broken and is here for
 * compatibility -- musl's crypt(3) probes AF_ALG for exactly this hash -- not as
 * a security primitive.
 */

#include "net/md5.h"

#include <string.h>

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(a, b, c, d, x, s, t)                    \
    do {                                             \
        (a) += (f) + (x) + (t);                      \
        (a) = ROL((a), (s));                         \
        (a) += (b);                                  \
    } while (0)

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint8_t md5_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_transform(md5_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = F(b, c, d);
            g = i;
        } else if (i < 32) {
            f = G(b, c, d);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = H(b, c, d);
            g = (3 * i + 5) & 15;
        } else {
            f = I(b, c, d);
            g = (7 * i) & 15;
        }
        STEP(a, b, c, d, x[g], md5_shift[i], md5_k[i]);
        /* Rotate the register roles instead of shifting four variables. */
        uint32_t t = d;
        d = c;
        c = b;
        b = a;
        a = t;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;

    __builtin_memset(x, 0, sizeof(x));
}

void md5_init(md5_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->count = 0;
    ctx->buflen = 0;
}

void md5_update(md5_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->count += (uint64_t)len;

    if (ctx->buflen) {
        size_t need = 64 - ctx->buflen;
        size_t take = (len < need) ? len : need;
        __builtin_memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            md5_transform(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }

    while (len >= 64) {
        md5_transform(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        __builtin_memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void md5_final(md5_ctx_t *ctx, uint8_t out[16])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad[72];
    size_t padlen = (ctx->buflen < 56) ? (56 - ctx->buflen) : (120 - ctx->buflen);

    __builtin_memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++)
        pad[padlen + i] = (uint8_t)(bits >> (8 * i));
    md5_update(ctx, pad, padlen + 8);

    for (int i = 0; i < 4; i++) {
        out[i * 4] = (uint8_t)ctx->state[i];
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
    }

    __builtin_memset(ctx, 0, sizeof(*ctx));
}

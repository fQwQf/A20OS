#ifndef _NET_MD5_H
#define _NET_MD5_H

#include <stddef.h>
#include <stdint.h>

/* MD5 (RFC 1321). Broken as a hash; present for compatibility with musl's
 * crypt(3), which probes AF_ALG for exactly this transform. */
#define MD5_DIGEST_LEN 16
#define MD5_BLOCK_LEN 64

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t buf[MD5_BLOCK_LEN];
    uint32_t buflen;
} md5_ctx_t;

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const void *data, size_t len);
void md5_final(md5_ctx_t *ctx, uint8_t out[MD5_DIGEST_LEN]);

#endif /* _NET_MD5_H */

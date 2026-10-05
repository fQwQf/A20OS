/*
 * AF_ALG — kernel crypto provider for the Linux crypto socket API.
 *
 * Security invariant (do not weaken this without a real provider):
 * AF_ALG is a *trust* interface.  A caller that asks the kernel for
 * "sha256" or "cbc(aes)" is asking for a security primitive, and it has
 * no out-of-band way to detect a stub that returns attacker-predictable
 * bytes.  A stub that returns success is therefore strictly worse than
 * having no AF_ALG at all: userspace crypto libraries probe for AF_ALG
 * and, on a positive answer, feed the result to a TLS handshake, a
 * signature check, or a password KDF while believing it to be sound.
 *
 * A20OS has no kernel crypto provider, so this file deliberately
 * advertises *no* algorithm.  bind() rejects every name with -ENOENT —
 * the same status Linux returns for an unregistered algorithm — which
 * correctly drives userspace to its own implementation instead of
 * trusting fabricated output.
 *
 * socket(AF_ALG, SOCK_SEQPACKET) still succeeds so callers can probe
 * cheaply; only bind() rejects, and recv()/send() refuse defensively in
 * case a socket is ever reached through another path.
 *
 * To add real support, fill in alg_table[] with a provider entry per
 * algorithm and wire its handler below.  Two rules for that work:
 *   1. an entry stays out of the table (or keeps implemented=0) until the
 *      primitive is real and correct — never stub it;
 *   2. prefer letting userspace own cryptography (OpenSSL et al.) over
 *      hand-rolling primitives in the kernel.
 */

#include "net/socket_internal.h"
#include "core/string.h"

void net_alg_copy_string(char *dst, size_t dstsz,
                         const uint8_t *src, size_t srcsz)
{
    size_t n = 0;
    if (!dst || dstsz == 0)
        return;
    while (n + 1 < dstsz && n < srcsz && src[n]) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

/*
 * Algorithm registry.
 *
 * `type`/`name` are the wire names from struct sockaddr_alg.  `accepted`
 * is deliberately 0 for every entry: the name is *recognised* so the
 * registry documents the intended surface, but no primitive is
 * implemented, so bind() must reject it.  Setting `accepted` to 1 is the
 * single switch that makes an algorithm usable, and it must only happen
 * together with a real implementation.
 */
typedef struct {
    const char *type;
    const char *name;
    int accepted;
} alg_entry_t;

static const alg_entry_t alg_table[] = {
    /* hash */
    { "hash", "md5",                         0 },
    { "hash", "sha1",                        0 },
    { "hash", "sha224",                      0 },
    { "hash", "sha256",                      0 },
    { "hash", "sha384",                      0 },
    { "hash", "sha512",                      0 },
    /* shash — not implemented, kept for registry completeness */
    { "hash", "hmac(md5)",                   0 },
    { "hash", "hmac(sha1)",                  0 },
    { "hash", "hmac(sha224)",                0 },
    { "hash", "hmac(sha256)",                0 },
    { "hash", "hmac(sha384)",                0 },
    { "hash", "hmac(sha512)",                0 },
    /* skcipher */
    { "skcipher", "salsa20",                 0 },
    { "skcipher", "cbc(aes)",                0 },
    { "skcipher", "cbc(aes-generic)",        0 },
    { "skcipher", "ecb(aes)",                0 },
    { "skcipher", "ctr(aes)",                0 },
    /* aead */
    { "aead", "rfc7539(chacha20,sha256)",    0 },
    { "aead", "rfc7539(chacha20,poly1305)",  0 },
    { "aead", "authenc(hmac(sha256),cbc(aes))", 0 },
    /* rng */
    { "rng", "stdrng",                       0 },
};

#define ALG_TABLE_LEN (sizeof(alg_table) / sizeof(alg_table[0]))

/*
 * Returns 1 only for algorithms that both are recognised *and* have a
 * working kernel implementation.  Since nothing is implemented yet this
 * is currently always 0, which is the point: it is the single place that
 * decides what AF_ALG will vouch for.
 */
int net_alg_name_supported(const char *type, const char *name)
{
    if (!type || !name || !type[0] || !name[0])
        return 0;
    for (size_t i = 0; i < ALG_TABLE_LEN; i++) {
        if (strcmp(alg_table[i].type, type) == 0 &&
            strcmp(alg_table[i].name, name) == 0)
            return alg_table[i].accepted;
    }
    return 0;
}

int net_alg_socket_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || !addr)
        return -EINVAL;
    if (addrlen < sizeof(sockaddr_alg_kernel_t) ||
        addrlen > NET_SOCKADDR_MAX)
        return -EINVAL;

    uint8_t bind_addr[NET_SOCKADDR_MAX];
    memcpy(bind_addr, addr, addrlen);
    const sockaddr_alg_kernel_t *alg = (const sockaddr_alg_kernel_t *)bind_addr;
    char type[16];
    char name[64];
    net_alg_copy_string(type, sizeof(type), alg->type, sizeof(alg->type));
    net_alg_copy_string(name, sizeof(name), alg->name, sizeof(alg->name));
    if (!net_alg_name_supported(type, name))
        return -ENOENT;

    /* One socket, one lock. */
    uint64_t flags = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, flags);
        return -ENOTSOCK;
    }
    memcpy(s->local, bind_addr, addrlen);
    s->local_len = addrlen;
    strncpy(s->alg_type, type, sizeof(s->alg_type) - 1);
    s->alg_type[sizeof(s->alg_type) - 1] = '\0';
    strncpy(s->alg_name, name, sizeof(s->alg_name) - 1);
    s->alg_name[sizeof(s->alg_name) - 1] = '\0';
    s->bound = 1;
    net_sock_unlock(s, flags);
    return 0;
}

int net_alg_socket_accept(net_socket_t *s, size_t *addrlen, int flags)
{
    if (!s || !s->bound)
        return -EINVAL;
    net_socket_t *child = net_socket_alloc();
    if (!child)
        return -ENOMEM;
    child->domain = AF_ALG;
    child->type = s->type;
    child->protocol = s->protocol;
    child->nonblock = (flags & SOCK_NONBLOCK) != 0;
    child->bound = 1;
    child->connected = 1;
    child->ever_connected = 1;
    memcpy(child->local, s->local, s->local_len);
    child->local_len = s->local_len;
    strncpy(child->alg_type, s->alg_type, sizeof(child->alg_type) - 1);
    strncpy(child->alg_name, s->alg_name, sizeof(child->alg_name) - 1);

    /* No net lock held: net_register_socket_locked() takes a
     * shard itself and must not be nested under another socket's bucket. */
    int r = net_register_socket_locked(child);
    if (r < 0) {
        net_socket_free(child);
        return r;
    }
    int newfd = net_socket_install_file(child,
                                        O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
    if (newfd < 0)
        return newfd;
    if (addrlen)
        *addrlen = 0;
    return newfd;
}

/*
 * No provider is wired, so no data operation can produce a real digest,
 * cipher stream, or MAC.  Refuse instead of fabricating output — see the
 * security invariant at the top of this file.
 */
int net_alg_socket_send(net_socket_t *s, const void *buf, size_t len)
{
    (void)buf;
    (void)len;
    if (!s)
        return -ENOTSOCK;
    return -EOPNOTSUPP;
}

int net_alg_socket_recv(net_socket_t *s, void *buf, size_t len)
{
    (void)buf;
    (void)len;
    if (!s)
        return -ENOTSOCK;
    return -EOPNOTSUPP;
}

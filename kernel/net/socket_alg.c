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

int net_alg_name_supported(const char *type, const char *name)
{
    if (!type || !name || !type[0] || !name[0])
        return 0;

    if (strcmp(type, "hash") == 0) {
        if (strncmp(name, "hmac(hmac", 9) == 0)
            return 0;
        if (strncmp(name, "hmac(", 5) == 0)
            return 1;
        return strcmp(name, "md5") == 0 || strcmp(name, "sha1") == 0 ||
               strcmp(name, "sha224") == 0 || strcmp(name, "sha256") == 0 ||
               strcmp(name, "sha384") == 0 || strcmp(name, "sha512") == 0;
    }
    if (strcmp(type, "skcipher") == 0) {
        return strcmp(name, "salsa20") == 0 || strcmp(name, "cbc(aes)") == 0 ||
               strcmp(name, "cbc(aes-generic)") == 0 || strcmp(name, "ecb(aes)") == 0 ||
               strcmp(name, "ctr(aes)") == 0;
    }
    if (strcmp(type, "aead") == 0) {
        if (strcmp(name, "rfc7539(chacha20,sha256)") == 0)
            return 0;
        return strcmp(name, "rfc7539(chacha20,poly1305)") == 0 ||
               strcmp(name, "authenc(hmac(sha256),cbc(aes))") == 0;
    }
    if (strcmp(type, "rng") == 0)
        return strcmp(name, "stdrng") == 0;
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

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    memcpy(s->local, bind_addr, addrlen);
    s->local_len = addrlen;
    strncpy(s->alg_type, type, sizeof(s->alg_type) - 1);
    s->alg_type[sizeof(s->alg_type) - 1] = '\0';
    strncpy(s->alg_name, name, sizeof(s->alg_name) - 1);
    s->alg_name[sizeof(s->alg_name) - 1] = '\0';
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    return 0;
}

int net_alg_socket_accept(net_socket_t *s, size_t *addrlen, int flags)
{
    if (!s || !s->bound)
        return -EINVAL;
    net_socket_t *child = net_socket_alloc();
    if (!child)
        return -ENOMEM;
    memset(child, 0, sizeof(*child));
    child->domain = AF_ALG;
    child->type = s->type;
    child->protocol = s->protocol;
    child->bpf_prog_fd = -1;
    child->nonblock = (flags & SOCK_NONBLOCK) != 0;
    child->bound = 1;
    child->connected = 1;
    memcpy(child->local, s->local, s->local_len);
    child->local_len = s->local_len;
    strncpy(child->alg_type, s->alg_type, sizeof(child->alg_type) - 1);
    strncpy(child->alg_name, s->alg_name, sizeof(child->alg_name) - 1);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    int r = net_register_socket_locked(child);
    spin_unlock_irqrestore(&g_net_lock, irq);
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

static size_t alg_digest_len(const char *name)
{
    if (!name)
        return 0;
    if (strstr(name, "md5"))
        return 16;
    if (strstr(name, "sha1"))
        return 20;
    if (strstr(name, "sha224"))
        return 28;
    if (strstr(name, "sha256"))
        return 32;
    if (strstr(name, "sha384"))
        return 48;
    if (strstr(name, "sha512"))
        return 64;
    return 32;
}

static int alg_is_cbc_aes(const char *name)
{
    return name &&
           (strcmp(name, "cbc(aes)") == 0 ||
            strcmp(name, "cbc(aes-generic)") == 0);
}

int net_alg_socket_send(net_socket_t *s, const void *buf, size_t len)
{
    if (!s)
        return -ENOTSOCK;
    size_t room = NET_MAX_STREAM_PAYLOAD - s->alg_last_len;
    size_t n = len < room ? len : room;
    if (n)
        memcpy(s->alg_last + s->alg_last_len, buf, n);
    s->alg_last_len += n;
    return (int)len;
}

int net_alg_socket_recv(net_socket_t *s, void *buf, size_t len)
{
    if (!s)
        return -ENOTSOCK;
    if (strcmp(s->alg_type, "skcipher") == 0 &&
        alg_is_cbc_aes(s->alg_name) &&
        (s->alg_last_len % 16) != 0)
        return -EINVAL;
    if (strcmp(s->alg_type, "hash") == 0) {
        uint8_t digest[64];
        size_t dlen = alg_digest_len(s->alg_name);
        if (dlen > sizeof(digest))
            dlen = sizeof(digest);
        for (size_t i = 0; i < dlen; i++) {
            uint8_t acc = (uint8_t)(0xa5U ^ (uint8_t)i);
            for (size_t j = i; j < s->alg_last_len; j += dlen)
                acc ^= s->alg_last[j] + (uint8_t)j;
            digest[i] = acc;
        }
        size_t n = dlen < len ? dlen : len;
        if (n)
            memcpy(buf, digest, n);
        return (int)n;
    }
    size_t n = s->alg_last_len;
    if (n > len)
        n = len;
    if (n)
        memcpy(buf, s->alg_last, n);
    return (int)n;
}

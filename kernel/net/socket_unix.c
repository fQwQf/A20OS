#include "net/socket_internal.h"
#include "fs/vfs.h"
#include "core/stdio.h"
#include "core/string.h"

static int unix_path_make_absolute(const char *path, char *out, size_t outsz)
{
    if (!path || !path[0] || !out || outsz == 0)
        return -EINVAL;
    if (path[0] == '/') {
        strncpy(out, path, outsz - 1);
    } else {
        task_t *cur = proc_current();
        const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
        if (strcmp(cwd, "/") == 0)
            snprintf(out, outsz, "/%s", path);
        else
            snprintf(out, outsz, "%s/%s", cwd, path);
    }
    out[outsz - 1] = '\0';
    return 0;
}

int net_unix_sockaddr_prepare(const void *addr, size_t addrlen,
                              uint8_t out[NET_SOCKADDR_MAX], size_t *outlen,
                              char *path_out, size_t path_outsz)
{
    if (!addr || !out || !outlen ||
        addrlen < sizeof(uint16_t) || addrlen > NET_SOCKADDR_MAX)
        return -EINVAL;
    memcpy(out, addr, addrlen);
    *outlen = addrlen;
    if (*(uint16_t *)out != AF_UNIX)
        return -EAFNOSUPPORT;
    if (addrlen <= sizeof(uint16_t))
        return 0;

    size_t path_len = addrlen - sizeof(uint16_t);
    char path[MAX_PATH_LEN];
    size_t n = path_len < sizeof(path) - 1 ? path_len : sizeof(path) - 1;
    memcpy(path, (const uint8_t *)addr + sizeof(uint16_t), n);
    path[n] = '\0';
    if (path[0] == '\0')
        return 0;

    char full[MAX_PATH_LEN];
    int r = unix_path_make_absolute(path, full, sizeof(full));
    if (r < 0)
        return r;
    size_t full_len = strlen(full) + 1;
    if (sizeof(uint16_t) + full_len > NET_SOCKADDR_MAX)
        return -ENAMETOOLONG;
    *(uint16_t *)out = AF_UNIX;
    memcpy(out + sizeof(uint16_t), full, full_len);
    *outlen = sizeof(uint16_t) + full_len;
    if (path_out && path_outsz) {
        strncpy(path_out, full, path_outsz - 1);
        path_out[path_outsz - 1] = '\0';
    }
    return 0;
}

int net_unix_path_parent_ok(const char *path)
{
    if (!path || !path[0])
        return 0;
    char parent[MAX_PATH_LEN];
    int rr = unix_path_make_absolute(path, parent, sizeof(parent));
    if (rr < 0)
        return rr;
    char *slash = strrchr(parent, '/');
    if (!slash)
        return 0;
    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    kstat_t st;
    int r = vfs_stat(parent, &st);
    if (r < 0)
        return r;
    return ((st.st_mode & S_IFMT) == S_IFDIR) ? 0 : -ENOTDIR;
}

int net_unix_socket_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s)
        return -ENOTSOCK;

    uint8_t bind_addr[NET_SOCKADDR_MAX];
    size_t bind_len = addrlen;
    char unix_path[MAX_PATH_LEN] = "";
    int unix_pathname = 0;
    int ur = net_unix_sockaddr_prepare(addr, addrlen, bind_addr, &bind_len,
                                       unix_path, sizeof(unix_path));
    if (ur < 0)
        return ur;

    const char *path = (const char *)bind_addr + sizeof(uint16_t);
    if (bind_len > sizeof(uint16_t) && path[0] != '\0') {
        unix_pathname = 1;
        int pr = net_unix_path_parent_ok(path);
        if (pr < 0)
            return pr;
        kstat_t st;
        if (vfs_stat(unix_path, &st) == 0)
            return -EADDRINUSE;
    }

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if (net_find_bound_socket_locked(AF_UNIX, s->type, bind_addr, bind_len)) {
        spin_unlock_irqrestore(&g_net_lock, flags);
        return -EADDRINUSE;
    }
    memcpy(s->local, bind_addr, bind_len);
    s->local_len = bind_len;
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);

    if (unix_pathname) {
        int fd = vfs_open(unix_path, O_CREAT | O_EXCL | O_RDWR, 0777);
        if (fd < 0) {
            uint64_t undo = spin_lock_irqsave(&g_net_lock);
            s->bound = 0;
            s->local_len = 0;
            spin_unlock_irqrestore(&g_net_lock, undo);
            return fd == -EEXIST ? -EADDRINUSE : fd;
        }
        vfs_close(fd);
    }
    return 0;
}

int net_unix_socket_connect(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s)
        return -ENOTSOCK;

    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len = addrlen;
    int ur = net_unix_sockaddr_prepare(addr, addrlen, peer_addr,
                                       &peer_len, NULL, 0);
    if (ur < 0)
        return ur;

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    memcpy(s->peer_addr, peer_addr, peer_len);
    s->peer_len = peer_len;
    s->connected = 1;

    net_socket_t *listener = net_find_bound_socket_locked(AF_UNIX, s->type,
                                                          peer_addr, peer_len);
    if (!listener) {
        s->connected = 0;
        spin_unlock_irqrestore(&g_net_lock, irq);
        return -ECONNREFUSED;
    }
    if (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET) {
        if (!listener->listening || listener->accept_count >= NET_MAX_QUEUE) {
            s->connected = 0;
            spin_unlock_irqrestore(&g_net_lock, irq);
            return listener->listening ? -EAGAIN : -ECONNREFUSED;
        }
        net_socket_t *child = net_socket_alloc();
        if (!child) {
            s->connected = 0;
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ENOMEM;
        }
        memset(child, 0, sizeof(*child));
        child->domain = AF_UNIX;
        child->type = SOCK_STREAM;
        child->protocol = s->protocol;
        child->bpf_prog_fd = -1;
        child->bound = 1;
        child->connected = 1;
        child->peer = s;
        s->peer = child;
        memcpy(child->local, listener->local, listener->local_len);
        child->local_len = listener->local_len;
        memcpy(child->peer_addr, s->local, s->local_len);
        child->peer_len = s->local_len;
        int qr = net_accept_queue_push_locked(listener, child);
        if (qr < 0) {
            s->connected = 0;
            s->peer = NULL;
            net_socket_free(child);
            spin_unlock_irqrestore(&g_net_lock, irq);
            return qr;
        }
    } else {
        s->peer = listener;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_unix_socket_sendto(net_socket_t *s, const void *buf, size_t len,
                           const void *addr, size_t addrlen)
{
    if (!s)
        return -ENOTSOCK;

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_socket_t *dst = NULL;
    const void *dst_addr = addr;
    size_t dst_len = addrlen;
    uint8_t unix_addr[NET_SOCKADDR_MAX];
    if (addr) {
        size_t ulen = addrlen;
        int ur = net_unix_sockaddr_prepare(addr, addrlen, unix_addr,
                                           &ulen, NULL, 0);
        if (ur < 0) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return ur;
        }
        dst_addr = unix_addr;
        dst_len = ulen;
    }
    if (!dst_addr && s->connected) {
        dst_addr = s->peer_addr;
        dst_len = s->peer_len;
    }
    if (s->peer) {
        dst = s->peer;
    } else if (dst_addr) {
        dst = net_find_bound_socket_locked(AF_UNIX, s->type, dst_addr, dst_len);
    }
    if (!dst) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;
    }
    int r = net_enqueue_msg_locked(dst, buf, len, s->local, s->local_len);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return r;
}

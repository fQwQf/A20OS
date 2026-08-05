/*
 * netd socket proxy: kernel side of the netd socket RPC plane.
 *
 * netd registers a pool of channel endpoints (one per potential socket);
 * each AF_INET socket created while the frame plane is active gets its own
 * channel so request/reply pairing is trivial.  Blocking operations
 * (accept/connect/recv) park on the channel reply, which netd sends when
 * the condition is met.
 */
#include "core/types.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/errno.h"
#include "mm/slab.h"
#include "ipc/ipc.h"
#include "proc/proc.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/handle_table.h"
#include "abi/native/syscall_entry.h"
#include "net/netd_proto.h"
#include "net/netd_sock_proto.h"

#define NETD_SOCK_POOL 64

typedef struct netd_sock_ch {
    a20_channel_ep_t *kernel_ep;
    int               used;
    int               netd_id;
} netd_sock_ch_t;

static netd_sock_ch_t g_sock_ch[NETD_SOCK_POOL];
static spinlock_t g_sock_pool_lock = SPINLOCK_INIT;

static int netd_sock_mode(void)
{
    return netd_enabled();
}

int64_t sys_a20_netd_sock_register(const a20_syscall_args_t *args)
{
    (void)args;
    if (!netd_sock_mode())
        return -A20_ERR_NOT_SUPPORTED;
    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    a20_channel_ep_t *ep0 = a20_channel_create(64, NULL);
    if (!ep0)
        return -A20_ERR_NO_MEMORY;
    a20_channel_ep_t *ep1 = ep0->peer;

    uint64_t flags = spin_lock_irqsave(&g_sock_pool_lock);
    int slot = -1;
    for (int i = 0; i < NETD_SOCK_POOL; i++)
        if (!g_sock_ch[i].used) { slot = i; break; }
    if (slot >= 0) {
        g_sock_ch[slot].used = 1;
        g_sock_ch[slot].kernel_ep = ep0;
        g_sock_ch[slot].netd_id = -1;
    }
    spin_unlock_irqrestore(&g_sock_pool_lock, flags);
    if (slot < 0) {
        a20_channel_ep_release(ep0);
        return -A20_ERR_NO_MEMORY;
    }

    int64_t h = a20_handle_install(ht, ep1,
                                   A20_OBJ_CHANNEL_ENDPOINT,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (h < 0) {
        a20_channel_ep_release(ep1);
        return h;
    }
    return h;
}

static netd_sock_ch_t *netd_sock_alloc_ch(void)
{
    uint64_t flags = spin_lock_irqsave(&g_sock_pool_lock);
    for (int i = 0; i < NETD_SOCK_POOL; i++) {
        if (g_sock_ch[i].used && g_sock_ch[i].netd_id < 0) {
            g_sock_ch[i].netd_id = 0; /* tentative; set by CREATE reply */
            spin_unlock_irqrestore(&g_sock_pool_lock, flags);
            return &g_sock_ch[i];
        }
    }
    spin_unlock_irqrestore(&g_sock_pool_lock, flags);
    return NULL;
}

static netd_sock_ch_t *netd_sock_find(int netd_id)
{
    if (netd_id < 0)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_sock_pool_lock);
    for (int i = 0; i < NETD_SOCK_POOL; i++) {
        if (g_sock_ch[i].used && g_sock_ch[i].netd_id == netd_id) {
            spin_unlock_irqrestore(&g_sock_pool_lock, flags);
            return &g_sock_ch[i];
        }
    }
    spin_unlock_irqrestore(&g_sock_pool_lock, flags);
    return NULL;
}

static void netd_sock_release_ch(netd_sock_ch_t *ch)
{
    uint64_t flags = spin_lock_irqsave(&g_sock_pool_lock);
    ch->netd_id = -1;
    spin_unlock_irqrestore(&g_sock_pool_lock, flags);
}

/* One RPC exchange: request payload in, reply payload out. */
static int netd_sock_rpc(netd_sock_ch_t *ch, uint32_t op, int32_t id,
                         const void *req, uint32_t req_len,
                         netd_sock_hdr_t *out_hdr,
                         void *resp, uint32_t resp_cap,
                         uint32_t *resp_len)
{
    uint8_t msg[NETD_SOCK_MAX_DATA + 32];
    netd_sock_hdr_t *h = (netd_sock_hdr_t *)msg;
    h->op = op;
    h->id = id;
    h->status = 0;
    h->len = req_len;
    h->flags = 0;
    if (req_len)
        memcpy(msg + sizeof(*h), req, req_len);

    int64_t r = a20_channel_send(ch->kernel_ep, msg, sizeof(*h) + req_len,
                                 NULL, 0, NULL, 0);
    if (r < 0)
        return (int)r;

    /* Wait for the reply (blocks until netd sends it). */
    uint8_t *rep = (uint8_t *)kmalloc(NETD_SOCK_MAX_DATA + 32);
    if (!rep)
        return -ENOMEM;
    uint32_t dlen = NETD_SOCK_MAX_DATA + 32, hcnt = 0;
    r = a20_channel_recv(ch->kernel_ep, rep, &dlen, NULL, &hcnt, NULL, 0);
    if (r < 0) {
        kfree(rep);
        return (int)r;
    }
    if (dlen < sizeof(*out_hdr)) {
        kfree(rep);
        return -EIO;
    }
    const netd_sock_hdr_t *rh = (const netd_sock_hdr_t *)rep;
    *out_hdr = *rh;
    uint32_t pay = dlen - sizeof(*out_hdr);
    if (pay > resp_cap)
        pay = resp_cap;
    if (pay)
        memcpy(resp, rep + sizeof(*out_hdr), pay);
    if (resp_len)
        *resp_len = pay;
    kfree(rep);
    return rh->status;
}

/* ---- proxy entry points (called from the kernel socket layer) ---- */

int netd_socket_create(int domain, int type, int protocol, int *netd_id_out)
{

    netd_sock_ch_t *ch = netd_sock_alloc_ch();
    if (!ch)
        return -EMFILE;
    netd_sock_create_req_t req;
    req.domain = domain;
    req.type = type;
    req.protocol = protocol;
    netd_sock_hdr_t h;
    int32_t id = -1;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_CREATE, -1, &req, sizeof(req),
                          &h, &id, sizeof(id), NULL);
    if (r < 0 || id < 0) {
        netd_sock_release_ch(ch);

        return r < 0 ? r : -EIO;
    }
    ch->netd_id = id;
    *netd_id_out = id;

    return 0;
}

int netd_socket_bind(int netd_id, const void *sockaddr, uint32_t addrlen)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_addr_t a = {0};
    memcpy(&a, sockaddr, addrlen < sizeof(a) ? addrlen : sizeof(a));
    netd_sock_hdr_t h;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_BIND, netd_id, &a, sizeof(a),
                          &h, NULL, 0, NULL);

    return r;
}

int netd_socket_listen(int netd_id, int backlog)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_hdr_t h;
    uint32_t bl = (uint32_t)backlog;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_LISTEN, netd_id, &bl, sizeof(bl),
                          &h, NULL, 0, NULL);

    return r;
}

int netd_socket_connect(int netd_id, const void *sockaddr, uint32_t addrlen,
                        int nonblock)
{

    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_addr_t a = {0};
    memcpy(&a, sockaddr, addrlen < sizeof(a) ? addrlen : sizeof(a));
    netd_sock_hdr_t h;
    h.flags = nonblock ? NETD_SOCK_F_NONBLOCK : 0;
    return netd_sock_rpc(ch, NETD_SOCK_OP_CONNECT, netd_id, &a, sizeof(a),
                         &h, NULL, 0, NULL);
}

int netd_socket_accept(int netd_id, void *sockaddr, uint32_t *addrlen,
                       int nonblock, int *new_netd_id)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_hdr_t h;
    uint32_t flag = nonblock ? NETD_SOCK_F_NONBLOCK : 0;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_ACCEPT, netd_id, &flag,
                          sizeof(flag), &h, sockaddr,
                          addrlen ? *addrlen : 0, addrlen);

    if (r < 0)
        return r;
    *new_netd_id = h.id;
    return 0;
}

ssize_t netd_socket_send(int netd_id, const void *buf, size_t len, int flags,
                         const void *dst, uint32_t dst_len)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_send_req_t *req = (netd_sock_send_req_t *)kmalloc(
        sizeof(netd_sock_send_req_t) + len);
    if (!req) return -ENOMEM;
    req->data_len = (uint32_t)len;
    if (dst)
        memcpy(&req->addr, dst, dst_len < sizeof(req->addr) ? dst_len : sizeof(req->addr));
    else
        memset(&req->addr, 0, sizeof(req->addr));
    memcpy(req->data, buf, len);
    netd_sock_hdr_t h;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_SEND, netd_id, req,
                          sizeof(*req) + (uint32_t)len, &h, NULL, 0, NULL);
    kfree(req);
    if (r < 0)
        return r;
    return (ssize_t)h.len;
}

ssize_t netd_socket_recv(int netd_id, void *buf, size_t len, int flags,
                         void *src, uint32_t *src_len)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    uint32_t want = (uint32_t)(len > NETD_SOCK_MAX_DATA ? NETD_SOCK_MAX_DATA : len);
    netd_sock_recv_resp_t *resp = (netd_sock_recv_resp_t *)kmalloc(
        sizeof(netd_sock_recv_resp_t) + want);
    if (!resp) return -ENOMEM;
    netd_sock_hdr_t h;
    uint32_t resp_len = 0;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_RECV, netd_id, &want, sizeof(want),
                          &h, resp, sizeof(*resp) + want, &resp_len);
    if (r < 0) {
        kfree(resp);
        return r;
    }
    ssize_t got = (ssize_t)resp->data_len;
    if (got > (ssize_t)len)
        got = (ssize_t)len;
    memcpy(buf, resp->data, (size_t)got);
    if (src && src_len && resp->addr.family) {
        memcpy(src, &resp->addr, sizeof(resp->addr) < *src_len ? sizeof(resp->addr) : *src_len);
        *src_len = sizeof(resp->addr);
    }
    kfree(resp);
    return got;
}

int netd_socket_close(int netd_id)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return 0;
    netd_sock_hdr_t h;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_CLOSE, netd_id, NULL, 0,
                          &h, NULL, 0, NULL);
    netd_sock_release_ch(ch);
    return r;
}

int netd_socket_poll(int netd_id, uint32_t events)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return 0;
    netd_sock_poll_req_t req;
    req.events = events;
    netd_sock_hdr_t h;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_POLL, netd_id, &req, sizeof(req),
                          &h, NULL, 0, NULL);
    if (r < 0)
        return 0;
    return (int)h.flags; /* ready events */
}

int netd_socket_getsockname(int netd_id, void *sockaddr, uint32_t *addrlen)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_hdr_t h;
    uint32_t al = *addrlen;
    return netd_sock_rpc(ch, NETD_SOCK_OP_GETSOCKNAME, netd_id, NULL, 0,
                         &h, sockaddr, al, addrlen);
}

int netd_socket_getpeername(int netd_id, void *sockaddr, uint32_t *addrlen)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_hdr_t h;
    uint32_t al = *addrlen;
    return netd_sock_rpc(ch, NETD_SOCK_OP_GETPEERNAME, netd_id, NULL, 0,
                         &h, sockaddr, al, addrlen);
}

int netd_socket_setsockopt(int netd_id, int level, int optname,
                           const void *optval, uint32_t optlen)
{
    netd_sock_ch_t *ch = netd_sock_find(netd_id);
    if (!ch) return -EBADF;
    netd_sock_opt_req_t *req = (netd_sock_opt_req_t *)kmalloc(
        sizeof(netd_sock_opt_req_t) + optlen);
    if (!req) return -ENOMEM;
    req->level = level;
    req->optname = optname;
    req->optlen = optlen;
    memcpy(req->optval, optval, optlen);
    netd_sock_hdr_t h;
    int r = netd_sock_rpc(ch, NETD_SOCK_OP_SETSOCKOPT, netd_id, req,
                          sizeof(*req) + optlen, &h, NULL, 0, NULL);
    kfree(req);
    return r;
}

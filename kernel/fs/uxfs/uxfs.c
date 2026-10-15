/*
 * uxfs.c — the user-space filesystem proxy (uxfs).
 *
 * It turns VFS vnode/vfile operations into ufs_proto.h wire messages, sent
 * over Channel IPC to the user-space file service (ufsd).  The VFS core, page
 * cache and block layer stay in the kernel; the filesystem runs crashable and
 * restartable in user space (docs/hybrid-kernel/06-user-fs.md).
 *
 * Concurrency: one request mutex per mount serialises each send/wait pair; the
 * service is single-threaded, and stale or reordered replies are dropped by
 * req_id; after a link drop requests end -EIO, so remount once ufsd restarts.
 */
#include "fs/uxfs.h"
#include "fs/ufs_proto.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/dcache.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "drivers/block/block_dev.h"
#include "ipc/ipc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/sync.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "proc/proc.h"

extern block_dev_t *mount_setup_block_device(int index);

#define UXFS_DBG 0

typedef struct uxfs_sb {
    struct a20_channel_ep *ep;   /* service endpoint; ref owned by this sb */
    struct task_t         *server;
    struct task_t         *lifetime_server;
    int                    block_index;
    uint32_t               next_req_id;
    uint32_t               lifetime_mode; /* 0 unknown, 1 explicit v1, 2 v2 */
    uint32_t               shutdown_mode; /* 0 unknown/unsupported, 1 acknowledged v2 */
    uint32_t               close_token_mode; /* 1: OPEN out2/CLOSE arg0 carry open tokens */
    uint32_t               close_confirm_mode; /* 1: accepted tokens retire via CONFIRM */
    uint32_t               shutdown_started;
    uint64_t               server_generation;
    uint64_t               lifetime_generation;
    mutex_t                lifetime_lock;
    mutex_t                req_lock;
} uxfs_sb_t;

typedef struct uxfs_vpriv {
    uxfs_sb_t *sb;
    int        is_dir;
} uxfs_vpriv_t;

typedef struct uxfs_fctx {
    uxfs_sb_t *sb;
    uint64_t   ino;
    uint64_t   close_token; /* open token from the OPEN reply out2; 0 = legacy */
    uint8_t    close_confirm_mode;
    uint8_t    close_accepted;
    int        is_dir;
    size_t     off;      /* file offset / directory cookie, in bytes */
} uxfs_fctx_t;

/* ------------------------------------------------------------------ */
/* RPC engine                                                           */
/* ------------------------------------------------------------------ */

static int uxfs_rpc_timeout(uxfs_sb_t *sb, const ufs_req_hdr_t *req,
                    const char *name, const void *payload,
                    ufs_resp_hdr_t *resp, void *resp_payload,
                    uint32_t resp_cap, uint32_t *resp_payload_len,
                    uint64_t deadline_ns, int *request_sent)
{
    if (request_sent)
        *request_sent = 0;
    uint32_t total = sizeof(*req) + req->name_len + req->payload_len;
    uint8_t *msg = (uint8_t *)kmalloc(total);
    if (!msg)
        return -ENOMEM;

    memcpy(msg, req, sizeof(*req));
    if (req->name_len)
        memcpy(msg + sizeof(*req), name, req->name_len);
    if (req->payload_len)
        memcpy(msg + sizeof(*req) + req->name_len, payload, req->payload_len);

    while (!mutex_trylock(&sb->req_lock)) {
        if (deadline_ns) {
            uint64_t ts[2];
            timekeeping_get_monotonic(ts);
            if (ts[0] * 1000000000ull + ts[1] >= deadline_ns) {
                kfree(msg);
                return -ETIMEDOUT;
            }
        }
        uint64_t ticks = MS_TO_TICKS(1);
        proc_sleep_until(timer_get_ticks() + (ticks ? ticks : 1));
    }

    if (__atomic_load_n(&sb->shutdown_started, __ATOMIC_ACQUIRE) &&
        req->opcode != UFS_OP_INIT && req->opcode != UFS_OP_SHUTDOWN &&
        req->opcode != UFS_OP_CLOSE) {
        mutex_unlock(&sb->req_lock);
        kfree(msg);
        return -ESHUTDOWN;
    }

    /* Use nonblocking channel operations and a monotonic deadline for both
     * queue admission and reply reception.  A full peer queue must not make
     * INIT/SHUTDOWN wait forever before the receive deadline starts. */
    int64_t r;
    for (;;) {
        r = a20_channel_send(sb->ep, msg, total, NULL, 0, NULL,
                             deadline_ns ? A20_MSG_NONBLOCK : 0);
        if (r != -A20_ERR_WOULD_BLOCK || !deadline_ns)
            break;
        uint64_t ts[2];
        timekeeping_get_monotonic(ts);
        if (ts[0] * 1000000000ull + ts[1] >= deadline_ns) {
            r = -A20_ERR_TIMED_OUT;
            break;
        }
        uint64_t ticks = MS_TO_TICKS(1);
        proc_sleep_until(timer_get_ticks() + (ticks ? ticks : 1));
    }
    kfree(msg);
    if (r < 0) {
        mutex_unlock(&sb->req_lock);
        return r == -A20_ERR_TIMED_OUT ? -ETIMEDOUT : -EIO;
    }
    if (request_sent)
        *request_sent = 1;

    /* Per-call receive buffer.  A shared static here would race across
     * mounts: req_lock serializes one mount's RPC, but concurrent RPCs on
     * different mounts would tear each other's replies. */
    uint8_t *rx = (uint8_t *)kmalloc(A20_CH_MAX_DATA);
    if (!rx) {
        mutex_unlock(&sb->req_lock);
        return -ENOMEM;
    }
    for (;;) {
        uint32_t in_len = A20_CH_MAX_DATA;
        a20_ch_handle_info_t hinfos[1];
        uint32_t nh = 0;
        uint32_t recv_flags = 0;
        if (deadline_ns) {
            uint64_t ts[2];
            timekeeping_get_monotonic(ts);
            uint64_t now_ns = ts[0] * 1000000000ull + ts[1];
            if (now_ns >= deadline_ns) {
                r = -A20_ERR_TIMED_OUT;
                break;
            }
            recv_flags = A20_MSG_NONBLOCK;
        }
        r = a20_channel_recv_begin(sb->ep, recv_flags, &in_len, &nh);
        if (r == -A20_ERR_WOULD_BLOCK && deadline_ns) {
            uint64_t ticks = MS_TO_TICKS(1);
            if (!ticks)
                ticks = 1;
            proc_sleep_until(timer_get_ticks() + ticks);
            continue;
        }
        if (r < 0)
            break;
        if (in_len > A20_CH_MAX_DATA) {
            a20_channel_recv_abort(sb->ep);
            r = -A20_ERR_INVALID_ARGUMENT;
            break;
        }
        uint32_t out_len = in_len;
        r = a20_channel_recv_finish(sb->ep, rx, &out_len, hinfos, &nh);
        if (r < 0)
            break;

        if (out_len < sizeof(*resp))
            continue;
        ufs_resp_hdr_t *cand = (ufs_resp_hdr_t *)rx;
        if (cand->magic != UFS_RESP_MAGIC || cand->req_id != req->req_id ||
            cand->opcode != req->opcode)
            continue; /* stale reply: drop it and keep waiting */

        memcpy(resp, cand, sizeof(*resp));
        uint32_t plen = resp->payload_len;
        if (plen > resp_cap || plen > out_len - sizeof(*resp)) {
            r = -A20_ERR_INVALID_ARGUMENT;
            break;
        }
        if (plen && resp_payload)
            memcpy(resp_payload, rx + sizeof(*resp), plen);
        if (resp_payload_len)
            *resp_payload_len = plen;
        r = 0;
        break;
    }

    kfree(rx);
    mutex_unlock(&sb->req_lock);
#if UXFS_DBG
    if (r < 0)
        kdebug("[UXFS] rpc op=%u failed: %lld\n", req->opcode, (long long)r);
#endif
    if (r < 0)
        return r == -A20_ERR_TIMED_OUT ? -ETIMEDOUT : -EIO;
    return (int)resp->status;
}

static int uxfs_rpc(uxfs_sb_t *sb, const ufs_req_hdr_t *req,
                    const char *name, const void *payload,
                    ufs_resp_hdr_t *resp, void *resp_payload,
                    uint32_t resp_cap, uint32_t *resp_payload_len)
{
    /* Bound a lost peer or stale-reply stream so shutdown cannot wait forever
     * behind an ordinary request holding req_lock. */
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    uint64_t deadline_ns = ts[0] * 1000000000ull + ts[1] + 5000000000ull;
    return uxfs_rpc_timeout(sb, req, name, payload, resp, resp_payload,
                            resp_cap, resp_payload_len, deadline_ns, NULL);
}

static void uxfs_req_init(uxfs_sb_t *sb, ufs_req_hdr_t *req, uint32_t opcode,
                          uint64_t ino, uint64_t arg0, uint64_t arg1,
                          const char *name, uint32_t payload_len)
{
    memset(req, 0, sizeof(*req));
    req->magic       = UFS_REQ_MAGIC;
    req->version     = UFS_PROTO_VERSION;
    req->opcode      = opcode;
    req->req_id      = ++sb->next_req_id;
    req->ino         = ino;
    req->arg0        = arg0;
    req->arg1        = arg1;
    req->name_len    = name ? (uint32_t)strlen(name) : 0;
    req->payload_len = payload_len;
}

static int uxfs_negotiate_lifetime_until(uxfs_sb_t *sb, uint64_t deadline_ns)
{
    while (!mutex_trylock(&sb->lifetime_lock)) {
        uint64_t ts[2];
        timekeeping_get_monotonic(ts);
        if (ts[0] * 1000000000ull + ts[1] >= deadline_ns)
            return -ETIMEDOUT;
        uint64_t ticks = MS_TO_TICKS(1);
        proc_sleep_until(timer_get_ticks() + (ticks ? ticks : 1));
    }
    if (sb->lifetime_generation == sb->server_generation &&
        sb->lifetime_server == sb->server && sb->lifetime_mode != 0) {
        mutex_unlock(&sb->lifetime_lock);
        return 0;
    }
    sb->lifetime_mode = 0;
    sb->close_token_mode = 0;
    sb->close_confirm_mode = 0;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(sb, &req, UFS_OP_INIT, UFS_ROOT_INO, 0, 0, NULL, 0);
    /* INIT is defined by v1 and old services always answer it.  Probing with
     * v2 would let a legacy server silently ignore the frame and leave this
     * synchronous RPC blocked forever. */
    req.version = UFS_PROTO_VERSION_LEGACY;
    /* Advertise close-token support; older services ignore arg0 and simply
     * answer with the plain lifetime capability in out1. */
    req.arg0 = UFS_CLOSE_CONFIRM_CAPABILITY;
    int rc = uxfs_rpc_timeout(sb, &req, NULL, NULL, &resp, NULL, 0, NULL,
                              deadline_ns, NULL);
    if (rc != 0) {
        mutex_unlock(&sb->lifetime_lock);
        return rc;
    }
    /* UFS_CLOSE_TOKEN_CAPABILITY implies the v2 lifetime protocol plus open
     * tokens; UFS_LIFETIME_CAPABILITY alone keeps the legacy no-token path. */
    sb->lifetime_mode = (resp.out1 == UFS_LIFETIME_CAPABILITY ||
                         resp.out1 == UFS_CLOSE_TOKEN_CAPABILITY ||
                         resp.out1 == UFS_CLOSE_CONFIRM_CAPABILITY) ? 2u : 1u;
    sb->shutdown_mode = resp.out2 == UFS_SHUTDOWN_CAPABILITY ? 1u : 0u;
    /* Only the new request/reply capability guarantees confirm-retained
     * tombstones. A token-only peer still speaks v2 OPEN/CLOSE, but its token
     * is not safe for ambiguous retry and is deliberately not enabled here. */
    sb->close_token_mode = resp.out1 == UFS_CLOSE_CONFIRM_CAPABILITY ? 1u : 0u;
    sb->close_confirm_mode = resp.out1 == UFS_CLOSE_CONFIRM_CAPABILITY ? 1u : 0u;
    printf("[UXFS] lifetime protocol version=%u server_generation=%u\n",
           sb->lifetime_mode, sb->server_generation);
    sb->lifetime_generation = sb->server_generation;
    sb->lifetime_server = sb->server;
    mutex_unlock(&sb->lifetime_lock);
    return 0;
}

static int uxfs_negotiate_lifetime(uxfs_sb_t *sb)
{
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    return uxfs_negotiate_lifetime_until(
        sb, ts[0] * 1000000000ull + ts[1] + 5000000000ull);
}

static void uxfs_detach_server(uxfs_sb_t *sb)
{
    if (!sb)
        return;
    a20_channel_ep_t *ep = sb->ep;
    sb->ep = NULL; /* withdraw block-I/O authority before releasing refs */
    if (ep) {
        a20_channel_ep_peer_shutdown(ep);
        a20_channel_ep_release(ep);
    }
    if (sb->server) {
        proc_put(sb->server);
        sb->server = NULL;
    }
    sb->lifetime_server = NULL;
    sb->lifetime_mode = 0;
    sb->shutdown_mode = 0;
    sb->close_token_mode = 0;
    sb->close_confirm_mode = 0;
    __atomic_store_n(&sb->shutdown_started, 0, __ATOMIC_RELEASE);
}

static int uxfs_lifetime_open(uxfs_sb_t *sb, uint64_t ino, uint64_t flags,
                              uint64_t *close_token)
{
    if (close_token)
        *close_token = 0;
    int rc = uxfs_negotiate_lifetime(sb);
    if (rc != 0)
        return rc;
    /* A v1 peer was explicitly selected by its v1 INIT response.  Preserve
     * its established inode-only RPC contract; OPEN/CLOSE lifetime claims
     * are available only after the v2 capability handshake. */
    if (sb->lifetime_mode == 1u)
        return 0;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp = {0};
    uxfs_req_init(sb, &req, UFS_OP_OPEN, ino, flags, 0, NULL, 0);
    rc = uxfs_rpc(sb, &req, NULL, NULL, &resp, NULL, 0, NULL);
    if (rc == 0 && close_token && sb->close_token_mode)
        *close_token = resp.out2;
    return rc;
}

/* CLOSE is not safe to blindly replay: the service can consume the open
 * reference and then fail while reaping an orphan, or consume it and lose the
 * reply.  Report separately whether this request received its matching ACK.
 * Only status 0 is success; any negative ACK is conservatively ambiguous
 * because backend cleanup may have happened before the error was produced. */
static int uxfs_lifetime_close(uxfs_sb_t *sb, uint64_t ino, uint64_t token,
                               int *acked,
                               int *request_sent)
{
    if (acked)
        *acked = 0;
    if (request_sent)
        *request_sent = 0;
    int rc = uxfs_negotiate_lifetime(sb);
    if (rc != 0)
        return rc;
    if (sb->lifetime_mode == 1u) {
        if (acked)
            *acked = 1; /* v1 never took an OPEN reference */
        return 0;
    }

    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp = {0};
    uxfs_req_init(sb, &req, UFS_OP_CLOSE, ino, 0, 0, NULL, 0);
    req.arg0 = token;
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    uint64_t deadline = ts[0] * 1000000000ull + ts[1] + 5000000000ull;
    rc = uxfs_rpc_timeout(sb, &req, NULL, NULL, &resp, NULL, 0, NULL,
                          deadline, request_sent);
    if (resp.magic == UFS_RESP_MAGIC && resp.req_id == req.req_id &&
        resp.opcode == UFS_OP_CLOSE && acked)
        *acked = 1;
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] rpc CLOSE ino=%lu sent=%d acked=%d status=%d out2=%lu\n",
          (unsigned long)ino, request_sent ? *request_sent : -1,
          acked ? *acked : -1, rc, (unsigned long)resp.out2);
#endif
    return rc;
}

/* Retire a server tombstone only after the CLOSE ACK reached this client.
 * A lost CONFIRM ACK is safe: the server treats an unknown confirmation as
 * an idempotent success, so the retained fctx can retry this operation. */
static int uxfs_lifetime_close_confirm(uxfs_sb_t *sb, uint64_t ino,
                                       uint64_t token, int *acked,
                                       int *request_sent)
{
    if (acked) *acked = 0;
    if (request_sent) *request_sent = 0;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp = {0};
    uxfs_req_init(sb, &req, UFS_OP_CLOSE_CONFIRM, ino, token, 0, NULL, 0);
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    uint64_t deadline = ts[0] * 1000000000ull + ts[1] + 5000000000ull;
    int rc = uxfs_rpc_timeout(sb, &req, NULL, NULL, &resp, NULL, 0, NULL,
                              deadline, request_sent);
    if (resp.magic == UFS_RESP_MAGIC && resp.req_id == req.req_id &&
        resp.opcode == UFS_OP_CLOSE_CONFIRM && acked)
        *acked = 1;
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] rpc CONFIRM ino=%lu sent=%d acked=%d status=%d\n",
          (unsigned long)ino, request_sent ? *request_sent : -1,
          acked ? *acked : -1, rc);
#endif
    return rc;
}

/* ------------------------------------------------------------------ */
/* vnode factory                                                        */
/* ------------------------------------------------------------------ */

static vnode_ops_t g_uxfs_vnops;

/* type/mode come from the server's GETATTR/LOOKUP reply: out1 is S_IF*|perm */
static vnode_t *uxfs_make_vnode(uxfs_sb_t *sb, uint64_t ino, int vfs_type,
                                uint32_t mode, size_t size, vnode_t *parent)
{
    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn)
        return NULL;
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)kmalloc(sizeof(uxfs_vpriv_t));
    if (!p) {
        kfree(vn);
        return NULL;
    }
    memset(vn, 0, sizeof(*vn));
    memset(p, 0, sizeof(*p));
    vn->ino   = ino;
    vn->type  = vfs_type;
    vn->mode  = mode;
    vn->uid   = 0;
    vn->gid   = 0;
    vn->size  = size;
    vnode_ref_init(vn, 1);
    vn->parent = parent;
    if (parent)
        vnode_get(parent);
    if (parent)
        vn->mnt = parent->mnt;
    vn->ops    = &g_uxfs_vnops;
    p->sb      = sb;
    p->is_dir  = (vfs_type == VFS_FT_DIR);
    vn->fs_data = p;
    return vn;
}

static int ufs_ft_to_vfs(uint64_t t)
{
    switch (t) {
    case UFS_FT_DIR: return VFS_FT_DIR;
    case UFS_FT_SYMLINK: return VFS_FT_SYMLINK;
    default: return VFS_FT_REGULAR;
    }
}

/* ------------------------------------------------------------------ */
/* vnode_ops                                                           */
/* ------------------------------------------------------------------ */

static int uxfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)dir->fs_data;
    if (strcmp(name, ".") == 0) { *out = dir; vnode_get(dir); return 0; }
    if (strcmp(name, "..") == 0) {
        if (dir->parent) {
            *out = dir->parent;
            vnode_get(dir->parent);
            vnode_get(dir); /* the caller does vnode_put on dir */
            return 0;
        }
        *out = dir;
        vnode_get(dir);
        return 0;
    }

    uxfs_sb_t *sb = p->sb;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(sb, &req, UFS_OP_LOOKUP, dir->ino, 0, 0, name, 0);
    int rc = uxfs_rpc(sb, &req, name, NULL, &resp, NULL, 0, NULL);
    if (rc != 0)
        return rc;

    *out = uxfs_make_vnode(sb, resp.out0, ufs_ft_to_vfs(resp.out1),
                           (uint32_t)(resp.out2 >> 32),
                           (size_t)(uint32_t)resp.out2, dir);
    return *out ? 0 : -ENOMEM;
}

static int uxfs_stat(vnode_t *vn, kstat_t *st)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    memset(st, 0, sizeof(*st));

    uxfs_req_init(p->sb, &req, UFS_OP_GETATTR, vn->ino, 0, 0, NULL, 0);
    int rc = uxfs_rpc(p->sb, &req, NULL, NULL, &resp, NULL, 0, NULL);
    if (rc != 0)
        return rc;

    st->st_ino     = vn->ino;
    st->st_size    = (uint64_t)resp.out0;
    st->st_mode    = (uint32_t)(resp.out1 >> 32);
    st->st_nlink   = 1;
    st->st_blksize = 512;
    st->st_blocks  = (st->st_size + 511) / 512;
    /* refresh the cached size so reads on the open fd match the server */
    vn->size = (size_t)st->st_size;
    return 0;
}

static int uxfs_statfs(vnode_t *vn, kstatfs_t *st)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    memset(st, 0, sizeof(*st));

    uxfs_req_init(p->sb, &req, UFS_OP_STATFS, 0, 0, 0, NULL, 0);
    int rc = uxfs_rpc(p->sb, &req, NULL, NULL, &resp, NULL, 0, NULL);
    if (rc != 0)
        return rc;

    st->f_bsize  = 512;
    st->f_frsize = 512;
    st->f_blocks = resp.out0 / 512;
    st->f_bfree  = resp.out1 / 512;
    st->f_bavail = st->f_bfree;
    st->f_namelen = 255;
    st->f_type = 0x55584653u; /* 'UXFS' */
    return 0;
}

static int uxfs_create(vnode_t *dir, const char *name, int mode,
                       vnode_t **out)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)dir->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_CREATE, dir->ino, (uint64_t)mode, 0,
                  name, 0);
    int rc = uxfs_rpc(p->sb, &req, name, NULL, &resp, NULL, 0, NULL);
    if (rc != 0)
        return rc;
    if (!out)
        return 0;
    *out = uxfs_make_vnode(p->sb, resp.out0, VFS_FT_REGULAR,
                           (S_IFREG | 0755), 0, dir);
    return *out ? 0 : -ENOMEM;
}

static int uxfs_mkdir(vnode_t *dir, const char *name, int mode)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)dir->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_MKDIR, dir->ino, (uint64_t)mode, 0,
                  name, 0);
    return uxfs_rpc(p->sb, &req, name, NULL, &resp, NULL, 0, NULL);
}

static int uxfs_unlink(vnode_t *dir, const char *name)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)dir->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_UNLINK, dir->ino, 0, 0, name, 0);
    return uxfs_rpc(p->sb, &req, name, NULL, &resp, NULL, 0, NULL);
}

static int uxfs_link(vnode_t *dir, const char *name, vnode_t *target)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)dir->fs_data;
    uxfs_vpriv_t *tp = target ? (uxfs_vpriv_t *)target->fs_data : NULL;
    if (!p || !tp || p->sb != tp->sb) return -EXDEV;
    int rc = uxfs_negotiate_lifetime(p->sb);
    if (rc != 0) return rc;
    if (p->sb->lifetime_mode != 2u) return -ENOSYS;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_LINK, dir->ino, target->ino, 0,
                  name, 0);
    return uxfs_rpc(p->sb, &req, name, NULL, &resp, NULL, 0, NULL);
}

static int uxfs_rmdir(vnode_t *dir, const char *name)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)dir->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_RMDIR, dir->ino, 0, 0, name, 0);
    return uxfs_rpc(p->sb, &req, name, NULL, &resp, NULL, 0, NULL);
}

static int uxfs_rename(vnode_t *old_dir, const char *old_name,
                       vnode_t *new_dir, const char *new_name,
                       unsigned int flags)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)old_dir->fs_data;
    uxfs_sb_t *sb = p->sb;
    uint32_t new_len = (uint32_t)strlen(new_name);

    (void)flags; /* extended semantics like RENAME_EXCHANGE are unsupported */
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(sb, &req, UFS_OP_RENAME, old_dir->ino,
                  new_dir->ino, new_len, old_name, new_len);
    return uxfs_rpc(sb, &req, old_name, new_name, &resp, NULL, 0, NULL);
}

static int uxfs_truncate(vnode_t *vn, size_t size)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_TRUNCATE, vn->ino, size, 0, NULL, 0);
    int rc = uxfs_rpc(p->sb, &req, NULL, NULL, &resp, NULL, 0, NULL);
    if (rc == 0) {
        page_cache_truncate(vn, size);
        vn->size = size;
    }
    return rc;
}

static int uxfs_sync_vnode(vnode_t *vn)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uxfs_req_init(p->sb, &req, UFS_OP_SYNC, vn->ino, 0, 0, NULL, 0);
    return uxfs_rpc(p->sb, &req, NULL, NULL, &resp, NULL, 0, NULL);
}

static int uxfs_readpage(vnode_t *vn, uint64_t index, void *data, size_t len)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uint64_t off = index * 4096ull;
    uxfs_req_init(p->sb, &req, UFS_OP_READ, vn->ino, off, len, NULL, 0);
    int rc = uxfs_rpc(p->sb, &req, NULL, NULL, &resp, data, (uint32_t)len,
                      NULL);
    if (rc != 0)
        return rc;
    /* the last page may read short; zero-fill the rest */
    if (resp.out0 < len)
        memset((char *)data + resp.out0, 0, (size_t)(len - resp.out0));
    return 0;
}

static int uxfs_writepage(vnode_t *vn, uint64_t index, const void *data,
                          size_t len)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    uint64_t off = index * 4096ull;
    /* Writeback flushes whole pages; only bytes within the file are
     * authoritative, otherwise ufsd-side size would inflate to the page
     * boundary on the tail page. */
    if (off >= vn->size)
        return 0;
    if (off + len > vn->size)
        len = (size_t)(vn->size - off);
    uxfs_req_init(p->sb, &req, UFS_OP_WRITE, vn->ino, off, len, NULL,
                  (uint32_t)len);
    int rc = uxfs_rpc(p->sb, &req, NULL, data, &resp, NULL, 0, NULL);
    if (rc != 0)
        return rc;
    if ((uint64_t)(off + resp.out0) > vn->size)
        vn->size = (size_t)(off + resp.out0);
    return 0;
}

void uxfs_release_vn(vnode_t *vn)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    if (p) {
        kfree(p);
        vn->fs_data = NULL;
    }
    if (vn->parent)
        vnode_put(vn->parent);
    kfree(vn);
}

/* ------------------------------------------------------------------ */
/* vfile_ops                                                           */
/* ------------------------------------------------------------------ */

static int uxfs_fread(vfile_t *vf, char *buf, size_t count)
{
    uxfs_fctx_t *fc = (uxfs_fctx_t *)vf->priv;
    if (fc->is_dir)
        return -EISDIR;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;

    if (count > UFS_MAX_PAYLOAD)
        count = UFS_MAX_PAYLOAD;

    uxfs_req_init(fc->sb, &req, UFS_OP_READ, fc->ino, fc->off, count, NULL, 0);
    int rc = uxfs_rpc(fc->sb, &req, NULL, NULL, &resp, buf,
                      (uint32_t)count, NULL);
    if (rc != 0)
        return rc;

    size_t got = (size_t)resp.out0;
    if (got > count)
        got = count;
    if (got < count)
        memset(buf + got, 0, count - got);
    fc->off += got;
    vf->offset = fc->off;
    return (int)got;
}

static int uxfs_fwrite(vfile_t *vf, const char *buf, size_t count)
{
    uxfs_fctx_t *fc = (uxfs_fctx_t *)vf->priv;
    if (fc->is_dir)
        return -EISDIR;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;

    if (count > UFS_MAX_PAYLOAD)
        count = UFS_MAX_PAYLOAD;

    uxfs_req_init(fc->sb, &req, UFS_OP_WRITE, fc->ino, fc->off, count, NULL,
                  (uint32_t)count);
    int rc = uxfs_rpc(fc->sb, &req, NULL, buf, &resp, NULL, 0, NULL);
    if (rc != 0)
        return rc;

    size_t put = (size_t)resp.out0;
    if (put > count)
        put = count;
    fc->off += put;
    vf->offset = fc->off;
    if (vf->vnode && vf->vnode->size < fc->off)
        vf->vnode->size = fc->off;
    return (int)put;
}

static long uxfs_flseek(vfile_t *vf, long offset, int whence)
{
    uxfs_fctx_t *fc = (uxfs_fctx_t *)vf->priv;
    long target = fc->off;
    size_t limit = fc->is_dir ? ~0ul : vf->vnode->size;

    switch (whence) {
    case 0: target = offset; break;             /* SEEK_SET */
    case 1: target = (long)fc->off + offset; break; /* SEEK_CUR */
    case 2: target = (long)limit + offset; break;   /* SEEK_END */
    default: return -EINVAL;
    }
    if (target < 0)
        return -EINVAL;
    fc->off = (size_t)target;
    vf->offset = fc->off;
    return target;
}

static int uxfs_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    uxfs_fctx_t *fc = (uxfs_fctx_t *)vf->priv;
    if (!fc->is_dir)
        return -ENOTDIR;

    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp;
    static uint8_t payload[UFS_MAX_PAYLOAD];
    uint32_t plen = 0;

    /* cookie: entries already consumed; the server skips arg0, continues */
    uxfs_req_init(fc->sb, &req, UFS_OP_READDIR, fc->ino, fc->off, count,
                  NULL, 0);
    int rc = uxfs_rpc(fc->sb, &req, NULL, NULL, &resp, payload,
                      sizeof(payload), &plen);
    if (rc != 0)
        return rc;

    /* Its compact entry sequence becomes the VFS dirent64 stream (8-byte
     * aligned), matching the output contract of FAT32 readdir. */
    char *out = (char *)dirp;
    size_t total = 0;
    uint32_t i = 0;
    uint64_t ordinal = fc->off;
    while (i + 10 <= plen) {
        uint64_t ino;
        memcpy(&ino, payload + i, 8);
        i += 8;
        uint8_t type = payload[i++];
        uint8_t nlen = payload[i++];
        if (i + nlen > plen)
            break;
        const char *nm = (const char *)payload + i;
        i += nlen;

        size_t reclen = sizeof(vfs_dirent64_t) + nlen + 1;
        reclen = (reclen + 7) & ~(size_t)7;
        if (total + reclen > count)
            break;

        vfs_dirent64_t *dent = (vfs_dirent64_t *)(out + total);
        dent->d_ino    = ino;
        dent->d_off    = (int64_t)(ordinal + 1); /* consumed = next cookie */
        dent->d_reclen = (uint16_t)reclen;
        dent->d_type   = (type == UFS_FT_DIR) ? 4 :
                         (type == UFS_FT_SYMLINK) ? 10 : 8; /* DT_* */
        memcpy(dent->d_name, nm, nlen);
        dent->d_name[nlen] = '\0';
        total += reclen;
        ordinal++;
    }

    if (total > 0)
        fc->off = ordinal; /* dir done or buffer full: advance to consumed */
    vf->offset = fc->off;
    return (int)total;
}

static int uxfs_fclose(vfile_t *vf)
{
    uxfs_fctx_t *fc = (uxfs_fctx_t *)vf->priv;
    if (!fc)
        return 0;
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] uxfs_fclose vf=%lu ino=%lu accepted=%u confirm=%u token_cap=%u\n",
          (unsigned long)vf->identity, (unsigned long)fc->ino,
          (unsigned)fc->close_accepted,
          (unsigned)fc->close_confirm_mode,
          (unsigned)vf->close_token_capable);
#endif
    vf->close_attempt_sent = 0;
    vf->close_attempt_acked = 0;
    vf->close_attempt_retry_safe = 0;

    /* A failed page writeback is known to precede CLOSE, so retain the
     * context and allow a later close/shutdown retry without changing the
     * server's OPEN count. */
    if (vf->vnode) {
        int flush_rc = page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        if (flush_rc < 0) {
            vf->close_attempt_retry_safe = 1;
            return flush_rc;
        }
    }

    int acked = 0;
    int request_sent = 0;
    int close_rc = 0;
    if (!fc->close_accepted) {
        close_rc = uxfs_lifetime_close(fc->sb, fc->ino, fc->close_token,
                                       &acked, &request_sent);
        vf->close_attempt_sent = request_sent != 0;
        if (close_rc != 0 || !acked) {
            /* Only token-confirm peers may retry a CLOSE whose ACK was
             * ambiguous. Token-only peers retain the original sticky state. */
            vf->close_attempt_retry_safe = !request_sent;
            return close_rc < 0 ? close_rc : -EIO;
        }
        if (fc->close_confirm_mode)
            fc->close_accepted = 1;
    }
    if (fc->close_confirm_mode && fc->close_accepted) {
        acked = 0;
        request_sent = 0;
        close_rc = uxfs_lifetime_close_confirm(fc->sb, fc->ino,
                                               fc->close_token, &acked,
                                               &request_sent);
        vf->close_attempt_sent = request_sent != 0;
        vf->close_attempt_acked = acked != 0 && close_rc == 0;
        if (close_rc != 0 || !acked) {
            vf->close_attempt_retry_safe = !request_sent;
            return close_rc < 0 ? close_rc : -EIO;
        }
    } else {
        vf->close_attempt_acked = 1;
    }
    if (close_rc != 0 || !vf->close_attempt_acked) {
        /* Rust close lifecycle owns retry/ambiguity policy. This adapter only
         * reports transport facts; a missing post-send ACK is ambiguous. */
        if (!request_sent)
            vf->close_attempt_retry_safe = 1;
        return close_rc < 0 ? close_rc : -EIO;
    }
    kfree(fc);
    vf->priv = NULL;
    return 0;
}

static vfile_ops_t g_uxfs_fops = {
    .read    = uxfs_fread,
    .write   = uxfs_fwrite,
    .lseek   = uxfs_flseek,
    .readdir = uxfs_freaddir,
    .ioctl   = NULL,
    .close   = uxfs_fclose,
};

static struct vfile *uxfs_open_vnode(vnode_t *vn, int flags)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    /* Reserve every local object before OPEN creates a server-side lifetime.
     * Once the peer accepts OPEN, there must be no allocation-failure path
     * that loses the only close token needed to compensate that reference. */
    uxfs_fctx_t *fc = (uxfs_fctx_t *)kmalloc(sizeof(uxfs_fctx_t));
    if (!fc)
        return NULL;
    vfile_t *vf = vfile_alloc();
    if (!vf) {
        kfree(fc);
        return NULL;
    }

    uint64_t close_token = 0;
    if (uxfs_lifetime_open(p->sb, vn->ino, (uint64_t)(flags & 3),
                           &close_token) != 0) {
        vfile_free(vf);
        kfree(fc);
        return NULL;
    }
    memset(fc, 0, sizeof(*fc));
    fc->sb     = p->sb;
    fc->ino    = vn->ino;
    fc->close_token = close_token;
    fc->close_confirm_mode = close_token != 0 && p->sb->close_confirm_mode;
    fc->is_dir = p->is_dir;
    fc->off    = (flags & 0x400 /* O_APPEND */) ? vn->size : 0;

    vf->vnode = vn;
    vnode_get(vn);
    vf->flags  = flags;
    vf->close_token_capable = fc->close_confirm_mode;
    vf->offset = fc->off;
    vfile_ref_init(vf, 1);
    vf->ops  = &g_uxfs_fops;
    vf->priv = fc;
    return vf;
}

static vnode_ops_t g_uxfs_vnops = {
    .lookup    = uxfs_lookup,
    .create    = uxfs_create,
    .mkdir     = uxfs_mkdir,
    .unlink    = uxfs_unlink,
    .link      = uxfs_link,
    .rmdir     = uxfs_rmdir,
    .rename    = uxfs_rename,
    .stat      = uxfs_stat,
    .statfs    = uxfs_statfs,
    .truncate  = uxfs_truncate,
    .readpage   = uxfs_readpage,
    .writepage  = uxfs_writepage,
    .sync_vnode = uxfs_sync_vnode,
    .open      = uxfs_open_vnode,
    .release   = uxfs_release_vn,
};

/* ------------------------------------------------------------------ */
/* mount / unmount / block IO ownership                                  */
/* ------------------------------------------------------------------ */

#define UXFS_MAX_SBS 8
static uxfs_sb_t g_uxfs_sbs[UXFS_MAX_SBS];

/* Native block-backed mounts consult this before attaching a second cache to
 * a device already owned by a live UXFS service.  The mount table stores the
 * bcache in fs_data for each disk filesystem. */
int uxfs_block_device_owned(block_dev_t *dev)
{
    if (!dev) return 0;
    for (int i = 0; i < UXFS_MAX_SBS; i++) {
        uxfs_sb_t *sb = &g_uxfs_sbs[i];
        if (sb->ep && mount_setup_block_device(sb->block_index) == dev)
            return 1;
    }
    return 0;
}

static int uxfs_device_has_native_mount(block_dev_t *dev)
{
    if (!dev) return 0;
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *mnt = vfs_mount_at(i);
        if (!mnt) continue;
        switch (mnt->type) {
        case FS_TYPE_FAT32:
        case FS_TYPE_EXT4:
        case FS_TYPE_NTFS:
        case FS_TYPE_ISOFS:
        case FS_TYPE_LITTLEFS: {
            bcache_t *bc = (bcache_t *)mnt->fs_data;
            if (bc && bc->dev == dev) return 1;
            break;
        }
        default:
            break;
        }
    }
    return 0;
}

/* Several mounts of one disk number may coexist (each with its own service
 * instance); only the caller's own registration matches, others are skipped. */
static block_dev_t *uxfs_owned_block_dev(struct task_t *task, int block_index)
{
    for (int i = 0; i < UXFS_MAX_SBS; i++) {
        uxfs_sb_t *sb = &g_uxfs_sbs[i];
        if (sb->ep && sb->block_index == block_index &&
            sb->server == task)
            return mount_setup_block_device(block_index);
    }
    return NULL;
}

int uxfs_serve_mount(const char *path, struct a20_channel_ep *ep,
                     struct task_t *server, int block_index,
                     uint32_t serve_flags)
{
    if (!path || !ep)
        return -EINVAL;

    vnode_t *target = vfs_resolve(path);
    if (!target)
        return -ENOENT;
    int is_dir = (target->type == VFS_FT_DIR);
    vnode_put(target);
    if (!is_dir)
        return -ENOTDIR;

    block_dev_t *service_dev = mount_setup_block_device(block_index);
    if (!service_dev)
        return -ENODEV;
    if (uxfs_device_has_native_mount(service_dev))
        return -EBUSY;
    for (int i = 0; i < UXFS_MAX_SBS; i++) {
        if (g_uxfs_sbs[i].ep &&
            mount_setup_block_device(g_uxfs_sbs[i].block_index) == service_dev)
            return -EBUSY;
    }

    int slot = -1;
    for (int i = 0; i < UXFS_MAX_SBS; i++) {
        if (!g_uxfs_sbs[i].ep) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EMFILE;
    uxfs_sb_t *sb = &g_uxfs_sbs[slot];

    sb->ep          = ep;
    sb->server      = proc_get(server);
    if (!sb->server) {
        sb->ep = NULL;
        return -ESRCH;
    }
    sb->lifetime_server = NULL;
    sb->block_index = block_index;
    sb->next_req_id = 0;
    sb->lifetime_mode = 0;
    sb->shutdown_mode = 0;
    sb->shutdown_started = 0;
    sb->lifetime_generation = 0;
    sb->server_generation++;
    if (sb->server_generation == 0)
        sb->server_generation = 1;
    mutex_init(&sb->lifetime_lock);
    mutex_init(&sb->req_lock);

    /*
     * No synchronous handshake: the caller of fs_serve is the service, so
     * blocking here for the INIT reply would self-deadlock.  Liveness is
     * checked by the first real file operation; on death the link drops, -EIO.
     */
    uint64_t root_ino = UFS_ROOT_INO;

    vnode_t *root = uxfs_make_vnode(sb, root_ino, VFS_FT_DIR,
                                    (S_IFDIR | 0755), 0, NULL);
    if (!root) {
        sb->ep = NULL;
        proc_put(sb->server);
        sb->server = NULL;
        return -ENOMEM;
    }

    mount_t *mnt = vfs_mount_alloc();
    if (!mnt) {
        vnode_put(root);
        sb->ep = NULL;
        proc_put(sb->server);
        sb->server = NULL;
        return -ENOMEM;
    }
    strncpy(mnt->path, path, MAX_PATH_LEN - 1);
    mnt->path[MAX_PATH_LEN - 1] = '\0';
    mnt->type  = FS_TYPE_UXFS;
    /* bit0: read-only backend (iso9660) declared; page-cache writes off. */
    mnt->flags = (serve_flags & 1u) ? VFS_MOUNT_RDONLY : 0;
    mnt->root  = root;
    root->mnt  = mnt;
    mnt->fs_data = sb;
    strncpy(mnt->dev, "ufsd", sizeof(mnt->dev) - 1);
    mnt->dev[sizeof(mnt->dev) - 1] = '\0';
    strncpy(mnt->fstype, "uxfs", sizeof(mnt->fstype) - 1);
    mnt->fstype[sizeof(mnt->fstype) - 1] = '\0';
    strncpy(mnt->opts, "rw", sizeof(mnt->opts) - 1);
    mnt->opts[sizeof(mnt->opts) - 1] = '\0';
    root->mnt = mnt;
    vnode_get(root); /* mount holds a persistent ref, like other FS do */
    vfs_mount_link_tree(mnt);
    vfs_dcache_invalidate_all();
    kinfo("[UXFS] mounted user filesystem at %s (block=%d)\n", path,
          block_index);
    return 0;
}

int uxfs_unmount(struct vnode *root)
{
    if (!root)
        return 0;
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)root->fs_data;
    if (!p)
        return 0;
    uxfs_sb_t *sb = p->sb;
    if (sb && sb->ep) {
        /* Negotiate with the existing v1 INIT frame.  A capable peer performs
         * backend sync/unmount and ACKs while block IO ownership is still
         * valid.  Timeout/error leaves the mount and endpoint attached so the
         * caller can retry; it never revokes a live service mid-flush. */
        __atomic_store_n(&sb->shutdown_started, 1, __ATOMIC_RELEASE);
        uint64_t ts[2];
        timekeeping_get_monotonic(ts);
        uint64_t deadline_ns = ts[0] * 1000000000ull + ts[1] +
                               5000000000ull;
        int rc = uxfs_negotiate_lifetime_until(sb, deadline_ns);
        if (rc < 0) {
            if (a20_channel_peer_closed(sb->ep)) {
                /* No task can issue further block I/O.  Drop the dead peer
                 * registration, leaving the filesystem's on-disk dirty
                 * state untouched for recovery on the next mount. */
                uxfs_detach_server(sb);
                return 0;
            }
            /* INIT did not dismantle the backend, so a failed negotiation
             * remains retryable. Keep normal RPCs quiesced until retry or
             * confirmed peer death; a timeout is not proof of no progress. */
            return rc;
        }
        if (a20_channel_peer_closed(sb->ep)) {
            /* A cached v1/v2 negotiation is not proof that this generation
             * still has a live service.  Once the endpoint confirms death,
             * detach it without claiming a clean filesystem shutdown. */
            uxfs_detach_server(sb);
            return 0;
        }
        if (sb->shutdown_mode) {
            ufs_req_hdr_t req;
            ufs_resp_hdr_t resp = {0};
            uxfs_req_init(sb, &req, UFS_OP_SHUTDOWN, UFS_ROOT_INO, 0, 0,
                          NULL, 0);
            req.version = UFS_PROTO_VERSION;
            rc = uxfs_rpc_timeout(sb, &req, NULL, NULL, &resp, NULL, 0,
                                  NULL, deadline_ns, NULL);
            if (rc < 0) {
                if (a20_channel_peer_closed(sb->ep)) {
                    uxfs_detach_server(sb);
                    return 0;
                }
                /* uxfs_rpc_timeout returns a transport error without touching
                 * resp.  Only a matched, complete EBUSY reply can authorize
                 * reopening admission after the service's non-destructive
                 * open-count preflight. */
                if (rc == -EBUSY && resp.out1 == UFS_SHUTDOWN_RETRY_SAFE)
                    __atomic_store_n(&sb->shutdown_started, 0,
                                     __ATOMIC_RELEASE);
                /* The service may still be partway through backend teardown.
                 * Preserve quiescence unless the peer explicitly marks a
                 * pre-teardown EBUSY that left the backend untouched. */
                return rc;
            }
        } else {
            if (a20_channel_peer_closed(sb->ep)) {
                uxfs_detach_server(sb);
                return 0;
            }
            /* Captured v1 peers cannot prove their backend is durable before
             * the kernel revokes block IO.  Keep their mount registered;
             * compatibility does not silently claim clean shutdown. */
            __atomic_store_n(&sb->shutdown_started, 0, __ATOMIC_RELEASE);
            return -EOPNOTSUPP;
        }
        uxfs_detach_server(sb);
    }
    return 0;
}

int uxfs_block_io(struct task_t *task, int block_index, int write,
                  uint64_t lba, void *buf, uint32_t count)
{
    block_dev_t *dev = uxfs_owned_block_dev(task, block_index);
    if (!dev)
        return -ENODEV;
    if (write)
        return dev->write_sector(dev, lba, buf, count) ? -EIO : 0;
    return dev->read_sector(dev, lba, buf, count) ? -EIO : 0;
}

int uxfs_block_capacity(struct task_t *task, int block_index,
                        uint64_t *out_sectors)
{
    /* block_dev_t.capacity is already sectors (virtio config-space
     * semantics, passed through by mount_setup/class_ops): no conversion. */
    block_dev_t *dev = uxfs_owned_block_dev(task, block_index);
    if (!dev || !out_sectors)
        return -ENODEV;
    *out_sectors = dev->capacity;
    return 0;
}

int uxfs_block_flush(struct task_t *task, int block_index)
{
    block_dev_t *dev = uxfs_owned_block_dev(task, block_index);
    if (!dev)
        return -ENODEV;
    if (!dev->flush)
        return -EOPNOTSUPP;
    return dev->flush(dev);
}

int uxfs_block_flush_supported(struct task_t *task, int block_index)
{
    block_dev_t *dev = uxfs_owned_block_dev(task, block_index);
    if (!dev)
        return -ENODEV;
    return dev->flush != NULL;
}

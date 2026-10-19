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
#include "fs/uxfs_lifetime_state.h"
#include "fs/uxfs_ops_state.h"
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
#include "core/lock.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "fs/vfs/vnode_identity.h"

/*
 * Canonical (sb, ino) -> vnode identity.  The Rust module owns the lookup /
 * duplicate / eviction / removal decisions; this adapter owns the IRQ-save
 * spinlock, the entry references, and vnode allocation / release.  Rust is the
 * default production path; UXFS_VNODE_IDENTITY_IMPL=c selects the header-only
 * C reference model (same algorithm) only for the regression image and the
 * host differential oracle.
 */
/* a20_vnode_identity_* is provided by kernel/fs/vfs/vnode_identity.rs */

extern block_dev_t *mount_setup_block_device(int index);

#define UXFS_DBG 0

static spinlock_t g_uxfs_mount_table_lock = SPINLOCK_INIT;
static spinlock_t g_uxfs_identity_lock = SPINLOCK_INIT;

typedef struct uxfs_sb {
    struct a20_channel_ep *ep;   /* service endpoint; ref owned by this sb */
    struct task_t         *server;
    int                    block_index;
    uint32_t               next_req_id;
    /* Rust-owned lifetime/token negotiation state. */
    uxfs_lifetime_state_t  lifetime;
    uint32_t               shutdown_started;
    /* Protected by g_uxfs_mount_table_lock; remains set while a claimed slot
     * is being initialized or retired outside that short critical section. */
    uint32_t               slot_reserved;
    /* Vnodes can outlive the mount endpoint through open files, mappings, and
     * cached pages. Do not reuse this static sb until all such vnodes die. */
    uint32_t               live_vnodes;
    uint64_t               server_generation;
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

/*
 * RPC engine selector.  Rust owns the request admission, deadline/retry
 * sequencing, stale-reply filtering and errno mapping
 * (kernel/fs/uxfs/uxfs_state.rs).  The C adapter keeps the endpoint, the
 * req_lock and the channel send/receive/sleep primitives.
 */
typedef struct a20_uxfs_rpc_ops {
    uint64_t (*now_ns)(void);
    void     (*sleep_tick)(void);
    int      (*try_lock)(void *ctx);
    void     (*unlock)(void *ctx);
    int      (*send)(void *ctx, const uint8_t *msg, uint32_t len,
                     uint64_t deadline_ns, int nonblock);
    int      (*recv)(void *ctx, uint8_t *buf, uint32_t cap,
                     uint64_t deadline_ns, int nonblock, uint32_t *out_len);
    int      (*has_ep)(void *ctx);
    int      (*shutdown_started)(void *ctx);
} a20_uxfs_rpc_ops_t;

extern int a20_uxfs_rpc_run(void *ctx, const a20_uxfs_rpc_ops_t *ops,
                            const ufs_req_hdr_t *req, const uint8_t *msg,
                            uint32_t msg_len, uint8_t *rx, uint32_t rx_cap,
                            ufs_resp_hdr_t *resp, void *resp_payload,
                            uint32_t resp_cap, uint32_t *resp_payload_len,
                            uint64_t deadline_ns, int *request_sent);
_Static_assert(sizeof(ufs_req_hdr_t) == 48, "uxfs rpc mirror: request header");
_Static_assert(sizeof(ufs_resp_hdr_t) == 48, "uxfs rpc mirror: response header");

/* Canonical vnode identity adapter (implementation follows the vnode
 * factory below; detach runs before it). */
static void uxfs_identity_clear_sb(uxfs_sb_t *sb);
static int uxfs_device_has_native_mount(block_dev_t *dev);
static const struct a20_uxfs_lifecycle g_uxfs_lifecycle;

static uint64_t a20_uxfs_bridge_now_ns(void)
{
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    return ts[0] * 1000000000ull + ts[1];
}

static void a20_uxfs_bridge_sleep_tick(void)
{
    uint64_t ticks = MS_TO_TICKS(1);
    proc_sleep_until(timer_get_ticks() + (ticks ? ticks : 1));
}

static int a20_uxfs_bridge_try_lock(void *ctx)
{
    return mutex_trylock(&((uxfs_sb_t *)ctx)->req_lock);
}

static void a20_uxfs_bridge_unlock(void *ctx)
{
    mutex_unlock(&((uxfs_sb_t *)ctx)->req_lock);
}

static int a20_uxfs_bridge_send(void *ctx, const uint8_t *msg, uint32_t len,
                                uint64_t deadline_ns, int nonblock)
{
    uxfs_sb_t *sb = (uxfs_sb_t *)ctx;
    uint32_t flags = (deadline_ns && nonblock) ? A20_MSG_NONBLOCK : 0;
    int64_t r = a20_channel_send(sb->ep, (void *)msg, len, NULL, 0, NULL, flags);
    if (r >= 0)
        return 0; /* SEND_OK */
    if (r == -A20_ERR_WOULD_BLOCK)
        return 1; /* SEND_WOULD_BLOCK */
    return 2;     /* SEND_ERROR */
}

static int a20_uxfs_bridge_recv(void *ctx, uint8_t *buf, uint32_t cap,
                                uint64_t deadline_ns, int nonblock,
                                uint32_t *out_len)
{
    uxfs_sb_t *sb = (uxfs_sb_t *)ctx;
    uint32_t in_len = cap;
    a20_ch_handle_info_t hinfos[1];
    uint32_t nh = 0;
    uint32_t flags = (deadline_ns && nonblock) ? A20_MSG_NONBLOCK : 0;
    int64_t r = a20_channel_recv_begin(sb->ep, flags, &in_len, &nh);
    if (r == -A20_ERR_WOULD_BLOCK)
        return -1; /* RECV_WOULD_BLOCK */
    if (r < 0)
        return -2; /* RECV_ERROR */
    if (in_len > cap) {
        a20_channel_recv_abort(sb->ep);
        return -3; /* RECV_INVALID */
    }
    uint32_t out = in_len;
    r = a20_channel_recv_finish(sb->ep, buf, &out, hinfos, &nh);
    if (r < 0)
        return -2;
    *out_len = out;
    return (int)out;
}

static int a20_uxfs_bridge_has_ep(void *ctx)
{
    return ((uxfs_sb_t *)ctx)->ep != NULL;
}

static int a20_uxfs_bridge_shutdown_started(void *ctx)
{
    return __atomic_load_n(&((uxfs_sb_t *)ctx)->shutdown_started,
                           __ATOMIC_ACQUIRE) != 0;
}

/* ------------------------------------------------------------------ */
/* RPC engine                                                           */
/* ------------------------------------------------------------------ */

static int uxfs_rpc_timeout(uxfs_sb_t *sb, const ufs_req_hdr_t *req,
                    const char *name, const void *payload,
                    ufs_resp_hdr_t *resp, void *resp_payload,
                    uint32_t resp_cap, uint32_t *resp_payload_len,
                    uint64_t deadline_ns, int *request_sent)
{
    /* Mechanical bridge table the Rust engine drives.  See
     * kernel/fs/uxfs/uxfs_state.rs for the state machine. */
    static const a20_uxfs_rpc_ops_t ops = {
        .now_ns = a20_uxfs_bridge_now_ns,
        .sleep_tick = a20_uxfs_bridge_sleep_tick,
        .try_lock = a20_uxfs_bridge_try_lock,
        .unlock = a20_uxfs_bridge_unlock,
        .send = a20_uxfs_bridge_send,
        .recv = a20_uxfs_bridge_recv,
        .has_ep = a20_uxfs_bridge_has_ep,
        .shutdown_started = a20_uxfs_bridge_shutdown_started,
    };
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

    uint8_t *rx = (uint8_t *)kmalloc(A20_CH_MAX_DATA);
    if (!rx) {
        kfree(msg);
        return -ENOMEM;
    }
    uint32_t plen = 0;
    int sent = 0;
    int rc = a20_uxfs_rpc_run(sb, &ops, req, msg, total, rx, A20_CH_MAX_DATA,
                              resp, resp_payload, resp_cap, &plen,
                              deadline_ns, &sent);
    if (resp_payload_len)
        *resp_payload_len = plen;
    if (request_sent)
        *request_sent = sent;
    kfree(rx);
    kfree(msg);
    return rc;
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
    if (a20_uxfs_lifetime_negotiated(&sb->lifetime, sb->server_generation,
                                     sb->server)) {
        mutex_unlock(&sb->lifetime_lock);
        return 0;
    }
    a20_uxfs_lifetime_reset(&sb->lifetime);
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
    /* The capability-to-mode/token/confirm/shutdown decision table lives in
     * kernel/fs/uxfs/lifetime_state.rs. */
    a20_uxfs_lifetime_apply_init(&sb->lifetime, resp.out1, resp.out2,
                                 sb->server_generation, sb->server);
    printf("[UXFS] lifetime protocol version=%u server_generation=%u\n",
           sb->lifetime.mode, sb->server_generation);
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
    /* Fail closed for every old vnode/fctx before withdrawing its endpoint.
     * Serialize the pointer change with the RPC engine, then keep admission
     * shut until the static sb slot is reused by a fresh mount. */
    __atomic_store_n(&sb->shutdown_started, 1, __ATOMIC_RELEASE);
    mutex_lock(&sb->req_lock);
    uint64_t mount_flags = spin_lock_irqsave(&g_uxfs_mount_table_lock);
    uint64_t identity_flags = spin_lock_irqsave(&g_uxfs_identity_lock);
    sb->slot_reserved = 1;
    a20_channel_ep_t *ep = sb->ep;
    sb->ep = NULL; /* withdraw block-I/O authority before releasing refs */
    spin_unlock_irqrestore(&g_uxfs_identity_lock, identity_flags);
    struct task_t *server = sb->server;
    sb->server = NULL;
    a20_uxfs_lifetime_reset(&sb->lifetime);
    spin_unlock_irqrestore(&g_uxfs_mount_table_lock, mount_flags);
    mutex_unlock(&sb->req_lock);
    if (ep) {
        a20_channel_ep_peer_shutdown(ep);
        a20_channel_ep_release(ep);
    }
    if (server)
        proc_put(server);
    /* The mount is going away: no vnode of this superblock stays canonical.
     * Old vnodes may outlive the mount, so the slot stays unavailable until
     * uxfs_release_vn() drops live_vnodes to zero. */
    uxfs_identity_clear_sb(sb);
    mount_flags = spin_lock_irqsave(&g_uxfs_mount_table_lock);
    sb->slot_reserved = 0;
    spin_unlock_irqrestore(&g_uxfs_mount_table_lock, mount_flags);
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
    if (a20_uxfs_lifetime_is_v1(&sb->lifetime))
        return 0;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp = {0};
    uxfs_req_init(sb, &req, UFS_OP_OPEN, ino, flags, 0, NULL, 0);
    rc = uxfs_rpc(sb, &req, NULL, NULL, &resp, NULL, 0, NULL);
    if (rc == 0 && close_token)
        *close_token = a20_uxfs_lifetime_open_token(&sb->lifetime, resp.out2);
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
    if (a20_uxfs_lifetime_close_v1_acked(&sb->lifetime)) {
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
    __atomic_add_fetch(&sb->live_vnodes, 1, __ATOMIC_RELEASE);
    return vn;
}


/* ------------------------------------------------------------------ */
/* canonical vnode identity                                             */
/* ------------------------------------------------------------------ */

/* One live vnode per (sb, inode), so every open file, shared mapping and
 * page-cache read observes one vnode (and its size / page state).  The lock
 * is a leaf IRQ-save spinlock: inside it, entries are only touched through
 * vnode_get and handle bookkeeping, while every put that can free a vnode
 * runs outside. */
#define UXFS_IDENTITY_PRUNE_BATCH 64
#define UXFS_IDENTITY_INSERT_ATTEMPTS 4
static uintptr_t uxfs_identity_ref_peek(uintptr_t vnode)
{
    return (uintptr_t)vnode_ref_read((vnode_t *)vnode);
}

static void uxfs_identity_register_peek(void)
{
    uint64_t flags = spin_lock_irqsave(&g_uxfs_identity_lock);
    a20_vnode_identity_set_ref_peek(uxfs_identity_ref_peek);
    spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
}

/* Canonical lookup-or-create for (sb, ino); returns a vnode carrying the
 * caller's reference, or NULL when allocation fails.  A hit refreshes the
 * cached attributes from the reply: the size only grows here (truncate sets
 * the exact size) and the mode always follows the server.  A missing inode
 * number or exhausted identity table fails closed; a private vnode would
 * silently recreate the stale-size bug this index exists to prevent. */
static vnode_t *uxfs_iget(uxfs_sb_t *sb, uint64_t ino, int vfs_type,
                          uint32_t mode, size_t size, vnode_t *parent)
{
    uint64_t flags;
    vnode_t *vn;

    /* Directory identity remains path-local: vnode->parent supplies `..`,
     * and dentry aliases can move. The canonical (sb, ino) cache is for
     * non-directory file data/page-cache coherence. */
    if (vfs_type == VFS_FT_DIR)
        return ino ? uxfs_make_vnode(sb, ino, vfs_type, mode, size, parent)
                   : NULL;

    flags = spin_lock_irqsave(&g_uxfs_identity_lock);
    vn = (vnode_t *)a20_vnode_identity_lookup((uintptr_t)sb, ino);
    if (vn) {
        vnode_get(vn); /* caller reference */
        spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
        if (size > vn->size)
            vn->size = size;
        vn->mode = mode;
        return vn;
    }
    spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);

    vn = uxfs_make_vnode(sb, ino, vfs_type, mode, size, parent);
    if (!vn)
        return NULL;
    if (ino == 0) {
        /* A successful LOOKUP/CREATE without an inode is malformed. */
        vnode_put(vn);
        return NULL;
    }

    for (int attempt = 0; attempt < UXFS_IDENTITY_INSERT_ATTEMPTS; attempt++) {
        a20_vnode_identity_insert_out_t out;
        uintptr_t victims[UXFS_IDENTITY_PRUNE_BATCH];
        size_t n;

        flags = spin_lock_irqsave(&g_uxfs_identity_lock);
        /* Detach withdraws ep under req_lock before clearing this index.
         * Check admission while holding the same lock used by that clear: an
         * in-flight lookup can either insert before detach (and be cleared),
         * or observe the closed endpoint and fail, but cannot repopulate it. */
        if (!sb->ep || __atomic_load_n(&sb->shutdown_started,
                                       __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
            vnode_put(vn);
            return NULL;
        }
        a20_vnode_identity_insert((uintptr_t)sb, ino, (uintptr_t)vn, &out);
        if (out.status == A20_VNODE_IDENTITY_INSERTED) {
            vnode_get(vn); /* entry reference */
            spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
            return vn;
        }
        if (out.status == A20_VNODE_IDENTITY_DUPLICATE) {
            vnode_t *existing = (vnode_t *)out.existing;
            vnode_get(existing); /* caller reference */
            spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
            vnode_put(vn); /* dispose the duplicate candidate */
            if (size > existing->size)
                existing->size = size;
            existing->mode = mode;
            return existing;
        }
        /* FULL: evict cache-only entries and retry; their references are
         * dropped outside the lock because vnode_put can free. */
        n = a20_vnode_identity_prune_batch(victims, UXFS_IDENTITY_PRUNE_BATCH);
        spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
        for (size_t i = 0; i < n; i++)
            vnode_put((vnode_t *)victims[i]);
        if (n == 0)
            break;
    }
    vnode_put(vn);
    return NULL;
}

/* Drop the canonical entry for (sb, ino).  Any open file keeps its vnode
 * alive through its own reference; the vnode just stops being the shared
 * instance for the inode.  Used when a name disappears or an inode number
 * is reused by a fresh create/mkdir. */
static void uxfs_identity_forget(uxfs_sb_t *sb, uint64_t ino)
{
    uint64_t flags;
    vnode_t *victim;

    if (!ino)
        return;
    flags = spin_lock_irqsave(&g_uxfs_identity_lock);
    victim = (vnode_t *)a20_vnode_identity_remove((uintptr_t)sb, ino);
    spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
    if (victim)
        vnode_put(victim);
}

/* Remove every canonical entry of one superblock (unmount and failed mount).
 * Old vnode objects may still point at this sb; live_vnodes prevents slot
 * reuse until their final references are gone. */
static void uxfs_identity_clear_sb(uxfs_sb_t *sb)
{
    for (;;) {
        uintptr_t victims[32];
        uint64_t flags = spin_lock_irqsave(&g_uxfs_identity_lock);
        size_t n = a20_vnode_identity_clear_sb((uintptr_t)sb, victims, 32);
        spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
        if (!n)
            break;
        for (size_t i = 0; i < n; i++)
            vnode_put((vnode_t *)victims[i]);
    }
}

/* Drop every cache-only canonical vnode of every uxfs mount (procfs
 * drop_caches bit 1).  Entries whose vnode is still referenced by an open
 * file, mapping or cached page stay. */
void uxfs_vnode_cache_prune_all(void)
{
    for (;;) {
        uintptr_t victims[UXFS_IDENTITY_PRUNE_BATCH];
        uint64_t flags = spin_lock_irqsave(&g_uxfs_identity_lock);
        a20_vnode_identity_set_ref_peek(uxfs_identity_ref_peek);
        size_t n = a20_vnode_identity_prune_batch(victims,
                                                  UXFS_IDENTITY_PRUNE_BATCH);
        spin_unlock_irqrestore(&g_uxfs_identity_lock, flags);
        if (!n)
            break;
        for (size_t i = 0; i < n; i++)
            vnode_put((vnode_t *)victims[i]);
    }
}

/* ------------------------------------------------------------------ */
/* vnode_ops                                                           */
/* ------------------------------------------------------------------ */

/*
 * Mechanical bridge table the Rust per-operation decision engine drives.  The
 * engine (kernel/fs/uxfs/uxfs_ops_state.rs) owns request construction,
 * response decoding and the canonical-identity / page-cache / size state
 * transitions; this adapter only supplies req_init (which allocates the sb's
 * next req_id), the RPC transport call, vnode accessors, iget / identity
 * forget / page-cache-truncate, and the lifetime gate for LINK.
 */
static void ops_req_init(void *ctx, struct ufs_req_hdr *req, uint32_t opcode,
                         uint64_t ino, uint64_t arg0, uint64_t arg1,
                         const char *name, uint32_t payload_len)
{
    uxfs_req_init((uxfs_sb_t *)ctx, (ufs_req_hdr_t *)req, opcode, ino, arg0,
                  arg1, name, payload_len);
}
static int ops_rpc(void *ctx, const struct ufs_req_hdr *req, const char *name,
                   const void *payload, struct ufs_resp_hdr *resp,
                   void *resp_payload, uint32_t resp_cap,
                   uint32_t *resp_payload_len)
{
    return uxfs_rpc((uxfs_sb_t *)ctx, (const ufs_req_hdr_t *)req, name, payload,
                    (ufs_resp_hdr_t *)resp, resp_payload, resp_cap,
                    resp_payload_len);
}
static void *ops_vpriv_sb(void *vnode) { return ((vnode_t *)vnode)->fs_data
    ? ((uxfs_vpriv_t *)((vnode_t *)vnode)->fs_data)->sb : 0; }
static uint64_t ops_vn_ino(void *vnode) { return ((vnode_t *)vnode)->ino; }
static uint64_t ops_vn_size(void *vnode) { return (uint64_t)((vnode_t *)vnode)->size; }
static void ops_vn_set_size(void *vnode, uint64_t sz) { ((vnode_t *)vnode)->size = (size_t)sz; }
static void *ops_vn_parent(void *vnode) { return ((vnode_t *)vnode)->parent; }
static void *ops_iget(void *ctx, uint64_t ino, int vfs_type, uint32_t mode,
                      uint64_t size, void *parent)
{
    return uxfs_iget((uxfs_sb_t *)ctx, ino, vfs_type, mode, (size_t)size,
                     (vnode_t *)parent);
}
static void ops_identity_forget(void *ctx, uint64_t ino)
{
    uxfs_identity_forget((uxfs_sb_t *)ctx, ino);
}
static void ops_vnode_get(void *vnode) { vnode_get((vnode_t *)vnode); }
static void ops_page_cache_truncate(void *vnode, uint64_t sz)
{
    page_cache_truncate((vnode_t *)vnode, (size_t)sz);
}
static int ops_negotiate_lifetime(void *ctx)
{
    return uxfs_negotiate_lifetime((uxfs_sb_t *)ctx);
}
static int ops_lifetime_is_v2(void *ctx)
{
    return a20_uxfs_lifetime_is_v2(&((uxfs_sb_t *)ctx)->lifetime);
}

static const struct a20_uxfs_ops g_uxfs_ops = {
    .req_init = ops_req_init,
    .rpc = ops_rpc,
    .vpriv_sb = ops_vpriv_sb,
    .vn_ino = ops_vn_ino,
    .vn_size = ops_vn_size,
    .vn_set_size = ops_vn_set_size,
    .vn_parent = ops_vn_parent,
    .iget = ops_iget,
    .identity_forget = ops_identity_forget,
    .vnode_get = ops_vnode_get,
    .page_cache_truncate = ops_page_cache_truncate,
    .negotiate_lifetime = ops_negotiate_lifetime,
    .lifetime_is_v2 = ops_lifetime_is_v2,
};
/* The Rust mirror (kernel/fs/uxfs/uxfs_ops_state.rs) asserts the wire headers;
 * this pins the bridge-table field count so a C/Rust drift fails the build. */
_Static_assert(sizeof(g_uxfs_ops) / sizeof(void *) == 13,
               "uxfs ops mirror: bridge table field count");
_Static_assert(sizeof(struct ufs_req_hdr) == 48, "uxfs ops mirror: request header");
_Static_assert(sizeof(struct ufs_resp_hdr) == 48, "uxfs ops mirror: response header");

/* Open-file (fctx/vfile) bridge for the Rust data-plane engine. */
static void *ops_fc_sb(void *fc) { return ((uxfs_fctx_t *)fc)->sb; }
static uint64_t ops_fc_ino(const void *fc) { return ((const uxfs_fctx_t *)fc)->ino; }
static uint64_t ops_fc_off(const void *fc) { return (uint64_t)((const uxfs_fctx_t *)fc)->off; }
static void ops_fc_set_off(void *fc, uint64_t off) { ((uxfs_fctx_t *)fc)->off = (size_t)off; }
static int ops_fc_is_dir(const void *fc) { return ((const uxfs_fctx_t *)fc)->is_dir; }
static void *ops_vf_vnode(void *vf) { return ((vfile_t *)vf)->vnode; }
static void ops_vf_set_offset(void *vf, uint64_t off) { ((vfile_t *)vf)->offset = (size_t)off; }
static uint64_t ops_file_vn_size(void *vn) { return (uint64_t)((vnode_t *)vn)->size; }
static void ops_file_vn_set_size(void *vn, uint64_t sz) { ((vnode_t *)vn)->size = (size_t)sz; }

static const struct a20_uxfs_fops g_uxfs_file_ops = {
    .req_init = ops_req_init,
    .rpc = ops_rpc,
    .fc_sb = ops_fc_sb,
    .fc_ino = ops_fc_ino,
    .fc_off = ops_fc_off,
    .fc_set_off = ops_fc_set_off,
    .fc_is_dir = ops_fc_is_dir,
    .vf_vnode = ops_vf_vnode,
    .vf_set_offset = ops_vf_set_offset,
    .vn_size = ops_file_vn_size,
    .vn_set_size = ops_file_vn_set_size,
};
_Static_assert(sizeof(g_uxfs_file_ops) / sizeof(void *) == 11,
               "uxfs file-ops mirror: bridge table field count");
_Static_assert(sizeof(uxfs_fctx_t) == 40, "uxfs file-ops mirror: fctx layout");

static int uxfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    return a20_uxfs_op_lookup(&g_uxfs_ops, dir, name, (void **)out);
}

static int uxfs_stat(vnode_t *vn, kstat_t *st)
{
    return a20_uxfs_op_stat(&g_uxfs_ops, vn, st);
}

static int uxfs_statfs(vnode_t *vn, kstatfs_t *st)
{
    return a20_uxfs_op_statfs(&g_uxfs_ops, vn, st);
}

static int uxfs_create(vnode_t *dir, const char *name, int mode,
                       vnode_t **out)
{
    return a20_uxfs_op_create(&g_uxfs_ops, dir, name, (uint32_t)mode,
                              (void **)out);
}

static int uxfs_mkdir(vnode_t *dir, const char *name, int mode)
{
    return a20_uxfs_op_mkdir(&g_uxfs_ops, dir, name, (uint32_t)mode);
}

static int uxfs_unlink(vnode_t *dir, const char *name)
{
    return a20_uxfs_op_unlink(&g_uxfs_ops, dir, name);
}

static int uxfs_link(vnode_t *dir, const char *name, vnode_t *target)
{
    return a20_uxfs_op_link(&g_uxfs_ops, dir, name, target);
}

static int uxfs_rmdir(vnode_t *dir, const char *name)
{
    return a20_uxfs_op_rmdir(&g_uxfs_ops, dir, name);
}

static int uxfs_rename(vnode_t *old_dir, const char *old_name,
                       vnode_t *new_dir, const char *new_name,
                       unsigned int flags)
{
    (void)flags; /* extended semantics like RENAME_EXCHANGE are unsupported */
    return a20_uxfs_op_rename(&g_uxfs_ops, old_dir, old_name, new_dir, new_name);
}

static int uxfs_truncate(vnode_t *vn, size_t size)
{
    return a20_uxfs_op_truncate(&g_uxfs_ops, vn, (uint64_t)size);
}

static int uxfs_sync_vnode(vnode_t *vn)
{
    return a20_uxfs_op_sync_vnode(&g_uxfs_ops, vn);
}

static int uxfs_readpage(vnode_t *vn, uint64_t index, void *data, size_t len)
{
    return a20_uxfs_op_readpage(&g_uxfs_ops, vn, index, data, (uint64_t)len);
}

static int uxfs_writepage(vnode_t *vn, uint64_t index, const void *data,
                          size_t len)
{
    return a20_uxfs_op_writepage(&g_uxfs_ops, vn, index, data, (uint64_t)len);
}

void uxfs_release_vn(vnode_t *vn)
{
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)vn->fs_data;
    uxfs_sb_t *sb = p ? p->sb : NULL;
    if (p) {
        kfree(p);
        vn->fs_data = NULL;
    }
    if (vn->parent)
        vnode_put(vn->parent);
    kfree(vn);
    /* Keep this as the final access: zero permits another mount to reuse sb. */
    if (sb)
        __atomic_sub_fetch(&sb->live_vnodes, 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* vfile_ops                                                           */
/* ------------------------------------------------------------------ */

static int uxfs_fread(vfile_t *vf, char *buf, size_t count)
{
    return a20_uxfs_file_read(&g_uxfs_file_ops, vf->priv, vf, buf, count);
}

static int uxfs_fwrite(vfile_t *vf, const char *buf, size_t count)
{
    return a20_uxfs_file_write(&g_uxfs_file_ops, vf->priv, vf, buf, count);
}

static long uxfs_flseek(vfile_t *vf, long offset, int whence)
{
    return (long)a20_uxfs_file_lseek(&g_uxfs_file_ops, vf->priv, vf,
                                     (int64_t)offset, whence);
}

static int uxfs_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    /* Rust decodes the RPC payload after the transport has released its
     * request lock.  A shared static buffer lets another open directory
     * overwrite that reply during the decode, including across superblocks. */
    uint8_t *payload = (uint8_t *)kmalloc(UFS_MAX_PAYLOAD);
    if (!payload)
        return -ENOMEM;
    int rc = a20_uxfs_file_readdir(&g_uxfs_file_ops, vf->priv, vf, dirp, count,
                                   payload, UFS_MAX_PAYLOAD);
    kfree(payload);
    return rc;
}

static int uxfs_fclose(vfile_t *vf)
{
    return a20_uxfs_file_close(&g_uxfs_lifecycle, vf);
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
    return (struct vfile *)a20_uxfs_file_open(&g_uxfs_lifecycle, vn, flags);
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

/* Mechanical bridge for the Rust namespace engine (mount/serve/unmount and
 * block-device ownership).  Every function here is a single store, a single
 * VFS/identity call, or a lock wrapper; all branching lives in
 * kernel/fs/uxfs/uxfs_ops_state.rs. */
static void *ops_ns_sb_table(void) { return g_uxfs_sbs; }
static uint64_t ops_ns_sb_stride(void) { return sizeof(uxfs_sb_t); }
static int ops_ns_sb_count(void) { return UXFS_MAX_SBS; }
static uint64_t ops_ns_lock_mounts(void) { return spin_lock_irqsave(&g_uxfs_mount_table_lock); }
static void ops_ns_unlock_mounts(uint64_t f) { spin_unlock_irqrestore(&g_uxfs_mount_table_lock, f); }
static void *ops_ns_block_device(int idx) { return mount_setup_block_device(idx); }
static int ops_ns_device_has_native(void *dev) { return uxfs_device_has_native_mount((block_dev_t *)dev); }
static int ops_ns_path_is_dir(const char *path)
{
    vnode_t *t = vfs_resolve(path);
    if (!t)
        return -ENOENT;
    int is_dir = (t->type == VFS_FT_DIR);
    vnode_put(t);
    return is_dir ? 1 : 0;
}
static void *ops_ns_iget(void *sb, uint64_t ino, int type, uint32_t mode,
                         uint64_t size, void *parent)
{
    return uxfs_iget((uxfs_sb_t *)sb, ino, type, mode, (size_t)size,
                     (vnode_t *)parent);
}
static void *ops_ns_mount_alloc(void) { return vfs_mount_alloc(); }
static void ops_ns_mount_init(void *mnt, const char *path, uint32_t flags,
                              void *root, void *sb)
{
    mount_t *m = (mount_t *)mnt;
    vnode_t *r = (vnode_t *)root;
    strncpy(m->path, path, MAX_PATH_LEN - 1);
    m->path[MAX_PATH_LEN - 1] = '\0';
    m->type = FS_TYPE_UXFS;
    m->flags = flags;
    m->root = r;
    m->fs_data = sb;
    strncpy(m->dev, "ufsd", sizeof(m->dev) - 1);
    m->dev[sizeof(m->dev) - 1] = '\0';
    strncpy(m->fstype, "uxfs", sizeof(m->fstype) - 1);
    m->fstype[sizeof(m->fstype) - 1] = '\0';
    strncpy(m->opts, "rw", sizeof(m->opts) - 1);
    m->opts[sizeof(m->opts) - 1] = '\0';
    r->mnt = m;
}
static void ops_ns_mount_link_tree(void *mnt) { vfs_mount_link_tree((mount_t *)mnt); }
static void ops_ns_dcache_invalidate_all(void *x) { (void)x; vfs_dcache_invalidate_all(); }
static void ops_ns_identity_register_peek(void *x) { (void)x; uxfs_identity_register_peek(); }
static void ops_ns_identity_clear_sb(void *sb) { uxfs_identity_clear_sb((uxfs_sb_t *)sb); }
static void ops_ns_vnode_get(void *vn) { vnode_get((vnode_t *)vn); }
static void ops_ns_vnode_put(void *vn) { vnode_put((vnode_t *)vn); }
static void *ops_ns_vnode_fs_data(void *vn) { return ((vnode_t *)vn)->fs_data; }
static void ops_ns_lifetime_reset(void *p) { a20_uxfs_lifetime_reset((uxfs_lifetime_state_t *)p); }
static void *ops_ns_proc_get(void *t) { return proc_get((struct task_t *)t); }
static void ops_ns_proc_put(void *t) { proc_put((struct task_t *)t); }
static void ops_ns_slot_init_req_lock(void *sb) { mutex_init(&((uxfs_sb_t *)sb)->req_lock); }
static void ops_ns_detach_server(void *sb) { uxfs_detach_server((uxfs_sb_t *)sb); }
static int ops_ns_peer_closed(void *ep) { return a20_channel_peer_closed((struct a20_channel_ep *)ep); }
static uint64_t ops_ns_now_ns(void)
{
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    return ts[0] * 1000000000ull + ts[1];
}
static int ops_ns_negotiate_until(void *sb, uint64_t dl)
{
    return uxfs_negotiate_lifetime_until((uxfs_sb_t *)sb, dl);
}
static int ops_ns_shutdown_rpc(void *sb, uint64_t dl, int *retry_safe)
{
    uxfs_sb_t *s = (uxfs_sb_t *)sb;
    ufs_req_hdr_t req;
    ufs_resp_hdr_t resp = {0};
    uxfs_req_init(s, &req, UFS_OP_SHUTDOWN, UFS_ROOT_INO, 0, 0, NULL, 0);
    req.version = UFS_PROTO_VERSION;
    int rc = uxfs_rpc_timeout(s, &req, NULL, NULL, &resp, NULL, 0, NULL, dl, NULL);
    if (retry_safe)
        *retry_safe = (rc == -EBUSY && resp.out1 == UFS_SHUTDOWN_RETRY_SAFE) ? 1 : 0;
    return rc;
}
static void ops_ns_mount_log(const char *path, int block_index)
{
    kinfo("[UXFS] mounted user filesystem at %s (block=%d)\n", path, block_index);
}

static const struct a20_uxfs_ns g_uxfs_ns = {
    .sb_table = ops_ns_sb_table,
    .sb_stride = ops_ns_sb_stride,
    .sb_count = ops_ns_sb_count,
    .lock_mounts = ops_ns_lock_mounts,
    .unlock_mounts = ops_ns_unlock_mounts,
    .block_device = ops_ns_block_device,
    .device_has_native_mount = ops_ns_device_has_native,
    .path_is_dir = ops_ns_path_is_dir,
    .iget = ops_ns_iget,
    .mount_alloc = ops_ns_mount_alloc,
    .mount_init = ops_ns_mount_init,
    .mount_link_tree = ops_ns_mount_link_tree,
    .dcache_invalidate_all = ops_ns_dcache_invalidate_all,
    .identity_register_peek = ops_ns_identity_register_peek,
    .identity_clear_sb = ops_ns_identity_clear_sb,
    .vnode_get = ops_ns_vnode_get,
    .vnode_put = ops_ns_vnode_put,
    .vnode_fs_data = ops_ns_vnode_fs_data,
    .lifetime_reset = ops_ns_lifetime_reset,
    .proc_get = ops_ns_proc_get,
    .proc_put = ops_ns_proc_put,
    .slot_init_req_lock = ops_ns_slot_init_req_lock,
    .detach_server = ops_ns_detach_server,
    .peer_closed = ops_ns_peer_closed,
    .now_ns = ops_ns_now_ns,
    .negotiate_lifetime_until = ops_ns_negotiate_until,
    .shutdown_rpc = ops_ns_shutdown_rpc,
    .mount_log = ops_ns_mount_log,
};
_Static_assert(sizeof(g_uxfs_ns) / sizeof(void *) == 28,
               "uxfs namespace mirror: bridge table field count");
_Static_assert(__builtin_offsetof(uxfs_sb_t, server_generation) == 72,
               "uxfs namespace mirror: sb view prefix layout");

/* Mechanical bridge for the Rust open/close lifecycle engine. */
static void *ops_lc_fc_alloc(void) { return kmalloc(sizeof(uxfs_fctx_t)); }
static void *ops_lc_vfile_alloc(void) { return vfile_alloc(); }
static void ops_lc_vfile_free(void *vf) { vfile_free((vfile_t *)vf); }
static void ops_lc_kfree(void *p) { kfree(p); }
static void *ops_lc_vn_vpriv(void *vn) { return ((vnode_t *)vn)->fs_data; }
static void *ops_lc_vpriv_sb(void *p) { return ((uxfs_vpriv_t *)p)->sb; }
static int ops_lc_vpriv_is_dir(void *p) { return ((uxfs_vpriv_t *)p)->is_dir; }
static uint64_t ops_lc_vn_ino(const void *vn) { return ((const vnode_t *)vn)->ino; }
static uint64_t ops_lc_vn_size(const void *vn) { return (uint64_t)((const vnode_t *)vn)->size; }
static int ops_lc_lifetime_open(void *sb, uint64_t ino, uint64_t flags,
                                uint64_t *token)
{
    return uxfs_lifetime_open((uxfs_sb_t *)sb, ino, flags, token);
}
static int ops_lc_lifetime_confirm_mode(void *sb)
{
    return ((uxfs_sb_t *)sb)->lifetime.close_confirm_mode != 0;
}
static void ops_lc_fc_init(void *fc, void *sb, uint64_t ino, uint64_t token,
                           int confirm, int is_dir, uint64_t off)
{
    uxfs_fctx_t *c = (uxfs_fctx_t *)fc;
    memset(c, 0, sizeof(*c));
    c->sb = (uxfs_sb_t *)sb;
    c->ino = ino;
    c->close_token = token;
    c->close_confirm_mode = (uint8_t)confirm;
    c->is_dir = is_dir;
    c->off = (size_t)off;
}
static void ops_lc_vf_init(void *vf, void *vn, int flags, int capable,
                           uint64_t off, void *priv)
{
    vfile_t *v = (vfile_t *)vf;
    v->vnode = (vnode_t *)vn;
    vnode_get((vnode_t *)vn);
    v->flags = flags;
    v->close_token_capable = (uint8_t)capable;
    v->offset = (size_t)off;
    vfile_ref_init(v, 1);
    v->ops = &g_uxfs_fops;
    v->priv = priv;
}
static void *ops_lc_fc_sb(void *fc) { return ((uxfs_fctx_t *)fc)->sb; }
static uint64_t ops_lc_fc_ino(const void *fc) { return ((const uxfs_fctx_t *)fc)->ino; }
static uint64_t ops_lc_fc_close_token(const void *fc) { return ((const uxfs_fctx_t *)fc)->close_token; }
static int ops_lc_fc_confirm(const void *fc) { return ((const uxfs_fctx_t *)fc)->close_confirm_mode; }
static int ops_lc_fc_accepted(const void *fc) { return ((const uxfs_fctx_t *)fc)->close_accepted; }
static void ops_lc_fc_set_accepted(void *fc, int v) { ((uxfs_fctx_t *)fc)->close_accepted = (uint8_t)v; }
static void *ops_lc_vf_vnode(void *vf) { return ((vfile_t *)vf)->vnode; }
static void *ops_lc_vf_priv(void *vf) { return ((vfile_t *)vf)->priv; }
static void ops_lc_vf_set_close_attempt(void *vf, int sent, int acked, int retry_safe)
{
    vfile_t *v = (vfile_t *)vf;
    v->close_attempt_sent = (uint8_t)sent;
    v->close_attempt_acked = (uint8_t)acked;
    v->close_attempt_retry_safe = (uint8_t)retry_safe;
}
static void ops_lc_fc_release(void *vf, void *fc)
{
    kfree(fc);
    ((vfile_t *)vf)->priv = NULL;
}
static int ops_lc_pc_writeback(void *vn)
{
    return page_cache_writeback_vnode((vnode_t *)vn, NULL, NULL);
}
static int ops_lc_lifetime_close(void *sb, uint64_t ino, uint64_t token,
                                 int *acked, int *sent)
{
    return uxfs_lifetime_close((uxfs_sb_t *)sb, ino, token, acked, sent);
}
static int ops_lc_lifetime_close_confirm(void *sb, uint64_t ino, uint64_t token,
                                         int *acked, int *sent)
{
    return uxfs_lifetime_close_confirm((uxfs_sb_t *)sb, ino, token, acked, sent);
}
static void ops_lc_trace(void *vf, void *fc)
{
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] uxfs_fclose vf=%lu ino=%lu accepted=%u confirm=%u token_cap=%u\n",
          (unsigned long)((vfile_t *)vf)->identity,
          (unsigned long)((uxfs_fctx_t *)fc)->ino,
          (unsigned)((uxfs_fctx_t *)fc)->close_accepted,
          (unsigned)((uxfs_fctx_t *)fc)->close_confirm_mode,
          (unsigned)((vfile_t *)vf)->close_token_capable);
#else
    (void)vf;
    (void)fc;
#endif
}

static const struct a20_uxfs_lifecycle g_uxfs_lifecycle = {
    .fc_alloc = ops_lc_fc_alloc,
    .vfile_alloc = ops_lc_vfile_alloc,
    .vfile_free = ops_lc_vfile_free,
    .kfree = ops_lc_kfree,
    .vn_vpriv = ops_lc_vn_vpriv,
    .vpriv_sb = ops_lc_vpriv_sb,
    .vpriv_is_dir = ops_lc_vpriv_is_dir,
    .vn_ino = ops_lc_vn_ino,
    .vn_size = ops_lc_vn_size,
    .lifetime_open = ops_lc_lifetime_open,
    .lifetime_confirm_mode = ops_lc_lifetime_confirm_mode,
    .fc_init = ops_lc_fc_init,
    .vf_init = ops_lc_vf_init,
    .fc_sb = ops_lc_fc_sb,
    .fc_ino = ops_lc_fc_ino,
    .fc_close_token = ops_lc_fc_close_token,
    .fc_close_confirm_mode = ops_lc_fc_confirm,
    .fc_close_accepted = ops_lc_fc_accepted,
    .fc_set_close_accepted = ops_lc_fc_set_accepted,
    .vf_vnode = ops_lc_vf_vnode,
    .vf_priv = ops_lc_vf_priv,
    .vf_set_close_attempt = ops_lc_vf_set_close_attempt,
    .fc_release = ops_lc_fc_release,
    .pc_writeback = ops_lc_pc_writeback,
    .lifetime_close = ops_lc_lifetime_close,
    .lifetime_close_confirm = ops_lc_lifetime_close_confirm,
    .trace = ops_lc_trace,
};
_Static_assert(sizeof(g_uxfs_lifecycle) / sizeof(void *) == 27,
               "uxfs lifecycle mirror: bridge table field count");

/* Native block-backed mounts consult this before attaching a second cache to
 * a device already owned by a live UXFS service.  The mount table stores the
 * bcache in fs_data for each disk filesystem. */
int uxfs_block_device_owned(block_dev_t *dev)
{
    return a20_uxfs_ns_device_owned(&g_uxfs_ns, dev);
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
    return (block_dev_t *)a20_uxfs_ns_owned_block_dev(&g_uxfs_ns, task, block_index);
}

int uxfs_serve_mount(const char *path, struct a20_channel_ep *ep,
                     struct task_t *server, int block_index,
                     uint32_t serve_flags)
{
    return a20_uxfs_ns_serve_mount(&g_uxfs_ns, path, ep, server, block_index,
                                   serve_flags);
}

int uxfs_unmount(struct vnode *root)
{
    return a20_uxfs_ns_unmount(&g_uxfs_ns, root);
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

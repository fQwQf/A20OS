/*
 * ufsd — user-space filesystem host (docs/zh/hybrid-kernel/06-user-fs.md).
 *
 * Together with the kernel uxfs agent, this forms the complete "migrate the
 * filesystem implementation out of kernel mode" path. A single process
 * carries multiple filesystem backends:
 *
 *   fat      — fat32lite path model (same source reused from
 *              kernel/fs/diskfs/fat32lite.c)
 *   ext4     — kernel diskfs sources compiled as-is under the fscompat
 *              environment
 *   ntfs     — same (read-only semantics declared by the backend)
 *   iso9660  — same (read-only)
 *
 * Block IO uniformly enters the kernel block layer through the gated
 * fs_block_io syscall; the kernel only lets the service task that registered
 * this mount access its declared block device.
 *
 * Usage: ufsd <mount_path> <block_index> [fat|ext4|ntfs|iso9660]
 * Defaults to fstype=fat for compatibility with existing callers.
 */
#include <stdint.h>
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "abi/native/syscall_nr.h"
#include "core/types.h"
#include "core/stdio.h"
#include "ufs_backends.h"
#include "../svc/a20_services_idl.h"

typedef struct {
    uint32_t     size;
    uint32_t     version;
    uint32_t     server_channel;
    int32_t      block_index;
    uint64_t     target;
    uint32_t     target_len;
    uint32_t     flags;
} ufsd_serve_args_t;

typedef struct {
    uint32_t     size;
    uint32_t     version;
    int32_t      block_index;
    uint32_t     write;
    uint64_t     lba;
    uint32_t     count;
    uint32_t     _pad;
    uint64_t     buf;
} ufsd_block_io_args_t;

uint8_t ufs_tx[UFSD_MSG_MAX];
void (*ufs_log_sink)(const char *line);
extern void (*fscompat_log_sink)(const char *line);

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static a20_handle_t g_out;

static void log_str(const char *s)
{
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, s, a20_strlen(s), (void *)0);
}

static void log_dec(uint32_t v)
{
    char b[12];
    int n = 0;
    if (!v)
        b[n++] = '0';
    else {
        char t[12];
        int m = 0;
        while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
        while (m) b[n++] = t[--m];
    }
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, b, (uint64_t)n, (void *)0);
}

static void sink_line(const char *line)
{
    log_str("[fs]");
    log_str(line);
    log_str("\n");
}

/* ------------------------------------------------------------------ */
/* Controlled block IO: authorization for fs_block_io presupposes the fs_serve
 * registration record */
/* ------------------------------------------------------------------ */

static int32_t g_block_index = -1;

int fsio_read(uint32_t lba, void *buf, uint32_t count)
{
    if (g_block_index < 0 || count > 4096)
        return 1;
    ufsd_block_io_args_t a;
    a20_memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.version = 1;
    a.block_index = g_block_index;
    a.write = 0;
    a.lba = lba;
    a.count = count;
    a.buf = (uint64_t)(uintptr_t)buf;
    return a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                         (uint64_t)&a, 0, 0, 0, 0, 0)) ? 0 : 1;
}

int fsio_write(uint32_t lba, const void *buf, uint32_t count)
{
    if (g_block_index < 0 || count > 4096)
        return 1;
    ufsd_block_io_args_t a;
    a20_memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.version = 1;
    a.block_index = g_block_index;
    a.write = 1;
    a.lba = lba;
    a.count = count;
    a.buf = (uint64_t)(uintptr_t)buf;
    return a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                         (uint64_t)&a, 0, 0, 0, 0, 0)) ? 0 : 1;
}

uint64_t fsio_capacity_sectors(void)
{
    static uint64_t cached;
    if (cached)
        return cached;
    if (g_block_index < 0)
        return 0;
    ufsd_block_io_args_t a;
    a20_memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.version = 1;
    a.block_index = g_block_index;
    a.write = 0;
    a.count = 0; /* capacity query semantics: the sector count is returned
                 * via kargs.lba */
    a.buf = (uint64_t)(uintptr_t)&cached;
    if (!a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                       (uint64_t)&a, 0, 0, 0, 0, 0)))
        return 0;
    return cached;
}

/* Per-boot epoch source for the close-token registry (monotonic ns; 0 when
 * the clock is unavailable, which degrades token issue to the legacy path). */
uint64_t fsio_monotonic_ns(void)
{
    a20_time_ns_t ns = 0;
    if (!a20_status_is_ok(a20_clock_get(A20_CLOCK_MONOTONIC, &ns)))
        return 0;
    return ns;
}

/* UXFS flush capability uses a v2 request and a fixed-width magic response.
 * A v1 capacity query can return an arbitrary 64-bit sector count, so it must
 * never be interpreted as evidence that a durability callback exists. */
int fsio_flush_supported(void)
{
    const uint64_t magic = 0x5546585300000001ull;
    uint64_t response = 0;
    if (g_block_index < 0)
        return 0;

    ufsd_block_io_args_t a;
    a20_memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.version = 2;
    a.block_index = g_block_index;
    a.write = 2;
    a.buf = (uint64_t)(uintptr_t)&response;
    if (!a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                       (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0)))
        return 0;
    return response == magic;
}

int fsio_flush(void)
{
    if (g_block_index < 0)
        return -19; /* ENODEV */

    ufsd_block_io_args_t a;
    a20_memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.version = 1;
    a.block_index = g_block_index;
    a.write = 2;
    a20_status_t status = a20_syscall6(A20_SYS_fs_block_io,
                                       (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(status))
        return 0;
    switch (A20_ABS_ERROR(status)) {
    case A20_ERR_NOT_SUPPORTED: return -95; /* EOPNOTSUPP */
    case A20_ERR_INVALID_ARGUMENT: return -22; /* EINVAL */
    case A20_ERR_BAD_HANDLE: return -19; /* ENODEV */
    case A20_ERR_FAULT: return -14; /* EFAULT */
    default: return -5; /* EIO */
    }
}

/* ------------------------------------------------------------------ */

static const ufs_backend_t *pick_backend(const char *fstype)
{
    if (!fstype[0] || strcmp(fstype, "fat") == 0)
        return &UFS_BACKEND_FAT;
    if (strcmp(fstype, "ext4") == 0 || strcmp(fstype, "ntfs") == 0 ||
        strcmp(fstype, "iso9660") == 0)
        return &UFS_BACKEND_VNFS;
    return 0;
}

/* Set only when INIT negotiates the CLOSE_CONFIRM protocol; legacy token-only
 * requests fall back to ordinary v2 lifetime semantics without token issue. */
static int g_close_tokens_enabled;
static int g_close_confirm_enabled;

static void serve_init(ufs_resp_hdr_t *r, const ufs_backend_t *be,
                       const ufs_req_hdr_t *q)
{
    r->out0 = UFS_ROOT_INO;
    r->out1 = ufs_backend_lifetime_negotiate(be, q->arg0,
                                               &g_close_tokens_enabled,
                                               &g_close_confirm_enabled);
    r->out2 = UFS_SHUTDOWN_CAPABILITY;
}

static int reply(a20_handle_t ep, const ufs_resp_hdr_t *r)
{
    memcpy(ufs_tx, r, sizeof(*r));
    return a20_channel_send(ep, ufs_tx, sizeof(*r) + r->payload_len, 0, 0) >= 0
               ? 0 : -1;
}

static int32_t parse_int(const char *s)
{
    int32_t v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ */
/* Supervision channel: IDL echo probe on svcmgr's fixed slot endpoint */
/* ------------------------------------------------------------------ */

/* user_data tags for the EventQ watch: distinguish the fs channel from the
 * supervision slot as a wakeup source */
#define UFS_EV_FS  1u
#define UFS_EV_SVC 2u

/* Returns 1 = one supervision message was handled */
static int svc_ep_pump(a20_handle_t svc_ep)
{
    uint8_t buf[64];
    uint32_t blen = sizeof(buf);
    uint32_t hcnt = 0;
    a20_status_t st =
        a20_channel_recv_flags(svc_ep, buf, &blen, 0, &hcnt,
                               A20_MSG_NONBLOCK);
    if (st == -A20_ERR_WOULD_BLOCK)
        return 0;
    if (st < 0)
        return -1; /* slot not installed or supervisor gone: ignore
         * silently */
    if (blen < sizeof(a20_idl_envelope_t))
        return 1;
    a20_idl_envelope_t env;
    a20_memcpy(&env, buf, sizeof(env));
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return 1;
    if (env.type == SVCMGR_REQ_CRASH)
        a20_task_exit(A20_SVC_CRASH_CODE);
    /* SVCMGR_REQ_ECHO and other types: echo back verbatim (echod
     * contract) */
    if (a20_channel_send(svc_ep, buf, blen, 0, 0) < 0)
        return -1;
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *mount_path = (argc > 1) ? argv[1] : "/ufs";
    g_block_index = (argc > 2) ? parse_int(argv[2]) : 2;
    const char *fstype = (argc > 3) ? argv[3] : "fat";

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    ufs_log_sink = sink_line;

    const ufs_backend_t *be = pick_backend(fstype);
    if (!be) {
        log_str("UFSD: unknown fstype ");
        log_str(fstype);
        log_str("\n");
        a20_task_exit(3);
    }

    /*
     * Register first (the kernel records block device ownership and
     * completes the mount), then mount the concrete FS: authorization for
     * fs_block_io presupposes the registration record, so reversing the
     * order yields EPERM on the very first sector read.
     */
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) != A20_OK) {
        log_str("UFSD: channel create failed\n");
        a20_task_exit(1);
    }

    /* The mount point must already exist (VFS convention); ignore if it
     * does. */
    a20_path_create_args_t ca;
    a20_memset(&ca, 0, sizeof(ca));
    ca.size = sizeof(ca);
    ca.version = 1;
    ca.dir = A20_HANDLE_NULL;
    ca.type = 1; /* dir semantics of path_create */
    ca.mode = 0755;
    ca.path = (uint64_t)(uintptr_t)mount_path;
    ca.path_len = a20_strlen(mount_path);
    a20_syscall6(A20_SYS_path_create, (uint64_t)&ca, 0, 0, 0, 0, 0);

    ufsd_serve_args_t sa;
    a20_memset(&sa, 0, sizeof(sa));
    sa.size = sizeof(sa);
    sa.version = 1;
    sa.server_channel = pair.endpoints[1];
    sa.block_index = g_block_index;
    sa.target = (uint64_t)(uintptr_t)mount_path;
    sa.target_len = a20_strlen(mount_path);
    /* bit0 = read-only backend: the kernel page cache disables buffered
     * writes on this basis (iso9660 is physically read-only). */
    sa.flags = (strcmp(fstype, "iso9660") == 0) ? 1u : 0u;
    int64_t serve_st = a20_syscall6(A20_SYS_fs_serve, (uint64_t)&sa,
                                    0, 0, 0, 0, 0);
    if (!a20_status_is_ok(serve_st)) {
        log_str("UFSD: fs_serve failed st=-");
        log_dec((uint32_t)(-serve_st));
        log_str("\n");
        a20_task_exit(2);
    }

    /*
     * Missing-disk detection: a capacity query returning 0 means no block
     * device at that index (e.g. a scratch slot on a single-disk image).
     * Unmount the mount just registered and exit 0, so the supervisor sees a
     * clean completion rather than consuming the restart budget.
     */
    if (fsio_capacity_sectors() == 0) {
        a20_syscall6(A20_SYS_fs_umount, (uint64_t)(uintptr_t)mount_path,
                     a20_strlen(mount_path), 0, 0, 0, 0);
        log_str("UFSD: no block device at index ");
        log_dec((uint32_t)g_block_index);
        log_str("; exiting cleanly\n");
        a20_task_exit(0);
    }

    /* Capture filesystem mount diagnostics only during the mount itself.
     * The C service previously wired only its protocol logger, so messages
     * emitted through fscompat printf/klog vanished and a NULL root gave no
     * indication which parser/policy check rejected the image. */
    fscompat_log_sink = sink_line;
    int mount_status = be->mount(fstype);
    fscompat_log_sink = 0;
    if (mount_status != 0) {
        log_str("UFSD: mount ");
        log_str(fstype);
        log_str(" failed status=-");
        log_dec((uint32_t)(-mount_status));
        log_str("\n");
        a20_task_exit(1);
    }

    log_str("UFSD: serving ");
    log_str(mount_path);
    log_str(" as ");
    log_str(fstype);
    log_str("\n");

    /*
     * Unified wait across both channels: the fs channel and the supervision
     * slot are attached to the same EventQ, blocking in event_wait while idle
     * (the "supervision channel joins the unified EventQ wait" item in
     * docs/zh/roadmap). Both MESSAGE_READY and peer close wake the loop; the
     * drain-before-wait order guarantees that messages already published
     * before the watch was registered are still processed.
     */
    a20_handle_t svc_ep = ((a20_handle_t)A20_SVC_PING_SLOT);
    a20_handle_t eq;
    if (a20_event_queue_create(&eq) != A20_OK) {
        log_str("UFSD: eventq create failed\n");
        a20_task_exit(1);
    }
    if (a20_event_watch(eq, pair.endpoints[0],
                        A20_EVENT_MASK(A20_EVENT_MESSAGE_READY) |
                        A20_EVENT_MASK(A20_EVENT_PEER_CLOSED),
                        UFS_EV_FS) != A20_OK) {
        log_str("UFSD: watch fs ep failed\n");
        a20_task_exit(1);
    }
    if (a20_event_watch(eq, svc_ep,
                        A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                        UFS_EV_SVC) != A20_OK) {
        /* The supervision slot may not have been installed by this task
         * (startup modes with no ping channel): degrade to being woken only
         * by the fs channel; the supervision echo is handled incidentally on
         * the next fs traffic. */
        log_str("UFSD: watch svc ep failed; continuing without it\n");
    }

    static uint8_t rx[UFSD_MSG_MAX];
    static char name_buf[512];
    static char aux_buf[512];
    int backend_unmounted = 0;
    for (;;) {
        for (;;) {
            int pr = svc_ep_pump(svc_ep);
            if (pr <= 0)
                break;
        }

        uint32_t blen = sizeof(rx);
        uint32_t hcnt = 0;
        a20_status_t st =
            a20_channel_recv_flags(pair.endpoints[0], rx, &blen, 0, &hcnt,
                                   A20_MSG_NONBLOCK);
        if (st == -A20_ERR_WOULD_BLOCK) {
            a20_event_t ev;
            if (a20_event_wait(eq, (a20_time_t){ .secs = A20_TIMEOUT_INFINITE,
                                                 .nsecs = 0 },
                               &ev) < 0)
                continue;
            continue;
        }
        if (st < 0)
            break; /* peer closed: in-flight requests have ended -EIO,
         * waiting for a restart and re-mount */
        if (blen < sizeof(ufs_req_hdr_t))
            continue;
        const ufs_req_hdr_t *q = (const ufs_req_hdr_t *)rx;
        if (q->magic != UFS_REQ_MAGIC)
            continue;

        uint32_t nl = q->name_len;
        if (nl > sizeof(name_buf) - 1)
            nl = sizeof(name_buf) - 1;
        memcpy(name_buf, rx + sizeof(*q), nl);
        name_buf[nl] = '\0';
        const char *name = name_buf;

        uint32_t pl_off = sizeof(*q) + q->name_len;
        uint32_t pl_nul = q->payload_len < sizeof(aux_buf) - 1
                              ? q->payload_len
                              : sizeof(aux_buf) - 1;
        memcpy(aux_buf, rx + pl_off, pl_nul);
        aux_buf[pl_nul] = '\0';

        ufs_resp_hdr_t r;
        a20_memset(&r, 0, sizeof(r));
        r.magic = UFS_RESP_MAGIC;
        r.opcode = q->opcode;
        r.req_id = q->req_id;
        int64_t rc = 0;

        switch (q->opcode) {
        case UFS_OP_INIT: serve_init(&r, be, q); break;
        case UFS_OP_SHUTDOWN:
            if (q->version != UFS_PROTO_VERSION || q->ino != UFS_ROOT_INO ||
                q->name_len != 0 || q->payload_len != 0) {
                rc = -22;
            } else {
                rc = be->unmount ? be->unmount() : 0;
                if (rc == 0)
                    backend_unmounted = 1;
                else if (rc == -16)
                    r.out1 = UFS_SHUTDOWN_RETRY_SAFE;
            }
            break;
        case UFS_OP_LOOKUP:
            rc = be->lookup(q->ino, name, q->name_len, &r);
            break;
        case UFS_OP_GETATTR: rc = be->getattr(q->ino, &r); break;
        case UFS_OP_READDIR: rc = be->readdir(q->ino, q->arg0, &r); break;
        case UFS_OP_CREATE:
            rc = be->create(q->ino, name, (uint32_t)q->arg0, &r);
            break;
        case UFS_OP_MKDIR: rc = be->mkdir(q->ino, name, &r); break;
        case UFS_OP_UNLINK: rc = be->unlink(q->ino, name); break;
        case UFS_OP_RMDIR: rc = be->rmdir(q->ino, name); break;
        case UFS_OP_TRUNCATE: rc = be->truncate(q->ino, q->arg0); break;
        case UFS_OP_READ:
            rc = be->read(q->ino, q->arg0, (uint32_t)q->arg1, &r);
            break;
        case UFS_OP_WRITE:
            rc = be->write(q->ino, q->arg0,
                           rx + sizeof(*q) + q->name_len, q->payload_len, &r);
            break;
        case UFS_OP_RENAME:
            rc = be->rename(q->ino, name, q->arg0, aux_buf, &r);
            break;
        case UFS_OP_SYNC: rc = be->sync(); break;
        case UFS_OP_OPEN:
            if (q->version == UFS_PROTO_VERSION && be->open) {
                uint64_t token = 0;
                rc = be->open(q->ino,
                              g_close_tokens_enabled ? &token : 0);
                if (rc == 0)
                    r.out2 = token; /* 0 = legacy open, no token */
            } else {
                rc = -U_ENOSYS;
            }
            break;
        case UFS_OP_CLOSE:
            rc = (q->version == UFS_PROTO_VERSION && be->close)
                     ? ((g_close_tokens_enabled && q->arg0 == 0)
                         ? -U_ENOENT : be->close(q->ino, q->arg0))
                     : -U_ENOSYS;
            break;
        case UFS_OP_CLOSE_CONFIRM:
            rc = (q->version == UFS_PROTO_VERSION &&
                  g_close_confirm_enabled && be->close_confirm)
                     ? be->close_confirm(q->ino, q->arg0) : -U_ENOSYS;
            break;
        case UFS_OP_STATFS: be->statfs(&r); break;
        default:
            r.status = -38 /* ENOSYS */;
            break;
        }
        r.status = (int32_t)rc;

        if (reply(pair.endpoints[0], &r) != 0)
            break;
        if (q->opcode == UFS_OP_SHUTDOWN && rc == 0)
            break;
    }
    int unmount_rc = backend_unmounted ? 0 :
                     (be->unmount ? (int)be->unmount() : 0);
    a20_task_exit(unmount_rc < 0 ? 1 : 0);
}

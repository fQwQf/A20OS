/*
 * ufsd — 用户态文件系统宿主（docs/hybrid-kernel/06-user-fs.md）。
 *
 * 与内核 uxfs 代理共同构成"文件系统实现迁出内核态"的完整路径。单进程
 * 承载多种文件系统后端：
 *
 *   fat      — fat32lite 路径模型（同源复用 kernel/fs/diskfs/fat32lite.c）
 *   ext4     — 内核 diskfs 源码经 fscompat 兼容环境原样编译
 *   ntfs     — 同上（只读语义由后端声明）
 *   iso9660  — 同上（只读）
 *
 * 块 IO 统一经受控的 fs_block_io syscall 进入内核块层；内核校验只有
 * 注册该挂载的服务任务可以访问其声明的块设备。
 *
 * 用法：ufsd <mount_path> <block_index> [fat|ext4|ntfs|iso9660]
 * 默认 fstype=fat 以兼容既有调用方。
 */
#include <stdint.h>
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "abi/native/syscall_nr.h"
#include "core/types.h"
#include "core/stdio.h"
#include "ufs_backends.h"
#include "../svc/a20_services_idl.h"
#include "../svc/svc_proto.h"

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

/* ------------------------------------------------------------------ */
/* 日志                                                                 */
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
/* 受控块 IO：fs_block_io 的授权以 fs_serve 注册记录为前提               */
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
    a.count = 0; /* 容量查询语义：经 kargs.lba 回传扇区数 */
    a.buf = (uint64_t)(uintptr_t)&cached;
    if (!a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                       (uint64_t)&a, 0, 0, 0, 0, 0)))
        return 0;
    return cached;
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

static void serve_init(ufs_resp_hdr_t *r)
{
    r->out0 = UFS_ROOT_INO;
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
/* 监管通道：svcmgr 固定槽位端点上的 IDL echo 探针                      */
/* ------------------------------------------------------------------ */

/* EventQ watch 的 user_data 标记：区分 fs 通道与监管槽位的唤醒源 */
#define UFS_EV_FS  1u
#define UFS_EV_SVC 2u

/* 返回 1 = 处理了一条监管消息 */
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
        return -1; /* 槽位未安装或监管者消失：静默忽略 */
    if (blen < sizeof(a20_idl_envelope_t))
        return 1;
    a20_idl_envelope_t env;
    a20_memcpy(&env, buf, sizeof(env));
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return 1;
    if (env.type == SVCMGR_REQ_CRASH)
        a20_task_exit(A20_SVC_CRASH_CODE);
    /* SVCMGR_REQ_ECHO 与其他类型：原样回显（echod 契约） */
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
     * 先注册（内核记录块设备所有权并完成挂载），再挂具体 FS：
     * fs_block_io 的授权以注册记录为前提，顺序颠倒会在首个扇区读上
     * 得到 EPERM。
     */
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) != A20_OK) {
        log_str("UFSD: channel create failed\n");
        a20_task_exit(1);
    }

    /* 挂载点须预先存在（VFS 惯例）；已存在则忽略。 */
    a20_path_create_args_t ca;
    a20_memset(&ca, 0, sizeof(ca));
    ca.size = sizeof(ca);
    ca.version = 1;
    ca.dir = A20_HANDLE_NULL;
    ca.type = 1; /* path_create 的 dir 语义 */
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
    int64_t serve_st = a20_syscall6(A20_SYS_fs_serve, (uint64_t)&sa,
                                    0, 0, 0, 0, 0);
    if (!a20_status_is_ok(serve_st)) {
        log_str("UFSD: fs_serve failed st=-");
        log_dec((uint32_t)(-serve_st));
        log_str("\n");
        a20_task_exit(2);
    }

    /*
     * 缺盘检测：容量查询返回 0 说明该序号没有块设备（如单盘镜像上的
     * scratch 槽位）。卸载刚注册的挂载并以 0 退出，让监管者视为干净
     * 完成而不是触发重启预算。
     */
    if (fsio_capacity_sectors() == 0) {
        a20_syscall6(A20_SYS_fs_umount, (uint64_t)(uintptr_t)mount_path,
                     a20_strlen(mount_path), 0, 0, 0, 0);
        log_str("UFSD: no block device at index ");
        log_dec((uint32_t)g_block_index);
        log_str("; exiting cleanly\n");
        a20_task_exit(0);
    }

    if (be->mount(fstype) != 0) {
        log_str("UFSD: mount ");
        log_str(fstype);
        log_str(" failed\n");
        a20_task_exit(1);
    }

    log_str("UFSD: serving ");
    log_str(mount_path);
    log_str(" as ");
    log_str(fstype);
    log_str("\n");

    /*
     * 双通道统一等待：fs 通道与监管槽位挂到同一个 EventQ，空闲时阻塞
     * 在 event_wait 上（docs/roadmap 的"监管通道接入 EventQ 统一等待"
     * 项）。MESSAGE_READY 与对端关闭都会唤醒循环；先排空再等待的顺序
     * 保证 watch 注册前已发布的消息同样被处理。
     */
    a20_handle_t svc_ep = A20_SVC_PING_HANDLE;
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
        /* 监管槽位可能未被本任务安装（无 ping 通道的启动方式）：
         * 降级为只由 fs 通道唤醒，监管 echo 由下次 fs 流量顺带处理。 */
        log_str("UFSD: watch svc ep failed; continuing without it\n");
    }

    static uint8_t rx[UFSD_MSG_MAX];
    static char name_buf[512];
    static char aux_buf[512];
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
            break; /* 对端关闭：在飞请求已按 -EIO 收场，等待重启重挂载 */
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
        case UFS_OP_INIT: serve_init(&r); break;
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
        case UFS_OP_STATFS: be->statfs(&r); break;
        default:
            r.status = -38 /* ENOSYS */;
            break;
        }
        r.status = (int32_t)rc;

        if (reply(pair.endpoints[0], &r) != 0)
            break;
    }
    a20_task_exit(0);
}

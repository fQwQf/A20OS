/*
 * ufsd — 用户态 FAT32 文件系统服务（docs/hybrid-kernel/06-user-fs.md）。
 *
 * 与内核 uxfs 代理共同构成"文件系统实现迁出内核态"的完整路径：内核只保留
 * VFS 核心与块层；本进程持有全部 FAT32 逻辑（同源复用 kernel/fs/diskfs/
 * fat32lite.c，freestanding、fat32lite_io_t 回调注入），经 channel 消息
 * 服务 vnode 操作，块 IO 走受控的 fs_block_io syscall 进入内核块层。
 *
 * 用法：ufsd [mount_path] [block_index]   （默认 "/ufs" 2）
 * 进程崩溃后由监管者重启；重启后挂载点需重新 fs_serve 注册。
 */
#include <stdint.h>
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "abi/native/syscall_nr.h"
#include "core/types.h"
#include "fs/fat32lite.h"
#include "fs/ufs_proto.h"

/* ---- 与内核 VFS 一致的 errno 值（freestanding 环境自持） ---- */
#define U_EPERM   1
#define U_ENOENT  2
#define U_EIO     5
#define U_EEXIST  17
#define U_ENODEV  19
#define U_ENOTDIR 20
#define U_EISDIR  21
#define U_EINVAL  22
#define U_EMFILE  24
#define U_ENOSPC  28
#define U_EROFS   30
#define U_ENOSYS  38

/* ---- fs_serve / fs_block_io 线格式（与 kernel/include/abi/native/types.h
 *      保持一致；用户侧不包含该头，因其引用内核内部 ipc 头） ---- */

/*
 * 与 kernel/include/abi/native/types.h 的 a20_fs_serve_args_t /
 * a20_fs_block_io_args_t 逐字段一致（a20_handle_t 为 32 位句柄索引，
 * 用户侧不包含该头，因其引用内核内部 ipc 头）。
 */
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

#define UFSD_MSG_MAX 65536u

/* ---- ino ↔ path 映射：fat32lite 以路径为键，协议以 ino 为句柄 ---- */

#define UFS_MAX_NODES 256

typedef struct {
    char     path[FAT32LITE_PATH_MAX];
    uint64_t ino;
    uint8_t  dead;
} ufs_node_t;

static ufs_node_t      g_nodes[UFS_MAX_NODES];
static uint64_t        g_next_ino = UFS_ROOT_INO + 1;
static fat32lite_fs_t  g_fs;
static int32_t         g_block_index = -1;

/* ---- 日志：继承的 stdout 句柄，缺失时静默 ---- */

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

/* ---- 块 IO 后端：fs_block_io 受控通道（内核校验服务任务所有权） ---- */

static int io_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
    (void)ctx;
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

static int io_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
    (void)ctx;
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

static const fat32lite_io_t g_io = {
    .ctx = 0,
    .read = io_read,
    .write = io_write,
};

/* ---- 节点表 ---- */

static ufs_node_t *node_by_ino(uint64_t ino)
{
    for (int i = 0; i < UFS_MAX_NODES; i++) {
        if (g_nodes[i].ino == ino && !g_nodes[i].dead && g_nodes[i].path[0])
            return &g_nodes[i];
    }
    return 0;
}

static ufs_node_t *node_by_path(const char *path)
{
    for (int i = 0; i < UFS_MAX_NODES; i++) {
        if (!g_nodes[i].dead && g_nodes[i].path[0] &&
            a20_strncmp(g_nodes[i].path, path, FAT32LITE_PATH_MAX) == 0)
            return &g_nodes[i];
    }
    return 0;
}

static ufs_node_t *node_alloc(const char *path)
{
    for (int i = 0; i < UFS_MAX_NODES; i++) {
        if (!g_nodes[i].path[0]) {
            a20_strncpy(g_nodes[i].path, path, FAT32LITE_PATH_MAX - 1);
            g_nodes[i].path[FAT32LITE_PATH_MAX - 1] = '\0';
            g_nodes[i].ino = g_next_ino++;
            g_nodes[i].dead = 0;
            return &g_nodes[i];
        }
    }
    return 0;
}

static ufs_node_t *node_get(const char *path)
{
    ufs_node_t *n = node_by_path(path);
    return n ? n : node_alloc(path);
}

static void path_join(char *dst, const ufs_node_t *dir, const char *name,
                      uint32_t name_len)
{
    uint32_t dlen = a20_strlen(dir->path);
    if (dlen && dir->path[dlen - 1] == '/')
        dlen--;
    a20_memcpy(dst, dir->path, dlen);
    dst[dlen] = '/';
    uint32_t nl = name_len;
    if (nl > 32)
        nl = 32; /* 8.3 名上限远小于此 */
    a20_memcpy(dst + dlen + 1, name, nl);
    dst[dlen + 1 + nl] = '\0';
}

static uint32_t mode_for(int is_dir)
{
    /* S_IFDIR|0755 / S_IFREG|0755，与内核 vfs.h 位型一致 */
    return is_dir ? (0040000u | 0755u) : (0100000u | 0755u);
}

/* ---- 协议处理。载荷直接构造在应答消息头部之后（连续布局）。 ---- */

static uint8_t tx[UFSD_MSG_MAX];

static int reply(a20_handle_t ep, const ufs_resp_hdr_t *r)
{
    a20_memcpy(tx, r, sizeof(*r));
    return a20_channel_send(ep, tx, sizeof(*r) + r->payload_len, 0, 0) >= 0
               ? 0 : -1;
}

static void put_fail(ufs_resp_hdr_t *r, int err)
{
    r->status = err;
}

static void serve_init(ufs_resp_hdr_t *r)
{
    r->out0 = UFS_ROOT_INO;
}

static void serve_lookup(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r,
                         const char *name)
{
    ufs_node_t *dir = node_by_ino(q->ino);
    if (!dir) { put_fail(r, -U_ENOENT); return; }

    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, q->name_len);

    int is_dir = 0;
    uint32_t size = 0;
    int st = fat32lite_stat(&g_fs, child, &is_dir, &size);
    if (st != FAT32LITE_OK) {
        put_fail(r, st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_ENOTDIR ? -U_ENOTDIR : -U_EIO);
        return;
    }
    ufs_node_t *n = node_get(child);
    if (!n) { put_fail(r, -U_EMFILE); return; }

    r->out0 = n->ino;
    r->out1 = is_dir ? UFS_FT_DIR : UFS_FT_FILE;
    r->out2 = ((uint64_t)mode_for(is_dir) << 32) | size;
}

static void serve_getattr(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r)
{
    ufs_node_t *n = node_by_ino(q->ino);
    if (!n) { put_fail(r, -U_ENOENT); return; }
    int is_dir = 0;
    uint32_t size = 0;
    int st = fat32lite_stat(&g_fs, n->path, &is_dir, &size);
    if (st != FAT32LITE_OK) {
        put_fail(r, st == FAT32LITE_ENOENT ? -U_ENOENT : -U_EIO);
        return;
    }
    r->out0 = size;
    r->out1 = (uint64_t)mode_for(is_dir) << 32;
}

static void serve_readdir(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r)
{
    ufs_node_t *dir = node_by_ino(q->ino);
    if (!dir) { put_fail(r, -U_ENOENT); return; }

    fat32lite_dir_t d;
    if (fat32lite_opendir(&g_fs, dir->path, &d) != FAT32LITE_OK) {
        put_fail(r, -U_ENOTDIR);
        return;
    }

    uint8_t *out = tx + sizeof(ufs_resp_hdr_t);
    uint32_t cap = UFSD_MSG_MAX - sizeof(ufs_resp_hdr_t);
    uint32_t used = 0;
    uint64_t skip = q->arg0;

    fat32lite_dirent_t de;
    while (used + 10 + 32 <= cap) {
        int st = fat32lite_readdir(&d, &de);
        if (st == FAT32LITE_ENOENT)
            break;
        if (st != FAT32LITE_OK) { put_fail(r, -U_EIO); return; }
        if (skip) { skip--; continue; }

        char child[FAT32LITE_PATH_MAX];
        path_join(child, dir, de.name, a20_strlen(de.name));
        ufs_node_t *n = node_get(child);
        if (!n) { put_fail(r, -U_EMFILE); return; }

        uint8_t nlen = (uint8_t)a20_strlen(de.name);
        uint64_t ino = n->ino;
        a20_memcpy(out + used, &ino, 8); used += 8;
        out[used++] = de.is_dir ? UFS_FT_DIR : UFS_FT_FILE;
        out[used++] = nlen;
        a20_memcpy(out + used, de.name, nlen); used += nlen;
    }

    r->payload_len = used;
}

static void serve_create(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r,
                         const char *name)
{
    ufs_node_t *dir = node_by_ino(q->ino);
    if (!dir) { put_fail(r, -U_ENOENT); return; }
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, q->name_len);

    fat32lite_file_t f;
    int st = fat32lite_create(&g_fs, child, &f);
    if (st != FAT32LITE_OK) {
        put_fail(r, st == FAT32LITE_ENOSPC ? -U_ENOSPC
                    : st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_EEXIST ? -U_EEXIST
                    : st == FAT32LITE_EROFS ? -U_EROFS : -U_EINVAL);
        return;
    }
    fat32lite_close(&f);
    ufs_node_t *n = node_get(child);
    if (!n) { put_fail(r, -U_EMFILE); return; }
    r->out0 = n->ino;
}

static void serve_mkdir(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r,
                        const char *name)
{
    ufs_node_t *dir = node_by_ino(q->ino);
    if (!dir) { put_fail(r, -U_ENOENT); return; }
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, q->name_len);
    int st = fat32lite_mkdir(&g_fs, child);
    if (st != FAT32LITE_OK) {
        put_fail(r, st == FAT32LITE_ENOSPC ? -U_ENOSPC
                    : st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_EROFS ? -U_EROFS : -U_EINVAL);
        return;
    }
    ufs_node_t *n = node_get(child);
    if (!n) { put_fail(r, -U_EMFILE); return; }
    r->out0 = n->ino;
}

static void serve_unlink(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r,
                         const char *name)
{
    ufs_node_t *dir = node_by_ino(q->ino);
    if (!dir) { put_fail(r, -U_ENOENT); return; }
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, q->name_len);
    int st = fat32lite_unlink(&g_fs, child);
    if (st != FAT32LITE_OK) {
        put_fail(r, st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_EROFS ? -U_EROFS : -U_EINVAL);
        return;
    }
    ufs_node_t *n = node_by_path(child);
    if (n)
        n->dead = 1;
}

static void serve_rmdir(ufs_resp_hdr_t *r)
{
    put_fail(r, -U_ENOSYS); /* fat32lite 未提供目录删除；v1 边界 */
}

static void serve_truncate(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r)
{
    ufs_node_t *n = node_by_ino(q->ino);
    if (!n) { put_fail(r, -U_ENOENT); return; }
    if (q->arg0 != 0) { put_fail(r, -U_ENOSYS); return; } /* 仅支持截断为空 */

    fat32lite_file_t f;
    int st = fat32lite_create(&g_fs, n->path, &f);
    if (st != FAT32LITE_OK) { put_fail(r, -U_EIO); return; }
    fat32lite_close(&f);
}

static void serve_read(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r)
{
    ufs_node_t *n = node_by_ino(q->ino);
    if (!n) { put_fail(r, -U_ENOENT); return; }
    uint32_t count = (uint32_t)q->arg1;
    if (count > UFSD_MSG_MAX - sizeof(ufs_resp_hdr_t))
        count = UFSD_MSG_MAX - sizeof(ufs_resp_hdr_t);

    fat32lite_file_t f;
    int st = fat32lite_open(&g_fs, n->path, &f);
    if (st != FAT32LITE_OK) {
        put_fail(r, st == FAT32LITE_ENOENT ? -U_ENOENT : -U_EIO);
        return;
    }
    fat32lite_seek(&f, (uint32_t)q->arg0);
    int rd = fat32lite_read(&f, tx + sizeof(ufs_resp_hdr_t), count);
    fat32lite_close(&f);
    if (rd < 0) { put_fail(r, -U_EIO); return; }
    r->out0 = (uint64_t)rd;
    r->payload_len = (uint32_t)rd;
}

static void serve_write(const ufs_req_hdr_t *q, ufs_resp_hdr_t *r,
                        const uint8_t *req_bytes)
{
    ufs_node_t *n = node_by_ino(q->ino);
    if (!n) { put_fail(r, -U_ENOENT); return; }
    const uint8_t *payload = req_bytes + sizeof(*q) + q->name_len;
    uint32_t count = q->payload_len;
    uint64_t off = q->arg0;

    fat32lite_file_t f;
    int st = fat32lite_open(&g_fs, n->path, &f);
    if (st == FAT32LITE_ENOENT)
        st = fat32lite_create(&g_fs, n->path, &f);
    if (st != FAT32LITE_OK) { put_fail(r, -U_EIO); return; }
    /* 同 fat32lite_append 的约定：既有文件以可写语义续用 */
    f.writable = 1;
    if (off > f.size) { fat32lite_close(&f); put_fail(r, -U_EINVAL); return; }

    fat32lite_seek(&f, (uint32_t)off);
    int wr = fat32lite_write(&f, payload, count);
    fat32lite_close(&f);
    if (wr < 0) { put_fail(r, -U_EIO); return; }
    r->out0 = (uint64_t)wr;
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

/* ---- 主循环 ---- */

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *mount_path = (argc > 1) ? argv[1] : "/ufs";
    g_block_index = (argc > 2) ? parse_int(argv[2]) : 2;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;

    /*
     * 先注册（内核记录块设备所有权并完成挂载），再挂 FAT：fs_block_io
     * 的授权以注册记录为前提，顺序颠倒会在首个扇区读上得到 EPERM。
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

    if (fat32lite_mount(&g_fs, &g_io, 0) != FAT32LITE_OK) {
        log_str("UFSD: FAT mount failed\n");
        a20_task_exit(1);
    }

    a20_strncpy(g_nodes[0].path, "/", FAT32LITE_PATH_MAX);
    g_nodes[0].ino = UFS_ROOT_INO;
    g_nodes[0].dead = 0;

    log_str("UFSD: serving ");
    log_str(mount_path);
    log_str("\n");

    static uint8_t rx[UFSD_MSG_MAX];
    for (;;) {
        uint32_t blen = sizeof(rx);
        uint32_t hcnt = 0;
        a20_status_t st = a20_channel_recv(pair.endpoints[0], rx, &blen, 0,
                                           &hcnt);
        if (st < 0)
            break; /* 对端关闭：在飞请求已按 -EIO 收场，等待重启重挂载 */
        if (blen < sizeof(ufs_req_hdr_t))
            continue;
        const ufs_req_hdr_t *q = (const ufs_req_hdr_t *)rx;
        if (q->magic != UFS_REQ_MAGIC)
            continue;

        ufs_resp_hdr_t r;
        a20_memset(&r, 0, sizeof(r));
        r.magic = UFS_RESP_MAGIC;
        r.opcode = q->opcode;
        r.req_id = q->req_id;

        const char *name = (const char *)(rx + sizeof(*q));
        switch (q->opcode) {
        case UFS_OP_INIT: serve_init(&r); break;
        case UFS_OP_LOOKUP: serve_lookup(q, &r, name); break;
        case UFS_OP_GETATTR: serve_getattr(q, &r); break;
        case UFS_OP_READDIR: serve_readdir(q, &r); break;
        case UFS_OP_CREATE: serve_create(q, &r, name); break;
        case UFS_OP_MKDIR: serve_mkdir(q, &r, name); break;
        case UFS_OP_UNLINK: serve_unlink(q, &r, name); break;
        case UFS_OP_RMDIR: serve_rmdir(&r); break;
        case UFS_OP_TRUNCATE: serve_truncate(q, &r); break;
        case UFS_OP_READ: serve_read(q, &r); break;
        case UFS_OP_WRITE: serve_write(q, &r, rx); break;
        case UFS_OP_SYNC:
        case UFS_OP_STATFS: break; /* 直写后端无需冲刷；statfs 零值即可 */
        default: put_fail(&r, -U_ENOSYS); break;
        }

        if (reply(pair.endpoints[0], &r) != 0)
            break;
    }
    a20_task_exit(0);
}

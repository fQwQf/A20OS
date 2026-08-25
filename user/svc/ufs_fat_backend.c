/*
 * ufs_fat_backend.c — ufsd 的 FAT32 后端（fat32lite 路径模型）。
 *
 * fat32lite 以路径为键、协议以 ino 为句柄，本文件维护 ino↔path 映射表。
 * 块 IO 经 io_read/io_write（fs_block_io 受控通道）注入 fat32lite_io_t。
 */
#include <stdint.h>
#include "ufs_backends.h"
#include "core/types.h"
#include "core/stdio.h"
#include "fs/fat32lite.h"

#define UFS_MAX_NODES 256

typedef struct {
    char     path[FAT32LITE_PATH_MAX];
    uint64_t ino;
    uint8_t  dead;
} fat_node_t;

static fat_node_t     g_nodes[UFS_MAX_NODES];
static uint64_t       g_next_ino = UFS_ROOT_INO + 1;
static fat32lite_fs_t g_fs;

extern int  fsio_read(uint32_t lba, void *buf, uint32_t count);
extern int  fsio_write(uint32_t lba, const void *buf, uint32_t count);

static int fat_io_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
    (void)ctx;
    return fsio_read(lba, buf, count);
}

static int fat_io_write(void *ctx, uint32_t lba, const void *buf,
                        uint32_t count)
{
    (void)ctx;
    return fsio_write(lba, buf, count);
}

static const fat32lite_io_t g_io = {
    .ctx = 0,
    .read = fat_io_read,
    .write = fat_io_write,
};

static fat_node_t *node_by_ino(uint64_t ino)
{
    for (int i = 0; i < UFS_MAX_NODES; i++) {
        if (g_nodes[i].ino == ino && !g_nodes[i].dead && g_nodes[i].path[0])
            return &g_nodes[i];
    }
    return 0;
}

static fat_node_t *node_by_path(const char *path)
{
    for (int i = 0; i < UFS_MAX_NODES; i++) {
        if (!g_nodes[i].dead && g_nodes[i].path[0] &&
            a20_strncmp(g_nodes[i].path, path, FAT32LITE_PATH_MAX) == 0)
            return &g_nodes[i];
    }
    return 0;
}

static fat_node_t *node_alloc(const char *path)
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

static fat_node_t *node_get(const char *path)
{
    fat_node_t *n = node_by_path(path);
    return n ? n : node_alloc(path);
}

static void path_join(char *dst, const fat_node_t *dir, const char *name,
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

static int fat_mount(const char *fstype)
{
    (void)fstype;
    if (fat32lite_mount(&g_fs, &g_io, 0) != FAT32LITE_OK)
        return -U_EIO;
    a20_memset(g_nodes, 0, sizeof(g_nodes));
    a20_strncpy(g_nodes[0].path, "/", FAT32LITE_PATH_MAX);
    g_nodes[0].ino = UFS_ROOT_INO;
    g_next_ino = UFS_ROOT_INO + 1;
    return 0;
}

static void put_fail(ufs_resp_hdr_t *r, int err)
{
    r->status = err;
}

static int64_t fat_lookup(uint64_t dir_ino, const char *name,
                          uint32_t name_len, ufs_resp_hdr_t *r)
{
    fat_node_t *dir = node_by_ino(dir_ino);
    if (!dir)
        return -U_ENOENT;
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, name_len);

    int is_dir = 0;
    uint32_t size = 0;
    int st = fat32lite_stat(&g_fs, child, &is_dir, &size);
    if (st != FAT32LITE_OK) {
        r->status = st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_ENOTDIR ? -U_ENOTDIR : -U_EIO;
        return r->status;
    }
    fat_node_t *n = node_get(child);
    if (!n) {
        r->status = -U_EMFILE;
        return r->status;
    }
    r->out0 = n->ino;
    r->out1 = is_dir ? UFS_FT_DIR : UFS_FT_FILE;
    r->out2 = ((uint64_t)mode_for(is_dir) << 32) | size;
    return 0;
}

static int64_t fat_getattr(uint64_t ino, ufs_resp_hdr_t *r)
{
    fat_node_t *n = node_by_ino(ino);
    if (!n)
        return -U_ENOENT;
    /* 根目录不走 fat32lite 路径解析（其对 "/" 返回 EINVAL） */
    if (n->ino == UFS_ROOT_INO) {
        r->out0 = 0;
        r->out1 = (uint64_t)mode_for(1) << 32;
        return 0;
    }
    int is_dir = 0;
    uint32_t size = 0;
    int st = fat32lite_stat(&g_fs, n->path, &is_dir, &size);
    if (st != FAT32LITE_OK) {
        r->status = st == FAT32LITE_ENOENT ? -U_ENOENT : -U_EIO;
        return r->status;
    }
    r->out0 = size;
    r->out1 = (uint64_t)mode_for(is_dir) << 32;
    return 0;
}

static int64_t fat_readdir(uint64_t dir_ino, uint64_t skip,
                           ufs_resp_hdr_t *r)
{
    fat_node_t *dir = node_by_ino(dir_ino);
    if (!dir) {
        r->status = -U_ENOENT;
        return r->status;
    }
    fat32lite_dir_t d;
    if (fat32lite_opendir(&g_fs, dir->path, &d) != FAT32LITE_OK) {
        r->status = -U_ENOTDIR;
        return r->status;
    }

    uint8_t *out = ufs_payload_buf();
    uint32_t cap = ufs_payload_cap();
    uint32_t used = 0;

    fat32lite_dirent_t de;
    while (used + 10 + 32 <= cap) {
        int st = fat32lite_readdir(&d, &de);
        if (st == FAT32LITE_ENOENT)
            break;
        if (st != FAT32LITE_OK) {
            r->status = -U_EIO;
            return r->status;
        }
        if (skip) { skip--; continue; }

        char child[FAT32LITE_PATH_MAX];
        path_join(child, dir, de.name, a20_strlen(de.name));
        fat_node_t *n = node_get(child);
        if (!n) {
            r->status = -U_EMFILE;
            return r->status;
        }

        uint8_t nlen = (uint8_t)a20_strlen(de.name);
        uint64_t ino = n->ino;
        a20_memcpy(out + used, &ino, 8); used += 8;
        out[used++] = de.is_dir ? UFS_FT_DIR : UFS_FT_FILE;
        out[used++] = nlen;
        a20_memcpy(out + used, de.name, nlen); used += nlen;
    }
    r->payload_len = used;
    return 0;
}

static int64_t fat_create(uint64_t dir_ino, const char *name, uint32_t mode,
                          ufs_resp_hdr_t *r)
{
    (void)mode;
    fat_node_t *dir = node_by_ino(dir_ino);
    if (!dir) {
        r->status = -U_ENOENT;
        return r->status;
    }
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, a20_strlen(name));

    fat32lite_file_t f;
    int st = fat32lite_create(&g_fs, child, &f);
    if (st != FAT32LITE_OK) {
        r->status = st == FAT32LITE_ENOSPC ? -U_ENOSPC
                    : st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_EEXIST ? -U_EEXIST
                    : st == FAT32LITE_EROFS ? -U_EROFS : -U_EINVAL;
        return r->status;
    }
    fat32lite_close(&f);
    fat_node_t *n = node_get(child);
    if (!n) {
        r->status = -U_EMFILE;
        return r->status;
    }
    r->out0 = n->ino;
    return 0;
}

static int64_t fat_mkdir(uint64_t dir_ino, const char *name,
                         ufs_resp_hdr_t *r)
{
    fat_node_t *dir = node_by_ino(dir_ino);
    if (!dir) {
        r->status = -U_ENOENT;
        return r->status;
    }
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, a20_strlen(name));
    int st = fat32lite_mkdir(&g_fs, child);
    if (st != FAT32LITE_OK) {
        r->status = st == FAT32LITE_ENOSPC ? -U_ENOSPC
                    : st == FAT32LITE_ENOENT ? -U_ENOENT
                    : st == FAT32LITE_EROFS ? -U_EROFS : -U_EINVAL;
        return r->status;
    }
    fat_node_t *n = node_get(child);
    if (!n) {
        r->status = -U_EMFILE;
        return r->status;
    }
    r->out0 = n->ino;
    return 0;
}

static int64_t fat_unlink(uint64_t dir_ino, const char *name)
{
    fat_node_t *dir = node_by_ino(dir_ino);
    if (!dir)
        return -U_ENOENT;
    char child[FAT32LITE_PATH_MAX];
    path_join(child, dir, name, a20_strlen(name));
    int st = fat32lite_unlink(&g_fs, child);
    if (st != FAT32LITE_OK) {
        return st == FAT32LITE_ENOENT ? -U_ENOENT
               : st == FAT32LITE_EROFS ? -U_EROFS : -U_EINVAL;
    }
    fat_node_t *n = node_by_path(child);
    if (n)
        n->dead = 1;
    return 0;
}

static int64_t fat_rmdir(uint64_t dir_ino, const char *name)
{
    (void)dir_ino;
    (void)name;
    return -U_ENOSYS; /* fat32lite 未提供目录删除；v1 边界 */
}

static int64_t fat_truncate(uint64_t ino, uint64_t size)
{
    fat_node_t *n = node_by_ino(ino);
    if (!n)
        return -U_ENOENT;
    if (size != 0)
        return -U_ENOSYS; /* 仅支持截断为空 */

    fat32lite_file_t f;
    if (fat32lite_create(&g_fs, n->path, &f) != FAT32LITE_OK)
        return -U_EIO;
    fat32lite_close(&f);
    return 0;
}

static int64_t fat_read(uint64_t ino, uint64_t off, uint32_t count,
                        ufs_resp_hdr_t *r)
{
    fat_node_t *n = node_by_ino(ino);
    if (!n) {
        r->status = -U_ENOENT;
        return r->status;
    }
    uint32_t cap = ufs_payload_cap();
    if (count > cap)
        count = cap;

    fat32lite_file_t f;
    int st = fat32lite_open(&g_fs, n->path, &f);
    if (st != FAT32LITE_OK) {
        r->status = st == FAT32LITE_ENOENT ? -U_ENOENT : -U_EIO;
        return r->status;
    }
    fat32lite_seek(&f, (uint32_t)off);
    int rd = fat32lite_read(&f, ufs_payload_buf(), count);
    fat32lite_close(&f);
    if (rd < 0) {
        r->status = -U_EIO;
        return r->status;
    }
    r->out0 = (uint64_t)rd;
    r->payload_len = (uint32_t)rd;
    return 0;
}

static int64_t fat_write(uint64_t ino, uint64_t off, const uint8_t *data,
                         uint32_t count, ufs_resp_hdr_t *r)
{
    fat_node_t *n = node_by_ino(ino);
    if (!n) {
        r->status = -U_ENOENT;
        return r->status;
    }
    fat32lite_file_t f;
    int st = fat32lite_open(&g_fs, n->path, &f);
    if (st == FAT32LITE_ENOENT)
        st = fat32lite_create(&g_fs, n->path, &f);
    if (st != FAT32LITE_OK) {
        r->status = -U_EIO;
        return r->status;
    }
    /* 同 fat32lite_append 的约定：既有文件以可写语义续用 */
    f.writable = 1;
    if (off > f.size) {
        fat32lite_close(&f);
        r->status = -U_EINVAL;
        return r->status;
    }
    fat32lite_seek(&f, (uint32_t)off);
    int wr = fat32lite_write(&f, data, count);
    fat32lite_close(&f);
    if (wr < 0) {
        r->status = -U_EIO;
        return r->status;
    }
    r->out0 = (uint64_t)wr;
    return 0;
}

static int64_t fat_rename(uint64_t old_dir, const char *old_name,
                          uint64_t new_dir, const char *new_name,
                          ufs_resp_hdr_t *r)
{
    (void)old_dir;
    (void)old_name;
    (void)new_dir;
    (void)new_name;
    (void)r;
    return -U_ENOSYS; /* fat32lite 无重命名原语 */
}

static int64_t fat_sync(void)
{
    /* 直写后端无需冲刷 */
    return 0;
}

static void fat_statfs(ufs_resp_hdr_t *r)
{
    /* fat32lite 不维护空闲计数；零值即可 */
    (void)r;
}

const ufs_backend_t UFS_BACKEND_FAT = {
    .name     = "fat",
    .mount    = fat_mount,
    .lookup   = fat_lookup,
    .getattr  = fat_getattr,
    .readdir  = fat_readdir,
    .create   = fat_create,
    .mkdir    = fat_mkdir,
    .unlink   = fat_unlink,
    .rmdir    = fat_rmdir,
    .truncate = fat_truncate,
    .read     = fat_read,
    .write    = fat_write,
    .rename   = fat_rename,
    .sync     = fat_sync,
    .statfs   = fat_statfs,
};

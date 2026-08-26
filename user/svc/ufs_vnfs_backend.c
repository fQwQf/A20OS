/*
 * ufs_vnfs_backend.c — vnode 型文件系统的通用用户态适配器。
 *
 * 承载经 fscompat 兼容环境原样编译的内核 diskfs 实现（ext4 / ntfs /
 * iso9660）。内核侧以 ino 寻址，本适配器维护 ino→vnode* 映射；所有
 * 文件操作经由各 FS 自身的 vnode_ops/vfile_ops 表执行，块 IO 经共享的
 * fs_block_io 受控通道进入内核块层。
 *
 * 服务进程重启后映射表清空：崩溃恢复契约要求重新挂载（新 ino 空间），
 * 与 ubd_recover 的语义一致。
 */
#include <stdint.h>
#include "ufs_backends.h"
#include "core/types.h"
#include "core/stdio.h"
#include "core/string.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/block_cache.h"
#include "fs/ext4.h"
#include "fs/isofs.h"
#include "fs/ntfs.h"
#include "fs/ntfs_internal.h"

extern int  fsio_read(uint32_t lba, void *buf, uint32_t count);
extern int  fsio_write(uint32_t lba, const void *buf, uint32_t count);
extern uint64_t fsio_capacity_sectors(void);

#define VNFS_MAX_NODES 512

typedef struct {
    uint64_t ino;
    vnode_t *vn;
} vn_node_t;

static bcache_t   *g_bc;
static vnode_t    *g_root;
static vn_node_t   g_nodes[VNFS_MAX_NODES];
static int         g_readonly;

static vnode_t *vn_map_get(uint64_t ino)
{
    for (int i = 0; i < VNFS_MAX_NODES; i++)
        if (g_nodes[i].vn && g_nodes[i].ino == ino)
            return g_nodes[i].vn;
    return 0;
}

static void vn_map_insert(uint64_t ino, vnode_t *vn)
{
    for (int i = 0; i < VNFS_MAX_NODES; i++) {
        if (!g_nodes[i].vn) {
            g_nodes[i].ino = ino;
            g_nodes[i].vn = vn;
            return;
        }
    }
}

static void vn_map_drop(uint64_t ino)
{
    for (int i = 0; i < VNFS_MAX_NODES; i++) {
        if (g_nodes[i].vn && g_nodes[i].ino == ino) {
            vnode_put(g_nodes[i].vn);
            g_nodes[i].vn = 0;
            return;
        }
    }
}

static uint8_t ft_of(const vnode_t *vn)
{
    switch (vn->type) {
    case VFS_FT_DIR: return UFS_FT_DIR;
    case VFS_FT_SYMLINK: return UFS_FT_SYMLINK;
    default: return UFS_FT_FILE;
    }
}

/* ---- 块设备描述符：把受控 IO 通道装配成 bcache 需要的形状 ---- */

static int dev_read_sector(struct block_dev *dev, uint64_t lba, void *buf,
                           size_t count)
{
    (void)dev;
    return fsio_read(lba, buf, (uint32_t)count) ? -1 : 0;
}

static int dev_write_sector(struct block_dev *dev, uint64_t lba,
                            const void *buf, size_t count)
{
    (void)dev;
    if (g_readonly)
        return -1;
    return fsio_write(lba, buf, (uint32_t)count) ? -1 : 0;
}

int vnfs_mount_generic(const char *fstype)
{
    static block_dev_t dev;
    dev.read_sector = dev_read_sector;
    dev.write_sector = dev_write_sector;
    dev.sector_size = 512;
    dev.capacity = fsio_capacity_sectors() * 512ull;
    dev.priv = 0;

    g_bc = bcache_create(&dev);
    if (!g_bc)
        return -U_EIO;

    if (strcmp(fstype, "ext4") == 0) {
        g_root = ext4_mount(g_bc);
        g_readonly = 0;
    } else if (strcmp(fstype, "iso9660") == 0) {
        g_root = isofs_mount(g_bc);
        g_readonly = 1;
    } else     if (strcmp(fstype, "ntfs") == 0) {
        /* 内核 ntfs 写路径已由 smoke-native-fs-all 的在库端到端测试覆盖
         * （创建/写读/目录/删除/崩溃重启持久化），开放读写语义。 */
        g_root = ntfs_mount(g_bc);
        g_readonly = 0;
    } else {
        return -U_ENOSYS;
    }
    if (!g_root)
        return -U_EINVAL;

    /*
     * 防御性规范：任何文件系统的根必须是目录。部分宿主 mkfs 变体的
     * 根记录标志位与内核解析存在出入（内核 ntfs 此前无在库挂载方），
     * 在适配层归一，不改动共享源码。
     */
    if (g_root->type != VFS_FT_DIR) {
        g_root->type = VFS_FT_DIR;
        g_root->mode = (g_root->mode & 07777u) | 0040000u;
    }

    for (int i = 0; i < VNFS_MAX_NODES; i++)
        g_nodes[i].vn = 0;
    vn_map_insert(UFS_ROOT_INO, g_root);
    return 0;
}

/* ---- 协议操作 ---- */

static void put_fail(ufs_resp_hdr_t *r, int err)
{
    r->status = err;
}

static int64_t vn_lookup(uint64_t dir_ino, const char *name,
                         uint32_t name_len, ufs_resp_hdr_t *r)
{
    (void)name_len;
    vnode_t *dir = vn_map_get(dir_ino);
    if (!dir) {
        put_fail(r, -U_ESTALE);
        return r->status;
    }
    if (!dir->ops || !dir->ops->lookup) {
        put_fail(r, -U_EINVAL);
        return r->status;
    }
    vnode_t *out = NULL;
    int rc = dir->ops->lookup(dir, name, &out);
    if (rc != 0) {
        put_fail(r, rc);
        return rc;
    }
    vn_map_insert(out->ino, out);
    r->out0 = out->ino;
    r->out1 = ft_of(out);
    r->out2 = ((uint64_t)out->mode << 32) | (uint32_t)out->size;
    return 0;
}

static int64_t vn_getattr(uint64_t ino, ufs_resp_hdr_t *r)
{
    vnode_t *vn = vn_map_get(ino);
    if (!vn) {
        put_fail(r, -U_ESTALE);
        return r->status;
    }
    kstat_t st;
    memset(&st, 0, sizeof(st));
    int rc = vn->ops->stat ? vn->ops->stat(vn, &st) : 0;
    if (rc != 0) {
        put_fail(r, rc);
        return rc;
    }
    r->out0 = st.st_size;
    r->out1 = (uint64_t)(st.st_mode ? st.st_mode : vn->mode) << 32;
    return 0;
}

static int64_t vn_readdir(uint64_t dir_ino, uint64_t skip,
                          ufs_resp_hdr_t *r)
{
    vnode_t *dir = vn_map_get(dir_ino);
    if (!dir) {
        put_fail(r, -U_ESTALE);
        return r->status;
    }
    vfile_t *vf = dir->ops && dir->ops->open ? dir->ops->open(dir, 0) : NULL;
    if (!vf) {
        put_fail(r, -U_EMFILE);
        return r->status;
    }

    uint8_t *out = ufs_payload_buf();
    uint32_t cap = ufs_payload_cap();
    uint32_t used = 0;

    static uint8_t stream[4096];
    for (;;) {
        int n = vf->ops && vf->ops->readdir
                    ? vf->ops->readdir(vf, stream, sizeof(stream))
                    : 0;
        if (n <= 0)
            break;

        uint32_t i = 0;
        while (i + sizeof(vfs_dirent64_t) <= (uint32_t)n &&
               used + 10 + 32 <= cap) {
            vfs_dirent64_t *de = (vfs_dirent64_t *)(stream + i);
            if (skip) { skip--; goto next_entry; }

            uint8_t nlen = (uint8_t)strlen(de->d_name);
            uint8_t ft = de->d_type == 4 ? UFS_FT_DIR
                       : de->d_type == 10 ? UFS_FT_SYMLINK
                                          : UFS_FT_FILE;
            uint64_t ino = de->d_ino;
            memcpy(out + used, &ino, 8); used += 8;
            out[used++] = ft;
            out[used++] = nlen;
            memcpy(out + used, de->d_name, nlen); used += nlen;

        next_entry:
            i += de->d_reclen ? de->d_reclen
                              : (uint16_t)(sizeof(vfs_dirent64_t));
        }
        if (n < (int)sizeof(stream))
            break; /* 本批已尽 */
        if (used + 10 + 32 > cap)
            break;
    }
    if (vf->ops && vf->ops->close)
        vf->ops->close(vf);
    r->payload_len = used;
    return 0;
}

static int64_t vn_create(uint64_t dir_ino, const char *name, uint32_t mode,
                         ufs_resp_hdr_t *r)
{
    vnode_t *dir = vn_map_get(dir_ino);
    if (!dir) {
        put_fail(r, -U_ESTALE);
        return r->status;
    }
    if (!dir->ops || !dir->ops->create) {
        put_fail(r, g_readonly ? -U_EROFS : -U_ENOSYS);
        return r->status;
    }
    vnode_t *out = NULL;
    int rc = dir->ops->create(dir, name, mode & 07777, &out);
    if (rc != 0) {
        put_fail(r, rc);
        return rc;
    }
    vn_map_insert(out->ino, out);
    r->out0 = out->ino;
    return 0;
}

static int64_t vn_mkdir(uint64_t dir_ino, const char *name,
                        ufs_resp_hdr_t *r)
{
    vnode_t *dir = vn_map_get(dir_ino);
    if (!dir) {
        put_fail(r, -U_ESTALE);
        return r->status;
    }
    if (!dir->ops || !dir->ops->mkdir) {
        put_fail(r, g_readonly ? -U_EROFS : -U_ENOSYS);
        return r->status;
    }
    int rc = dir->ops->mkdir(dir, name, 0755);
    if (rc != 0) {
        put_fail(r, rc);
        return rc;
    }
    /* 注册新目录以便后续寻址 */
    vnode_t *created = NULL;
    if (dir->ops->lookup && dir->ops->lookup(dir, name, &created) == 0 &&
        created) {
        vn_map_insert(created->ino, created);
        r->out0 = created->ino;
    }
    return 0;
}

static int64_t vn_unlink(uint64_t dir_ino, const char *name)
{
    vnode_t *dir = vn_map_get(dir_ino);
    if (!dir || !dir->ops || !dir->ops->unlink)
        return g_readonly ? -U_EROFS : -U_ENOSYS;

    /* 先解析目标以便失效映射 */
    vnode_t *victim = NULL;
    if (dir->ops->lookup)
        dir->ops->lookup(dir, name, &victim);

    int rc = dir->ops->unlink(dir, name);
    if (victim) {
        if (rc == 0)
            vn_map_drop(victim->ino);
        else
            vnode_put(victim); /* lookup 引用 */
    }
    return rc;
}

static int64_t vn_rmdir(uint64_t dir_ino, const char *name)
{
    vnode_t *dir = vn_map_get(dir_ino);
    if (!dir || !dir->ops || !dir->ops->rmdir)
        return g_readonly ? -U_EROFS : -U_ENOSYS;

    vnode_t *victim = NULL;
    if (dir->ops->lookup)
        dir->ops->lookup(dir, name, &victim);

    int rc = dir->ops->rmdir(dir, name);
    if (victim) {
        if (rc == 0)
            vn_map_drop(victim->ino);
        else
            vnode_put(victim);
    }
    return rc;
}

static int64_t vn_truncate(uint64_t ino, uint64_t size)
{
    vnode_t *vn = vn_map_get(ino);
    if (!vn)
        return -U_ESTALE;
    if (!vn->ops || !vn->ops->truncate)
        return g_readonly ? -U_EROFS : -U_ENOSYS;
    return vn->ops->truncate(vn, (size_t)size);
}

static int64_t vn_rw_open(uint64_t ino, int write, vfile_t **out_vf,
                          vnode_t **out_vn)
{
    vnode_t *vn = vn_map_get(ino);
    if (!vn)
        return -U_ESTALE;
    if (!vn->ops || !vn->ops->open)
        return -U_ENOSYS;
    vfile_t *vf = vn->ops->open(vn, write ? 1 : 0);
    if (!vf)
        return write && g_readonly ? -U_EROFS : -U_EIO;
    *out_vf = vf;
    *out_vn = vn;
    return 0;
}

static int64_t vn_read(uint64_t ino, uint64_t off, uint32_t count,
                       ufs_resp_hdr_t *r)
{

    vfile_t *vf;
    vnode_t *vn;
    int64_t rc = vn_rw_open(ino, 0, &vf, &vn);
    if (rc != 0) {
        put_fail(r, (int)rc);
        return rc;
    }
    uint32_t cap = ufs_payload_cap();
    if (count > cap)
        count = cap;

    /* 经 lseek 同步后端私有游标（如 ntfs 的 fc->file_off）：
     * 仅设 vf->offset 时，页粒度 RPC 的非零偏移会被忽略。 */
    if (vf->ops && vf->ops->lseek)
        vf->ops->lseek(vf, (long)off, 0);
    else
        vf->offset = off;
    int n = vf->ops && vf->ops->read ? vf->ops->read(vf,
                                                     (char *)ufs_payload_buf(),
                                                     count)
                                     : -U_ENOSYS;

    if (vf->ops && vf->ops->close)
        vf->ops->close(vf);
    if (n < 0) {
        put_fail(r, n);
        return n;
    }
    r->out0 = (uint64_t)n;
    r->payload_len = (uint32_t)n;
    (void)vn;
    return 0;
}

static int64_t vn_write(uint64_t ino, uint64_t off, const uint8_t *data,
                        uint32_t count, ufs_resp_hdr_t *r)
{
    vfile_t *vf;
    vnode_t *vn;
    int64_t rc = vn_rw_open(ino, 1, &vf, &vn);
    if (rc != 0) {
        put_fail(r, (int)rc);
        return rc;
    }
    if (vf->ops && vf->ops->lseek)
        vf->ops->lseek(vf, (long)off, 0);
    else
        vf->offset = off;
    int n = vf->ops && vf->ops->write ? vf->ops->write(vf,
                                                       (const char *)data,
                                                       count)
                                      : -U_ENOSYS;
    if (vf->ops && vf->ops->close)
        vf->ops->close(vf);
    if (n < 0) {
        put_fail(r, n);
        return n;
    }
    if (vn->size < off + (uint64_t)n)
        vn->size = (size_t)(off + n);
    r->out0 = (uint64_t)n;
    return 0;
}

static int64_t vn_rename(uint64_t old_dir, const char *old_name,
                         uint64_t new_dir, const char *new_name,
                         ufs_resp_hdr_t *r)
{
    vnode_t *od = vn_map_get(old_dir);
    vnode_t *nd = vn_map_get(new_dir);
    if (!od || !nd) {
        put_fail(r, -U_ESTALE);
        return r->status;
    }
    if (!od->ops || !od->ops->rename) {
        put_fail(r, g_readonly ? -U_EROFS : -U_ENOSYS);
        return r->status;
    }
    int rc = od->ops->rename(od, old_name, nd, new_name, 0);
    if (rc != 0)
        put_fail(r, rc);
    return rc;
}

static int64_t vn_sync(void)
{
    if (g_bc)
        bcache_sync(g_bc);
    return 0;
}

static void vn_statfs(ufs_resp_hdr_t *r)
{
    if (!g_root || !g_root->ops || !g_root->ops->statfs)
        return;
    kstatfs_t ks;
    memset(&ks, 0, sizeof(ks));
    if (g_root->ops->statfs(g_root, &ks) != 0)
        return;
    r->out0 = ks.f_blocks * ks.f_bsize;
    r->out1 = ks.f_bfree * ks.f_bsize;
}

const ufs_backend_t UFS_BACKEND_VNFS = {
    .name     = "vnfs",
    .mount    = vnfs_mount_generic,
    .lookup   = vn_lookup,
    .getattr  = vn_getattr,
    .readdir  = vn_readdir,
    .create   = vn_create,
    .mkdir    = vn_mkdir,
    .unlink   = vn_unlink,
    .rmdir    = vn_rmdir,
    .truncate = vn_truncate,
    .read     = vn_read,
    .write    = vn_write,
    .rename   = vn_rename,
    .sync     = vn_sync,
    .statfs   = vn_statfs,
};

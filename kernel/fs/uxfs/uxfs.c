/*
 * uxfs.c — 用户态文件系统代理（uxfs）。
 *
 * 把 VFS vnode/vfile 操作翻译为 ufs_proto.h 线协议消息，经 Channel IPC
 * 转发给用户态文件服务（ufsd）。VFS 核心、页缓存与块层留在内核；文件系统
 * 实现运行在可崩溃、可重启的用户态进程中（docs/hybrid-kernel/06-user-fs.md）。
 *
 * 并发模型：每个挂载一把请求互斥锁，串行化"发送请求-等待应答"对；服务侧
 * 是单线程循环，乱序/陈旧应答按 req_id 丢弃。服务断链后所有在飞请求以
 * -EIO 收场，重启 ufsd 后需重新挂载。
 */
#include "fs/uxfs.h"
#include "fs/ufs_proto.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/dcache.h"
#include "fs/page_cache.h"
#include "drivers/block/block_dev.h"
#include "ipc/ipc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/sync.h"
#include "proc/proc.h"

extern block_dev_t *mount_setup_block_device(int index);

#define UXFS_DBG 0

typedef struct uxfs_sb {
    struct a20_channel_ep *ep;   /* 服务端点；引用归属本 sb */
    struct task_t         *server;
    int                    block_index;
    uint32_t               next_req_id;
    mutex_t                req_lock;
} uxfs_sb_t;

typedef struct uxfs_vpriv {
    uxfs_sb_t *sb;
    int        is_dir;
} uxfs_vpriv_t;

typedef struct uxfs_fctx {
    uxfs_sb_t *sb;
    uint64_t   ino;
    int        is_dir;
    size_t     off;      /* 文件偏移 / 目录 cookie（字节） */
} uxfs_fctx_t;

/* ------------------------------------------------------------------ */
/* RPC 引擎                                                            */
/* ------------------------------------------------------------------ */

static int uxfs_rpc(uxfs_sb_t *sb, const ufs_req_hdr_t *req,
                    const char *name, const void *payload,
                    ufs_resp_hdr_t *resp, void *resp_payload,
                    uint32_t resp_cap, uint32_t *resp_payload_len)
{
    uint32_t total = sizeof(*req) + req->name_len + req->payload_len;
    uint8_t *msg = (uint8_t *)kmalloc(total);
    if (!msg)
        return -ENOMEM;

    memcpy(msg, req, sizeof(*req));
    if (req->name_len)
        memcpy(msg + sizeof(*req), name, req->name_len);
    if (req->payload_len)
        memcpy(msg + sizeof(*req) + req->name_len, payload, req->payload_len);

    mutex_lock(&sb->req_lock);

    /* 引用计数：send/recv 期间防止并发 handle_close 释放 ep。 */
    int64_t r = a20_channel_send(sb->ep, msg, total, NULL, 0, NULL, 0);
    kfree(msg);
    if (r < 0) {
        mutex_unlock(&sb->req_lock);
        return -EIO;
    }

    static uint8_t rx[A20_CH_MAX_DATA];
    for (;;) {
        uint32_t in_len = sizeof(rx);
        a20_ch_handle_info_t hinfos[1];
        uint32_t nh = 0;
        r = a20_channel_recv_begin(sb->ep, 0, &in_len, &nh);
        if (r < 0)
            break;
        if (in_len > sizeof(rx)) {
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
            continue; /* 陈旧应答：丢弃并继续等待 */

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

    mutex_unlock(&sb->req_lock);
#if UXFS_DBG
    if (r < 0)
        kdebug("[UXFS] rpc op=%u failed: %lld\n", req->opcode, (long long)r);
#endif
    if (r < 0)
        return -EIO;
    return (int)resp->status;
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

/* ------------------------------------------------------------------ */
/* vnode 工厂                                                          */
/* ------------------------------------------------------------------ */

static vnode_ops_t g_uxfs_vnops;

/* type/mode 来自服务端 GETATTR/LOOKUP 应答：out1 为 S_IF*|perm 位型 */
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
            vnode_get(dir); /* 调用方会对 dir 做 vnode_put */
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
    /* 刷新 vnode 缓存的尺寸，保持 open fd 读到的长度与服务端一致 */
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

    (void)flags; /* RENAME_EXCHANGE 等扩展语义暂不支持 */
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
    /* 文件尾页允许短读，剩余部分补零 */
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

    /* cookie 语义：已消费的目录项条数。服务端跳过 arg0 项后继续返回。 */
    uxfs_req_init(fc->sb, &req, UFS_OP_READDIR, fc->ino, fc->off, count,
                  NULL, 0);
    int rc = uxfs_rpc(fc->sb, &req, NULL, NULL, &resp, payload,
                      sizeof(payload), &plen);
    if (rc != 0)
        return rc;

    /* 服务端紧凑目录项序列 → VFS dirent64 流（8 字节对齐），与 FAT32
     * readdir 的输出契约一致。 */
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
        dent->d_off    = (int64_t)(ordinal + 1); /* 已消费条目数 = 下一 cookie */
        dent->d_reclen = (uint16_t)reclen;
        dent->d_type   = (type == UFS_FT_DIR) ? 4 :
                         (type == UFS_FT_SYMLINK) ? 10 : 8; /* DT_* */
        memcpy(dent->d_name, nm, nlen);
        dent->d_name[nlen] = '\0';
        total += reclen;
        ordinal++;
    }

    if (total > 0)
        fc->off = ordinal; /* 目录耗尽或调用缓冲满：推进到已消费位置 */
    vf->offset = fc->off;
    return (int)total;
}

static int uxfs_fclose(vfile_t *vf)
{
    uxfs_fctx_t *fc = (uxfs_fctx_t *)vf->priv;
    if (fc) {
        /* Flush dirty pages while the ufsd channel is guaranteed alive:
         * user-space FS servers crash/restart routinely, so write-behind
         * past close would race server death. */
        if (vf->vnode)
            page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        kfree(fc);
        vf->priv = NULL;
    }
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
    uxfs_fctx_t *fc = (uxfs_fctx_t *)kmalloc(sizeof(uxfs_fctx_t));
    if (!fc)
        return NULL;
    memset(fc, 0, sizeof(*fc));
    fc->sb     = p->sb;
    fc->ino    = vn->ino;
    fc->is_dir = p->is_dir;
    fc->off    = (flags & 0x400 /* O_APPEND */) ? vn->size : 0;

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        kfree(fc);
        return NULL;
    }
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags  = flags;
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
/* 挂载 / 卸载 / 块 IO 所有权                                          */
/* ------------------------------------------------------------------ */

#define UXFS_MAX_SBS 8
static uxfs_sb_t g_uxfs_sbs[UXFS_MAX_SBS];

/* 同一盘号允许多个挂载并存（各自独立的服务实例）；只匹配调用方
 * 自己的注册，其他实例的同号挂载跳过继续找。 */
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
    sb->server      = server;
    sb->block_index = block_index;
    sb->next_req_id = 0;
    mutex_init(&sb->req_lock);

    /*
     * 无同步握手：fs_serve 的调用方就是服务进程自身，若在此阻塞等待
     * INIT 应答会自我死锁。活跃性由第一个真实文件操作验证；服务死亡时
     * channel 断链使请求以 -EIO 收场。
     */
    uint64_t root_ino = UFS_ROOT_INO;

    vnode_t *root = uxfs_make_vnode(sb, root_ino, VFS_FT_DIR,
                                    (S_IFDIR | 0755), 0, NULL);
    if (!root) {
        sb->ep = NULL;
        return -ENOMEM;
    }

    mount_t *mnt = vfs_mount_alloc();
    if (!mnt) {
        vnode_put(root);
        sb->ep = NULL;
        return -ENOMEM;
    }
    strncpy(mnt->path, path, MAX_PATH_LEN - 1);
    mnt->path[MAX_PATH_LEN - 1] = '\0';
    mnt->type  = FS_TYPE_UXFS;
    /* bit0: 服务端声明只读后端（如 iso9660）；页缓存缓冲写据此禁用。 */
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
    vnode_get(root); /* mount 持有持久引用（与其他 FS 一致） */
    vfs_dcache_invalidate_all();
    kinfo("[UXFS] mounted user filesystem at %s (block=%d)\n", path,
          block_index);
    return 0;
}

void uxfs_unmount(struct vnode *root)
{
    if (!root)
        return;
    uxfs_vpriv_t *p = (uxfs_vpriv_t *)root->fs_data;
    if (!p)
        return;
    uxfs_sb_t *sb = p->sb;
    if (sb && sb->ep) {
        /* 先单向断链让服务进程的 recv 返回错误并退出（服务自身仍持有
         * 配对端点引用，仅靠 release 无法触达对端）；随后释放本侧引用。 */
        a20_channel_ep_peer_shutdown(sb->ep);
        a20_channel_ep_release(sb->ep);
        sb->ep = NULL;
    }
    sb->server = NULL;
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
    /* block_dev_t.capacity 的既定单位就是扇区（virtio 配置空间语义，
     * mount_setup/class_ops 原样传递），不再做字节换算。 */
    block_dev_t *dev = uxfs_owned_block_dev(task, block_index);
    if (!dev || !out_sectors)
        return -ENODEV;
    *out_sectors = dev->capacity;
    return 0;
}

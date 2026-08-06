#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/timekeeping.h"
#include "proc/proc.h"

/* ext4 file I/O: per-open context, read/write/lseek/readdir. */

ext4_inode_t *ext4_fctx_inode(ext4_fctx_t *fc) {
    if (!fc->inode_valid) {
        if (ext4_read_inode(fc->sb, fc->inode_num, &fc->inode) < 0)
            return NULL;
        fc->inode_valid = 1;
    }
    return &fc->inode;
}


void ext4_fctx_inode_dirty(ext4_fctx_t *fc) {
    fc->inode_valid = 0;
    fc->ext_valid = 0;
}


void ext4_fctx_refresh_size(vfile_t *vf, ext4_fctx_t *fc) {
    ext4_inode_t inode;
    if (ext4_read_inode(fc->sb, fc->inode_num, &inode) < 0) return;
    fc->file_size = ext4_inode_size(&inode);
    if (vf->vnode) {
        vf->vnode->size = fc->file_size;
        if (vf->vnode->fs_data) {
            ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vf->vnode->fs_data;
            fp->file_size = fc->file_size;
        }
    }
}


void ext4_fctx_set_size(vfile_t *vf, ext4_fctx_t *fc, uint64_t size) {
    fc->file_size = size;
    if (vf->vnode) {
        vf->vnode->size = fc->file_size;
        if (vf->vnode->fs_data) {
            ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vf->vnode->fs_data;
            fp->file_size = fc->file_size;
        }
    }
}


int ext4_fread(vfile_t *vf, char *buf, size_t count) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    if (fc->is_dir) return -EISDIR;
    if (count == 0) return 0;

    if (vf->vnode && vf->vnode->size != fc->file_size)
        ext4_fctx_inode_dirty(fc);
    ext4_inode_t *inode = ext4_fctx_inode(fc);
    if (!inode) return -EIO;
    ext4_fctx_set_size(vf, fc, ext4_inode_size(inode));
    if (fc->file_off >= fc->file_size) return 0;

    size_t remaining = fc->file_size - fc->file_off;
    if (count > remaining) count = remaining;
    if (count == 0) return 0;

    uint32_t bs = fc->sb->block_size;
    size_t done = 0;
    while (done < count) {
        size_t foff = fc->file_off + done;
        uint32_t lblk = (uint32_t)(foff / bs);
        uint32_t loff = (uint32_t)(foff % bs);
        size_t chunk = bs - loff;
        if (chunk > count - done) chunk = count - done;

        uint64_t phys = ext4_block_map_cached(fc, inode, lblk);
        if (!phys) {
            memset(buf + done, 0, chunk);
        } else {
            int r = bcache_read_bytes(fc->sb->bc, phys * bs + loff, buf + done, chunk);
            if (r < 0) break;
        }
        done += chunk;
    }

    fc->file_off += done;
    vf->offset = fc->file_off;
    return (int)done;
}


int ext4_fwrite(vfile_t *vf, const char *buf, size_t count) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    if (fc->is_dir) return -EISDIR;
    if (count == 0) return 0;

    if (vf->vnode && vf->vnode->size != fc->file_size)
        ext4_fctx_inode_dirty(fc);
    ext4_inode_t *inode = ext4_fctx_inode(fc);
    if (!inode) return -EIO;
    ext4_fctx_set_size(vf, fc, ext4_inode_size(inode));

    uint32_t bs = fc->sb->block_size;
    size_t done = 0;

    while (done < count) {
        size_t foff = fc->file_off + done;
        uint32_t lblk = (uint32_t)(foff / bs);
        uint32_t loff = (uint32_t)(foff % bs);
        size_t chunk = bs - loff;
        if (chunk > count - done) chunk = count - done;

        uint64_t phys = ext4_block_map_cached(fc, inode, lblk);
        if (!phys) {
            uint64_t nb = ext4_alloc_block(fc->sb);
            if (!nb) break;
            int gr = ext4_block_grow(fc->sb, inode, lblk, nb);
            if (gr < 0) { ext4_free_block(fc->sb, nb); break; }
            if (ext4_write_inode(fc->sb, fc->inode_num, inode) < 0) {
                ext4_free_block(fc->sb, nb);
                break;
            }
            phys = nb;
            fc->ext_valid = 0;
        }

        int r = bcache_write_bytes(fc->sb->bc, phys * bs + loff, buf + done, chunk);
        if (r < 0) break;
        done += chunk;
    }

    if (done > 0) {
        fc->file_off += done;
        if (fc->file_off > fc->file_size) {
            fc->file_size = fc->file_off;
            ext4_inode_set_size(inode, fc->file_size);
            ext4_write_inode(fc->sb, fc->inode_num, inode);
            if (vf->vnode && vf->vnode->fs_data) {
                ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vf->vnode->fs_data;
                fp->file_size = fc->file_size;
                vf->vnode->size = fc->file_size;
            }
        }
        vf->offset = fc->file_off;
    }
    return (int)done;
}


int ext4_vn_readpage(vnode_t *vn, uint64_t index,
                            void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vn->fs_data;
    if (fp->type == VFS_FT_DIR)
        return -EISDIR;

    memset(data, 0, len);
    uint64_t off = index * PAGE_SIZE;
    if (off >= fp->file_size)
        return 0;
    size_t n = fp->file_size - (size_t)off;
    if (n > len)
        n = len;

    ext4_fctx_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.sb = fp->sb;
    fc.inode_num = fp->inode_num;
    fc.file_size = fp->file_size;
    fc.is_dir = 0;
    fc.file_off = (size_t)off;

    vfile_t local;
    memset(&local, 0, sizeof(local));
    local.vnode = vn;
    local.flags = O_RDONLY;
    local.offset = (size_t)off;
    local.priv = &fc;
    int r = ext4_fread(&local, (char *)data, n);
    return r;
}


int ext4_vn_writepage(vnode_t *vn, uint64_t index,
                             const void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vn->fs_data;
    if (fp->type == VFS_FT_DIR)
        return -EISDIR;

    uint64_t off = index * PAGE_SIZE;
    if (off >= fp->file_size)
        return 0;
    size_t n = fp->file_size - (size_t)off;
    if (n > len)
        n = len;

    ext4_fctx_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.sb = fp->sb;
    fc.inode_num = fp->inode_num;
    fc.file_size = fp->file_size;
    fc.is_dir = (fp->type == VFS_FT_DIR);
    fc.file_off = (size_t)off;

    vfile_t vf;
    memset(&vf, 0, sizeof(vf));
    vf.vnode = vn;
    vf.flags = O_RDWR;
    vf.offset = (size_t)off;
    vf.priv = &fc;

    int r = ext4_fwrite(&vf, (const char *)data, n);
    if (r < 0)
        return r;
    return (size_t)r == n ? 0 : -EIO;
}


long ext4_flseek(vfile_t *vf, long offset, int whence) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    long new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END:
            ext4_fctx_inode_dirty(fc);
            ext4_fctx_inode(fc);
            new_off = (long)fc->file_size + offset;
            break;
        default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    vf->offset   = (size_t)new_off;
    fc->file_off = (size_t)new_off;
    return new_off;
}


int ext4_freaddir(vfile_t *vf, void *dirp, size_t count) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    if (!fc->is_dir) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(fc->sb, fc->inode_num, &di) < 0) return -EIO;

    uint32_t bs = fc->sb->block_size;
    uint32_t total_blocks = (fc->file_size + bs - 1) / bs;
    char *out = (char *)dirp;
    size_t total = 0;

    for (uint32_t b = (uint32_t)(fc->dir_off / bs); b < total_blocks; b++) {
        uint64_t p = ext4_block_map(fc->sb, &di, b);
        if (!p) continue;
        char *blk = (char *)kmalloc(bs);
        if (!blk) break;
        if (bcache_read_bytes(fc->sb->bc, p * bs, blk, bs) < 0) { kfree(blk); break; }

        uint32_t start = (b == (uint32_t)(fc->dir_off / bs)) ? (uint32_t)(fc->dir_off % bs) : 0;
        uint32_t off = start;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0)
                break;
            if (de->inode == 0) { off += de->rec_len; continue; }

            char fname[256];
            size_t nl = de->name_len;
            if (nl > 255) nl = 255;
            memcpy(fname, de->name, nl);
            fname[nl] = '\0';

            size_t reclen = offsetof(vfs_dirent64_t, d_name) + nl + 1;
            reclen = (reclen + 7) & ~7UL;
            if (total + reclen > count) { kfree(blk); goto out; }

            vfs_dirent64_t *dent = (vfs_dirent64_t *)(out + total);
            dent->d_ino    = de->inode;
            dent->d_off    = (int64_t)(b * bs + off);
            dent->d_reclen = (uint16_t)reclen;
            dent->d_type   = (de->file_type == EXT4_FT_DIR) ? DT_DIR :
                             (de->file_type == EXT4_FT_SYMLINK) ? DT_LNK : DT_REG;
            memcpy(dent->d_name, fname, nl + 1);
            total += reclen;

            off += de->rec_len;
            fc->dir_off = b * bs + off;
        }
        kfree(blk);
    }

out:
    return (int)total;
}


int ext4_fclose(vfile_t *vf) {
    if (vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}


vfile_t *ext4_open_vnode(vnode_t *vn, int flags) {
    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_fctx_t *fc = (ext4_fctx_t *)kmalloc(sizeof(ext4_fctx_t));
    if (!fc) return NULL;
    memset(fc, 0, sizeof(*fc));
    fc->sb        = fp->sb;
    fc->inode_num = fp->inode_num;
    uint64_t current_size = fp->file_size;
    if (ext4_read_inode(fp->sb, fp->inode_num, &fc->inode) == 0) {
        current_size = ext4_inode_size(&fc->inode);
        fp->file_size = current_size;
        vn->size = current_size;
        fc->inode_valid = 1;
    }
    fc->file_size = current_size;
    fc->is_dir    = (fp->type == VFS_FT_DIR);
    fc->file_off  = (flags & O_APPEND) ? current_size : 0;
    fc->dir_off   = 0;

    vfile_t *vf = vfile_alloc();
    if (!vf) { kfree(fc); return NULL; }
    vf->vnode     = vn;
    vnode_get(vn);
    vf->flags     = flags;
    vf->offset    = fc->file_off;
    vfile_ref_init(vf, 1);
    vf->ops       = &g_ext4_fops;
    vf->priv      = fc;
    return vf;
}


vfile_ops_t g_ext4_fops = {
    .read    = ext4_fread,
    .write   = ext4_fwrite,
    .lseek   = ext4_flseek,
    .readdir = ext4_freaddir,
    .ioctl   = NULL,
    .close   = ext4_fclose,
};

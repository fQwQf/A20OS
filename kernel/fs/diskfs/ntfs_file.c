#include "fs/ntfs.h"
#include "fs/ntfs_format.h"
#include "fs/ntfs_internal.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/sync.h"
#include "fs/block_cache.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/* NTFS file I/O. */

int ntfs_vn_readpage(vnode_t *vn, uint64_t index, void *data, size_t len)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return -EINVAL;
    uint64_t off = index * PAGE_SIZE;
    memset(data, 0, len);
    ntfs_lock(fp->sb);
    int r = 0;
    if (ntfs_resolve_data(fp) < 0) {
        r = -EIO;
    } else if (fp->data_resident) {
        if (off < fp->data_size) {
            size_t n = fp->data_size - off;
            if (n > len)
                n = len;
            uint8_t *rec = kmalloc(fp->sb->mft_record_size);
            if (rec) {
                if (ntfs_read_record(fp->sb, fp->mft_index, rec) == 0)
                    memcpy(data, rec + fp->data_res_off + off, n);
                else
                    r = -EIO;
                kfree(rec);
            } else {
                r = -ENOMEM;
            }
        }
    } else {
        if (off < fp->data_size) {
            size_t n = fp->data_size - off;
            if (n > len)
                n = len;
            if (ntfs_stream_read(fp->sb, fp->runs, fp->run_count,
                                 fp->data_size, off, data, n) < 0)
                r = -EIO;
        }
    }
    ntfs_unlock(fp->sb);
    return r;
}


int ntfs_vn_writepage(vnode_t *vn, uint64_t index, const void *data, size_t len)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return -EINVAL;
    uint64_t off = index * PAGE_SIZE;
    ntfs_lock(fp->sb);
    int r = ntfs_write_file(fp, off, data, len);
    ntfs_unlock(fp->sb);
    return r;
}


vfile_t *ntfs_open_vnode(vnode_t *vn, int flags)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return NULL;

    ntfs_fctx_t *fc = (ntfs_fctx_t *)kmalloc(sizeof(ntfs_fctx_t));
    vfile_t *vf = vfile_alloc();
    if (!fc || !vf) {
        if (fc) kfree(fc);
        if (vf) vfile_free(vf);
        return NULL;
    }
    memset(fc, 0, sizeof(*fc));
    fc->sb = fp->sb;
    fc->mft_index = fp->mft_index;
    fc->is_dir = fp->is_dir;

    if (fp->is_dir) {
        ntfs_lock(fp->sb);
        if (ntfs_read_directory(fp, &fc->dirents, &fc->dirent_count) < 0)
            fc->dirent_count = 0;
        ntfs_unlock(fp->sb);
    }

    vfile_ref_init(vf, 1);
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    vf->ops = &g_ntfs_fops;
    vf->priv = fc;
    return vf;
}


int ntfs_fread(vfile_t *vf, char *buf, size_t count)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vf->vnode->fs_data;
    if (!fc || !fp || fc->is_dir)
        return -EISDIR;
    ntfs_lock(fc->sb);
    if (ntfs_resolve_data(fp) < 0) {
        ntfs_unlock(fc->sb);
        return -EIO;
    }
    size_t fsize = fp->data_size;
    if (fc->file_off >= fsize) {
        ntfs_unlock(fc->sb);
        return 0;
    }
    size_t n = fsize - fc->file_off;
    if (n > count)
        n = count;
    int r;
    if (fp->data_resident) {
        uint8_t *rec = kmalloc(fp->sb->mft_record_size);
        if (!rec) { ntfs_unlock(fc->sb); return -ENOMEM; }
        if (ntfs_read_record(fp->sb, fp->mft_index, rec) < 0) {
            kfree(rec); ntfs_unlock(fc->sb); return -EIO;
        }
        memcpy(buf, rec + fp->data_res_off + fc->file_off, n);
        kfree(rec);
        r = (int)n;
    } else {
        r = ntfs_stream_read(fp->sb, fp->runs, fp->run_count, fp->data_size,
                             fc->file_off, buf, n);
    }
    fc->file_off += (size_t)r;
    vf->offset = fc->file_off;
    ntfs_unlock(fc->sb);
    return r;
}


int ntfs_fwrite(vfile_t *vf, const char *buf, size_t count)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vf->vnode->fs_data;
    if (!fc || !fp || fc->is_dir)
        return -EISDIR;
    ntfs_lock(fc->sb);
    int r = ntfs_write_file(fp, fc->file_off, buf, count);
    if (r > 0) {
        fc->file_off += (size_t)r;
        vf->offset = fc->file_off;
        if (vf->vnode->size < fc->file_off)
            vf->vnode->size = fc->file_off;
    }
    ntfs_unlock(fc->sb);
    return r;
}


long ntfs_flseek(vfile_t *vf, long offset, int whence)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vf->vnode->fs_data;
    if (!fc || !fp)
        return -EBADF;
    size_t base = 0;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = fc->file_off;
    else if (whence == SEEK_END)
        base = fp->data_size;
    else
        return -EINVAL;
    fc->file_off = (size_t)(offset < 0 ? 0 : (long)(base) + (offset < 0 ? 0 : (unsigned long)offset));
    vf->offset = fc->file_off;
    return (long)fc->file_off;
}


int ntfs_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    if (!fc || !fc->is_dir)
        return -ENOTDIR;

    char *out = (char *)dirp;
    size_t total = 0;
    while (fc->dirent_off < fc->dirent_count) {
        ntfs_dir_entry_t *d = &fc->dirents[fc->dirent_off];
        size_t namelen = strlen(d->name);
        size_t reclen = sizeof(vfs_dirent64_t) + namelen + 1;
        reclen = (reclen + 7) & ~(size_t)7;
        if (total + reclen > count)
            break;
        vfs_dirent64_t *dent = (vfs_dirent64_t *)(out + total);
        dent->d_ino = d->ref & 0x0000FFFFFFFFFFFFULL;
        dent->d_off = (int64_t)fc->dirent_off;
        dent->d_reclen = (uint16_t)reclen;
        dent->d_type = d->is_dir ? 4 : 8;   /* DT_DIR / DT_REG */
        memcpy(dent->d_name, d->name, namelen + 1);
        total += reclen;
        fc->dirent_off++;
    }
    return (int)total;
}


int ntfs_fclose(vfile_t *vf)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    if (fc) {
        if (fc->dirents)
            kfree(fc->dirents);
        kfree(fc);
        vf->priv = NULL;
    }
    return 0;
}


int ntfs_write_file(ntfs_vnode_priv_t *fp, uint64_t off,
                           const void *buf, size_t len)
{
    ntfs_sb_t *sb = fp->sb;
    if (fp->is_dir)
        return -EISDIR;
    if (len == 0)
        return 0;
    if (ntfs_resolve_data(fp) < 0)
        return -EIO;

    uint64_t end = off + len;
    uint64_t resident_limit = sb->mft_record_size - 0x60;

    /* Try to stay resident. */
    if (fp->data_resident && end <= resident_limit) {
        uint8_t *rec = kmalloc(sb->mft_record_size);
        if (!rec)
            return -ENOMEM;
        if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
            kfree(rec);
            return -EIO;
        }
        uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_DATA, 1);
        int r = -1;
        if (attr && attr[8] == 0) {
            uint16_t voff = nget16(attr + 0x14);
            if (off < fp->data_size)
                memcpy(rec + voff + off, buf, len);
            else {
                if (off > fp->data_size)
                    memset(rec + voff + fp->data_size, 0, (size_t)(off - fp->data_size));
                memcpy(rec + voff + off, buf, len);
            }
            nput32(attr + 0x10, (uint32_t)end);
            uint32_t used = nget32(rec + 0x18);
            if (used < voff + end)
                nput32(rec + 0x18, voff + (uint32_t)end);
            fp->data_size = end;
            fp->data_res_len = (uint32_t)end;
            ntfs_update_file_name_size(sb, fp->mft_index, end, 0);
            r = ntfs_write_record(sb, fp->mft_index, rec);
        }
        kfree(rec);
        if (r < 0)
            return r;
        return (int)len;
    }

    /* Convert to non-resident if needed. */
    if (fp->data_resident) {
        if (ntfs_convert_resident_to_nonresident(fp) < 0)
            return -EIO;
    }

    /* Ensure runs cover the target range. */
    if (ntfs_ensure_data_runs(fp, end) < 0)
        return -ENOSPC;

    int r = ntfs_stream_write(sb, fp->runs, fp->run_count, off, buf, len);
    if (r <= 0)
        return r;
    if (end > fp->data_size) {
        fp->data_size = end;
        ntfs_update_file_name_size(sb, fp->mft_index, end, 0);
        /* Update $DATA initialized size. */
        uint8_t *rec = kmalloc(sb->mft_record_size);
        if (rec) {
            if (ntfs_read_record(sb, fp->mft_index, rec) == 0) {
                uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size,
                                               NTFS_AT_DATA, 1);
                if (attr && attr[8] == 1) {
                    nput64(attr + 0x38, end);
                    nput64(attr + 0x30, end);
                    ntfs_write_record(sb, fp->mft_index, rec);
                }
            }
            kfree(rec);
        }
    }
    return r;
}

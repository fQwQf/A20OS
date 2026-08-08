#include "fs/ntfs.h"
#include "fs/ntfs_format.h"
#include "fs/ntfs_internal.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/sync.h"
#include "fs/block_cache.h"
#include "fs/file.h"
#include "fs/vfs/stat_perm.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/* NTFS namei: vnode operations. */

vnode_t *ntfs_make_vnode(ntfs_sb_t *sb, uint64_t mft_index,
                                uint32_t seq, int is_dir, vnode_t *parent)
{
    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn)
        return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino = mft_index;
    vn->type = is_dir ? VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0755);
    vn->uid = 0;
    vn->gid = 0;
    vnode_ref_init(vn, 1);
    vn->parent = parent;
    if (parent)
        vnode_get(parent);
    if (parent)
        vn->mnt = parent->mnt;        /* inherit mount for link()/dcache */

    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)kmalloc(sizeof(ntfs_vnode_priv_t));
    if (!fp) {
        kfree(vn);
        return NULL;
    }
    memset(fp, 0, sizeof(*fp));
    fp->sb = sb;
    fp->mft_index = mft_index;
    fp->seq = seq;
    fp->is_dir = is_dir;
    vn->fs_data = fp;
    vn->ops = &g_ntfs_vnode_ops;
    return vn;
}


void ntfs_release_vn(vnode_t *vn)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return;
    if (fp->runs)
        kfree(fp->runs);
    kfree(fp);
    vn->fs_data = NULL;
}


int ntfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    if (!out || !dir)
        return -EINVAL;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp || !fp->is_dir)
        return -ENOTDIR;

    ntfs_dir_entry_t *entries = NULL;
    uint32_t count = 0;
    if (ntfs_read_directory(fp, &entries, &count) < 0)
        return -EIO;
    vnode_t *found = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            uint64_t ref = entries[i].ref;
            uint64_t mft = ref & 0x0000FFFFFFFFFFFFULL;
            uint32_t seq = (uint32_t)((ref >> 48) & 0xFFFF);
            found = ntfs_make_vnode(fp->sb, mft, seq, entries[i].is_dir, dir);
            if (found)
                found->size = entries[i].size;
            break;
        }
    }
    kfree(entries);
    if (!found)
        return -ENOENT;
    *out = found;
    return 0;
}


int ntfs_stat(vnode_t *vn, kstat_t *st)
{
    if (!vn || !st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_size = (int64_t)vn->size;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_blocks = 0;
    st->st_blksize = 512;
    st->st_nlink = 1;
    return 0;
}


int ntfs_statfs(vnode_t *vn, kstatfs_t *st)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp || !st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->f_bsize = fp->sb->bytes_per_cluster;
    st->f_blocks = fp->sb->total_clusters;
    st->f_bfree = fp->sb->total_clusters / 2;
    st->f_bavail = fp->sb->total_clusters / 2;
    st->f_namelen = 255;
    return 0;
}


void ntfs_update_file_name_size(ntfs_sb_t *sb, uint64_t mft_index,
                                       uint64_t size, int is_dir)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return;
    if (ntfs_read_record(sb, mft_index, rec) < 0) {
        kfree(rec);
        return;
    }
    uint8_t *fn = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_FILE_NAME, 0);
    if (fn && fn[8] == 0) {
        uint16_t voff = nget16(fn + 0x14);
        uint8_t *v = rec + voff;
        nput64(v + 0x28, size);
        nput64(v + 0x30, size);
        if (is_dir)
            nput32(v + 0x38, NTFS_REC_IS_DIR);
        ntfs_write_record(sb, mft_index, rec);
    }
    kfree(rec);
}


int ntfs_update_file_name(ntfs_sb_t *sb, uint64_t mft_index,
                                 uint64_t parent_ref, const char *name,
                                 int is_dir, uint64_t data_size)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -ENOMEM;
    if (ntfs_read_record(sb, mft_index, rec) < 0) {
        kfree(rec);
        return -EIO;
    }

    uint8_t *fn = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_FILE_NAME, 0);
    if (!fn || fn[8] != 0) {
        kfree(rec);
        return -EIO;
    }

    uint32_t old_len = nget32(fn + 4);

    uint8_t new_attr[1024];
    int new_len = ntfs_build_file_name_attr(new_attr, sizeof(new_attr),
                                            parent_ref, name, is_dir,
                                            data_size);
    if (new_len < 0) {
        kfree(rec);
        return -EINVAL;
    }

    int32_t delta = new_len - (int32_t)old_len;
    uint32_t used = nget32(rec + 0x18);
    if ((int32_t)used + delta > (int32_t)sb->mft_record_size) {
        kfree(rec);
        return -ENOSPC;
    }

    if (delta != 0) {
        uint8_t *after = fn + old_len;
        memmove(fn + new_len, after, used - (uint32_t)(after - rec));
    }
    memcpy(fn, new_attr, (size_t)new_len);
    nput32(rec + 0x18, used + (uint32_t)delta);

    int r = ntfs_write_record(sb, mft_index, rec);
    kfree(rec);
    return r;
}


int ntfs_create_entry(ntfs_vnode_priv_t *dirfp, const char *name,
                             int mode, int is_dir, vnode_t **out)
{
    (void)mode;
    ntfs_sb_t *sb = dirfp->sb;
    int64_t free_idx = ntfs_find_free_record(sb, 12);
    if (free_idx < 0)
        return -ENOSPC;

    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -ENOMEM;
    memset(rec, 0, sb->mft_record_size);

    /* Build a fresh FILE record. */
    memcpy(rec, "FILE", 4);
    uint16_t usa_count = (uint16_t)(sb->mft_record_size / sb->bytes_per_sector + 1);
    nput16(rec + 0x04, 0x30);
    nput16(rec + 0x06, usa_count);
    nput16(rec + 0x10, 1);              /* sequence number */
    nput16(rec + 0x12, 1);              /* hard link count */
    nput16(rec + 0x14, 0x30);           /* first attribute offset */
    nput16(rec + 0x16, NTFS_REC_IN_USE | (is_dir ? NTFS_REC_IS_DIR : 0));
    nput32(rec + 0x1C, sb->mft_record_size);
    nput16(rec + 0x28, 3);              /* next attribute id */
    nput32(rec + 0x2C, (uint32_t)free_idx);
    /* USA value placeholder (0xFFFF); fixup fills real values. */
    nput16(rec + 0x30, 0xFFFF);

    uint32_t off = 0x30;

    /* $STANDARD_INFORMATION (resident, 72-byte value). */
    {
        uint32_t alen = NTFS_ATTR_HEADER_RES + 72;
        memset(rec + off, 0, alen);
        nput32(rec + off + 0x00, NTFS_AT_STD_INFO);
        nput32(rec + off + 0x04, alen);
        rec[off + 8] = 0;
        rec[off + 9] = 0;
        nput16(rec + off + 0x0A, NTFS_ATTR_HEADER_RES);
        nput16(rec + off + 0x0E, 1);
        nput32(rec + off + 0x10, 72);
        nput16(rec + off + 0x14, NTFS_ATTR_HEADER_RES);
        off += alen;
    }

    /* $FILE_NAME. */
    {
        uint8_t fn[1024];
        int fn_len = ntfs_build_file_name_attr(fn, sizeof(fn),
                                               dirfp->mft_index |
                                               ((uint64_t)dirfp->seq << 48),
                                               name, is_dir, 0);
        if (fn_len < 0) {
            kfree(rec);
            return -EINVAL;
        }
        memcpy(rec + off, fn, fn_len);
        off += fn_len;
    }

    if (is_dir) {
        /* $INDEX_ROOT for $I30. */
        uint32_t vlen = 16 + 16 + 16;   /* root + node + terminator */
        uint32_t alen = NTFS_ATTR_HEADER_RES + vlen;
        memset(rec + off, 0, alen);
        nput32(rec + off + 0x00, NTFS_AT_INDEX_ROOT);
        nput32(rec + off + 0x04, alen);
        rec[off + 8] = 0;
        rec[off + 9] = 0;
        nput16(rec + off + 0x0A, NTFS_ATTR_HEADER_RES);
        nput16(rec + off + 0x0E, 3);
        nput32(rec + off + 0x10, vlen);
        nput16(rec + off + 0x14, NTFS_ATTR_HEADER_RES);
        uint8_t *v = rec + off + NTFS_ATTR_HEADER_RES;
        nput32(v + 0x00, NTFS_AT_FILE_NAME);   /* index type */
        nput32(v + 0x04, 1);                   /* collation: file name */
        nput32(v + 0x08, (uint32_t)sb->index_record_size);
        v[0x0C] = (uint8_t)(sb->index_record_size / sb->bytes_per_cluster);
        uint8_t *node = v + 16;
        nput32(node + 0x00, 16);               /* entries offset */
        nput32(node + 0x04, 16);               /* total size */
        nput32(node + 0x08, 16);               /* allocated size */
        nput32(node + 0x0C, 0);                /* flags */
        /* 16-byte terminator entry. */
        nput64(node + 16, 0);
        nput16(node + 16 + 2, 16);
        nput16(node + 16 + 8, NTFS_IDX_ENTRY_LAST);
        off += alen;
    } else {
        /* Empty resident $DATA. */
        uint32_t alen = NTFS_ATTR_HEADER_RES;
        memset(rec + off, 0, alen);
        nput32(rec + off + 0x00, NTFS_AT_DATA);
        nput32(rec + off + 0x04, alen);
        rec[off + 8] = 0;
        rec[off + 9] = 0;
        nput16(rec + off + 0x0A, NTFS_ATTR_HEADER_RES);
        nput16(rec + off + 0x0E, 2);
        nput32(rec + off + 0x10, 0);
        nput16(rec + off + 0x14, NTFS_ATTR_HEADER_RES);
        off += alen;
    }

    /* End marker. */
    nput32(rec + off, NTFS_AT_END);
    nput32(rec + 0x18, off + 4);

    if (ntfs_write_record(sb, (uint64_t)free_idx, rec) < 0) {
        kfree(rec);
        return -EIO;
    }
    kfree(rec);

    /* Insert into the parent directory index. */
    int r = ntfs_index_insert(dirfp, name, (uint64_t)free_idx |
                              ((uint64_t)1 << 48), is_dir, 0);
    if (r < 0) {
        ntfs_free_mft_record(sb, (uint64_t)free_idx);
        return r;
    }

    vnode_t *vn = ntfs_make_vnode(sb, (uint64_t)free_idx, 1, is_dir, NULL);
    if (vn)
        vn->size = 0;
    if (out)
        *out = vn;
    return 0;
}


int ntfs_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out)
{
    (void)mode;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);
    int r = ntfs_create_entry(fp, name, mode, 0, out);
    ntfs_unlock(fp->sb);
    return r;
}


int ntfs_vn_mkdir(vnode_t *dir, const char *name, int mode)
{
    (void)mode;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);
    int r = ntfs_create_entry(fp, name, mode, 1, NULL);
    ntfs_unlock(fp->sb);
    return r;
}


int ntfs_vn_unlink(vnode_t *dir, const char *name)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);

    vnode_t *victim = NULL;
    int lr = ntfs_lookup(dir, name, &victim);
    if (lr < 0) {
        ntfs_unlock(fp->sb);
        return lr;
    }
    ntfs_vnode_priv_t *vp = (ntfs_vnode_priv_t *)victim->fs_data;
    if (vp->is_dir) {
        vnode_put(victim);
        ntfs_unlock(fp->sb);
        return -EISDIR;
    }

    int r = ntfs_index_remove(fp, name);
    if (r == 0) {
        /* Free data clusters and the MFT record. */
        if (ntfs_resolve_data(vp) == 0 && !vp->data_resident && vp->runs) {
            for (uint32_t i = 0; i < vp->run_count; i++)
                ntfs_free_clusters(fp->sb, vp->runs[i].lcn, vp->runs[i].length);
        }
        vfs_drop_time_meta_identity(dir->mnt, vp->mft_index);
        ntfs_free_mft_record(fp->sb, vp->mft_index);
    }
    vnode_put(victim);
    ntfs_unlock(fp->sb);
    return r;
}


int ntfs_vn_rmdir(vnode_t *dir, const char *name)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);

    vnode_t *victim = NULL;
    int lr = ntfs_lookup(dir, name, &victim);
    if (lr < 0) {
        ntfs_unlock(fp->sb);
        return lr;
    }
    ntfs_vnode_priv_t *vp = (ntfs_vnode_priv_t *)victim->fs_data;
    if (!vp->is_dir) {
        vnode_put(victim);
        ntfs_unlock(fp->sb);
        return -ENOTDIR;
    }

    ntfs_dir_entry_t *entries = NULL;
    uint32_t count = 0;
    int empty = ntfs_read_directory(vp, &entries, &count) == 0 && count == 0;
    kfree(entries);
    if (!empty) {
        vnode_put(victim);
        ntfs_unlock(fp->sb);
        return -ENOTEMPTY;
    }

    int r = ntfs_index_remove(fp, name);
    if (r == 0) {
        vfs_drop_time_meta_identity(dir->mnt, vp->mft_index);
        ntfs_free_mft_record(fp->sb, vp->mft_index);
    }
    vnode_put(victim);
    ntfs_unlock(fp->sb);
    return r;
}


int ntfs_vn_rename(vnode_t *old_dir, const char *old_name,
                          vnode_t *new_dir, const char *new_name,
                          unsigned int flags)
{
    if (flags & ~(RENAME_NOREPLACE))
        return -EINVAL;

    ntfs_vnode_priv_t *ofp = (ntfs_vnode_priv_t *)old_dir->fs_data;
    ntfs_vnode_priv_t *nfp = (ntfs_vnode_priv_t *)new_dir->fs_data;
    if (!ofp || !nfp)
        return -EINVAL;
    if (!ofp->is_dir || !nfp->is_dir)
        return -ENOTDIR;
    if (ofp->sb != nfp->sb)
        return -EXDEV;

    ntfs_lock(ofp->sb);

    vnode_t *src = NULL;
    int lr = ntfs_lookup(old_dir, old_name, &src);
    if (lr < 0) {
        ntfs_unlock(ofp->sb);
        return lr;
    }
    ntfs_vnode_priv_t *sp = (ntfs_vnode_priv_t *)src->fs_data;

    if (old_dir == new_dir && strcmp(old_name, new_name) == 0) {
        vnode_put(src);
        ntfs_unlock(ofp->sb);
        return 0;
    }

    vnode_t *tgt = NULL;
    int tgt_exists = (ntfs_lookup(new_dir, new_name, &tgt) == 0);
    if (tgt_exists && flags & RENAME_NOREPLACE) {
        vnode_put(src);
        vnode_put(tgt);
        ntfs_unlock(ofp->sb);
        return -EEXIST;
    }

    /* Cross-type replacement is refused; replacing a directory requires the
     * target to be empty. */
    if (tgt_exists) {
        ntfs_vnode_priv_t *tp = (ntfs_vnode_priv_t *)tgt->fs_data;
        if (sp->is_dir != tp->is_dir) {
            vnode_put(src);
            vnode_put(tgt);
            ntfs_unlock(ofp->sb);
            return sp->is_dir ? -ENOTDIR : -EISDIR;
        }
        if (tp->is_dir) {
            ntfs_dir_entry_t *e = NULL;
            uint32_t c = 0;
            int empty = ntfs_read_directory(tp, &e, &c) == 0 && c == 0;
            kfree(e);
            if (!empty) {
                vnode_put(src);
                vnode_put(tgt);
                ntfs_unlock(ofp->sb);
                return -ENOTEMPTY;
            }
        }
    }

    int r = ntfs_index_remove(ofp, old_name);
    if (r < 0) {
        vnode_put(src);
        if (tgt) vnode_put(tgt);
        ntfs_unlock(ofp->sb);
        return r;
    }

    /* Rewrite the child's FILE_NAME to point at the new parent. */
    uint64_t parent_ref = nfp->mft_index | ((uint64_t)nfp->seq << 48);
    r = ntfs_update_file_name(ofp->sb, sp->mft_index, parent_ref, new_name,
                              sp->is_dir, sp->data_size);
    if (r < 0) {
        /* Restore the old entry so the source name survives. */
        ntfs_index_insert(ofp, old_name, sp->mft_index |
                          ((uint64_t)sp->seq << 48), sp->is_dir, sp->data_size);
        vnode_put(src);
        if (tgt) vnode_put(tgt);
        ntfs_unlock(ofp->sb);
        return r;
    }

    /* Replace the target entry (if any) *after* the new entry is staged so a
     * crash leaves at most one name pointing at the child. */
    if (tgt_exists) {
        ntfs_vnode_priv_t *tp = (ntfs_vnode_priv_t *)tgt->fs_data;
        ntfs_index_remove(nfp, new_name);
        if (tp->is_dir || (!tp->data_resident && tp->runs)) {
            vfs_drop_time_meta_identity(new_dir->mnt, tp->mft_index);
            ntfs_free_mft_record(ofp->sb, tp->mft_index);
        }
    }

    r = ntfs_index_insert(nfp, new_name, sp->mft_index |
                          ((uint64_t)sp->seq << 48), sp->is_dir, sp->data_size);
    if (r < 0) {
        /* Roll back the FILE_NAME rewrite and restore the old entry. */
        uint64_t oparent = ofp->mft_index | ((uint64_t)ofp->seq << 48);
        ntfs_update_file_name(ofp->sb, sp->mft_index, oparent, old_name,
                              sp->is_dir, sp->data_size);
        ntfs_index_insert(ofp, old_name, sp->mft_index |
                          ((uint64_t)sp->seq << 48), sp->is_dir, sp->data_size);
        vnode_put(src);
        if (tgt) vnode_put(tgt);
        ntfs_unlock(ofp->sb);
        return r;
    }

    /* Repoint the cached vnode's parent for ".." resolution. */
    if (src->parent && src->parent != new_dir) {
        vnode_t *old_parent = src->parent;
        vnode_get(new_dir);
        src->parent = new_dir;
        vnode_put(old_parent);
    }

    vnode_put(src);
    if (tgt) vnode_put(tgt);
    ntfs_unlock(ofp->sb);
    return 0;
}


int ntfs_vn_truncate(vnode_t *vn, size_t size)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp || fp->is_dir)
        return -EINVAL;
    ntfs_lock(fp->sb);
    if (ntfs_resolve_data(fp) < 0) {
        ntfs_unlock(fp->sb);
        return -EIO;
    }
    /* Shrink the initialized size; for non-resident data we cannot easily
     * shrink the allocation, so we just drop the size. */
    fp->data_size = size;
    vn->size = size;

    uint8_t *rec = kmalloc(fp->sb->mft_record_size);
    int r = -1;
    if (rec) {
        if (ntfs_read_record(fp->sb, fp->mft_index, rec) == 0) {
            uint8_t *attr = ntfs_find_attr(rec, fp->sb->mft_record_size,
                                           NTFS_AT_DATA, 1);
            if (attr) {
                if (attr[8] == 0) {
                    nput32(attr + 0x10, (uint32_t)size);
                    ntfs_update_file_name_size(fp->sb, fp->mft_index, size, 0);
                } else {
                    nput64(attr + 0x30, size);
                    nput64(attr + 0x38, size);
                    ntfs_update_file_name_size(fp->sb, fp->mft_index, size, 0);
                }
                r = ntfs_write_record(fp->sb, fp->mft_index, rec);
            }
        }
        kfree(rec);
    }
    ntfs_unlock(fp->sb);
    return r;
}

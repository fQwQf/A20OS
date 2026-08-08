#include "fs/fat32.h"
#include "fs/fat32_internal.h"
#include "fs/vfs/stat_perm.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/panic.h"
#include "proc/proc.h"

/* FAT32 vnode operations. */

int fat32_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;

    /* Special entries */
    if (strcmp(name, ".") == 0)  { *out = dir; vnode_get(dir); return 0; }
    if (strcmp(name, "..") == 0) {
        if (dir->parent) {
            *out = dir->parent;
            vnode_get(dir->parent);
            vnode_get(dir);          /* vnode_lookup_path will vnode_put(dir) */
            return 0;
        }
        *out = dir; vnode_get(dir); return 0;
    }

    fat32_lock(p->sb);
    int is_dir; size_t sz; size_t doff;
    uint32_t cluster = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (!cluster) {
        fat32_unlock(p->sb);
        return -ENOENT;
    }

    /* Assign a unique inode number from cluster */
    *out = fat32_make_vnode(p->sb, cluster, sz, is_dir, dir, (uint64_t)cluster);
    if (!*out) {
        fat32_unlock(p->sb);
        return -ENOMEM;
    }
    fat32_unlock(p->sb);
    /* parent ref_count bumped in make_vnode */
    return 0;
}


int fat32_stat(vnode_t *vn, kstat_t *st) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    fat32_lock(p->sb);
    memset(st, 0, sizeof(*st));
    st->st_ino  = vn->ino;
    st->st_size = p->file_size;
    st->st_blksize = 512;
    st->st_blocks  = (p->file_size + 511) / 512;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_nlink = 1;
    fat32_unlock(p->sb);
    return 0;
}


void fat32_release_vn(vnode_t *vn) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    if (p && p->unlinked) {
        /* Inode was unlinked while still referenced: reclaim its data now
         * that the last reference is gone. */
        fat32_sb_t *sb = p->sb;
        p->unlinked = 0;
        fat32_lock(sb);
        vfs_drop_time_meta_identity(vn->mnt, vn->ino);
        fat32_drop_meta(sb, vn->ino);
        fat32_free_cluster_chain(sb, p->first_cluster);
        fat32_unlock(sb);
    }
    if (vn->fs_data) { kfree(vn->fs_data); vn->fs_data = NULL; }
    if (vn->parent && vn->parent != vn) vnode_put(vn->parent);
    kfree(vn);
}


int fat32_vn_mkdir(vnode_t *dir, const char *name, int mode) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;
    fat32_lock(p->sb);

    /* Check existence */
    int is_dir; size_t sz; size_t doff;
    uint32_t existing = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (existing) {
        fat32_unlock(p->sb);
        return -EEXIST;
    }

    /* Allocate cluster for new directory */
    uint32_t new_cluster = fat32_alloc_cluster(p->sb);
    if (!new_cluster) {
        fat32_unlock(p->sb);
        return -ENOSPC;
    }

    fat32_sb_t *sb = p->sb;
    /* Write "." and ".." entries */
    fat32_dirent_t dot;
    memset(&dot, 0, sizeof(dot));
    memset(dot.name, ' ', 11);
    dot.name[0] = '.';
    dot.attr = FAT_ATTR_DIRECTORY;
    dot.fst_clus_hi = (uint16_t)(new_cluster >> 16);
    dot.fst_clus_lo = (uint16_t)(new_cluster & 0xFFFF);
    uint64_t new_base = cluster_byte_offset(sb, new_cluster);
    bcache_write_bytes(sb->bc, new_base, &dot, sizeof(dot));

    memset(&dot, 0, sizeof(dot));
    memset(dot.name, ' ', 11);
    dot.name[0] = '.'; dot.name[1] = '.';
    dot.attr = FAT_ATTR_DIRECTORY;
    dot.fst_clus_hi = (uint16_t)(p->first_cluster >> 16);
    dot.fst_clus_lo = (uint16_t)(p->first_cluster & 0xFFFF);
    bcache_write_bytes(sb->bc, new_base + 32, &dot, sizeof(dot));

    int r = fat32_create_dirents(sb, p->first_cluster, name,
                                 FAT_ATTR_DIRECTORY, new_cluster);
    if (r < 0) {
        fat32_free_cluster_chain(sb, new_cluster);
        fat32_unlock(p->sb);
        return r;
    }

    fat32_meta_t *m = fat32_get_meta(sb, (uint64_t)new_cluster, 1, 1);
    task_t *cur = proc_current();
    if (m) {
        m->mode = S_IFDIR | (mode & 07777);
        if (dir->mode & S_ISGID)
            m->mode |= S_ISGID;
        m->uid = cur ? (uint32_t)cur->cred.fsuid : 0;
        m->gid = ((dir->mode & S_ISGID) ? dir->gid :
                  (cur ? (uint32_t)cur->cred.fsgid : 0));
    }
    fat32_unlock(p->sb);
    return 0;
}


int fat32_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;

    fat32_sb_t *sb = p->sb;
    fat32_lock(sb);
    int is_dir; size_t sz; size_t doff;
    uint32_t existing = fat32_dir_lookup(sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (existing) {
        fat32_unlock(sb);
        return -EEXIST;
    }

    uint32_t new_cluster = fat32_alloc_cluster(sb);
    if (!new_cluster) {
        fat32_unlock(sb);
        return -ENOSPC;
    }

    int r = fat32_create_dirents(sb, p->first_cluster, name,
                                 FAT_ATTR_ARCHIVE, new_cluster);
    if (r < 0) {
        fat32_free_cluster_chain(sb, new_cluster);
        fat32_unlock(sb);
        return r;
    }

    fat32_meta_t *m = fat32_get_meta(sb, (uint64_t)new_cluster, 0, 1);
    task_t *cur = proc_current();
    if (m) {
        m->mode = S_IFREG | (mode & 07777);
        m->uid = cur ? (uint32_t)cur->cred.fsuid : 0;
        m->gid = ((dir->mode & S_ISGID) ? dir->gid :
                  (cur ? (uint32_t)cur->cred.fsgid : 0));
    }

    if (out) {
        *out = fat32_make_vnode(sb, new_cluster, 0, 0, dir, (uint64_t)new_cluster);
        if (!*out) {
            fat32_unlock(sb);
            return -ENOMEM;
        }
    }
    fat32_unlock(sb);
    return 0;
}


int fat32_vn_unlink(vnode_t *dir, const char *name) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;
    fat32_lock(p->sb);

    int is_dir; size_t sz; size_t doff;
    uint32_t cluster = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (!cluster) {
        fat32_unlock(p->sb);
        return -ENOENT;
    }
    if (is_dir) {
        fat32_unlock(p->sb);
        return -EISDIR;
    }

    /* Mark directory entry as deleted */
    fat32_sb_t *sb = p->sb;
    fat32_delete_dirents(sb, p->first_cluster, doff);

    /* If a vnode is still alive (open fd, dcache, ...), keep the cluster
     * chain and let release() free it once the last reference drops. */
    vnode_t *victim = fat32_vcache_remove(sb, (uint64_t)cluster);
    if (victim) {
        fat32_vnode_priv_t *vp = (fat32_vnode_priv_t *)victim->fs_data;
        if (vp)
            vp->unlinked = 1;
        fat32_unlock(p->sb);
        vnode_put(victim);
        return 0;
    }

    vfs_drop_time_meta_identity(dir->mnt, (uint64_t)cluster);
    fat32_drop_meta(sb, (uint64_t)cluster);
    fat32_free_cluster_chain(sb, cluster);
    fat32_unlock(p->sb);
    return 0;
}


int fat32_vn_truncate(vnode_t *vn, size_t size) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    if (!p) return -EINVAL;
    if (p->is_dir) return -EISDIR;
    fat32_lock(p->sb);

    uint32_t old_first_cluster = p->first_cluster;
    if (size == 0) {
        fat32_sb_t *sb = p->sb;
        uint32_t first = p->first_cluster;
        if (first >= 2 && first < FAT32_CLUSTER_END) {
            uint32_t tail = fat_read(sb, first);
            fat_write(sb, first, FAT32_CLUSTER_END_MARK);
            fat32_free_cluster_chain(sb, tail);
        }
    } else {
        uint32_t cluster = p->first_cluster;
        if (cluster < 2 || cluster >= FAT32_CLUSTER_END) {
            cluster = fat32_alloc_cluster(p->sb);
            if (!cluster) {
                fat32_unlock(p->sb);
                return -ENOSPC;
            }
            p->first_cluster = cluster;
        }

        size_t need_clusters = (size + p->sb->bytes_per_cluster - 1) / p->sb->bytes_per_cluster;
        for (size_t i = 1; i < need_clusters; i++) {
            uint32_t next = fat_read(p->sb, cluster);
            if (next >= FAT32_CLUSTER_END) {
                next = fat32_extend_chain(p->sb, cluster);
                if (!next) {
                    fat32_unlock(p->sb);
                    return -ENOSPC;
                }
            }
            cluster = next;
        }
    }

    p->file_size = size;
    vn->size = size;

    if (vn->parent && vn->parent->fs_data) {
        fat32_vnode_priv_t *pp = (fat32_vnode_priv_t *)vn->parent->fs_data;
        size_t off = 0;
        while (1) {
            fat32_dirent_t de;
            int r = read_raw_dirent(p->sb, pp->first_cluster, off, &de);
            if (r <= 0 || de.name[0] == 0x00) break;
            if ((uint8_t)de.name[0] != 0xE5 && de.attr != FAT_ATTR_LFN) {
                uint32_t clus = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
                if (clus == old_first_cluster) {
                    de.fst_clus_hi = (uint16_t)(p->first_cluster >> 16);
                    de.fst_clus_lo = (uint16_t)(p->first_cluster & 0xffff);
                    de.file_size = (uint32_t)size;
                    uint32_t dc = pp->first_cluster;
                    size_t rem = off;
                    while (rem >= p->sb->bytes_per_cluster) {
                        rem -= p->sb->bytes_per_cluster;
                        dc = fat_read(p->sb, dc);
                    }
                    bcache_write_bytes(p->sb->bc, cluster_byte_offset(p->sb, dc) + rem, &de, sizeof(de));
                    break;
                }
            }
            off += sizeof(fat32_dirent_t);
        }
    }

    fat32_unlock(p->sb);
    return 0;
}


int fat32_vn_chmod(vnode_t *vn, int mode) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    fat32_lock(p->sb);
    fat32_meta_t *m = fat32_get_meta(p->sb, vn->ino, p->is_dir, 1);
    if (!m) {
        fat32_unlock(p->sb);
        return -ENOSPC;
    }
    m->mode = (m->mode & S_IFMT) | (mode & 07777);
    vn->mode = m->mode;
    fat32_unlock(p->sb);
    return 0;
}


int fat32_vn_chown(vnode_t *vn, int uid, int gid) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    fat32_lock(p->sb);
    fat32_meta_t *m = fat32_get_meta(p->sb, vn->ino, p->is_dir, 1);
    if (!m) {
        fat32_unlock(p->sb);
        return -ENOSPC;
    }
    if (uid != -1) m->uid = (uint32_t)uid;
    if (gid != -1) m->gid = (uint32_t)gid;
    if (uid != -1 || gid != -1) {
        m->mode &= ~S_ISUID;
        if (m->mode & S_IXGRP)
            m->mode &= ~S_ISGID;
    }
    vn->uid = m->uid;
    vn->gid = m->gid;
    vn->mode = m->mode;
    fat32_unlock(p->sb);
    return 0;
}


int fat32_vn_rmdir(vnode_t *dir, const char *name) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;
    fat32_lock(p->sb);

    int is_dir; size_t sz; size_t doff;
    uint32_t cluster = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (!cluster) {
        fat32_unlock(p->sb);
        return -ENOENT;
    }
    if (!is_dir) {
        fat32_unlock(p->sb);
        return -ENOTDIR;
    }

    fat32_sb_t *sb = p->sb;
    int r = fat32_dir_is_empty(sb, cluster);
    if (r < 0) {
        fat32_unlock(p->sb);
        return r;
    }

    fat32_delete_dirents(sb, p->first_cluster, doff);

    vnode_t *victim = fat32_vcache_remove(sb, (uint64_t)cluster);
    if (victim) {
        fat32_vnode_priv_t *vp = (fat32_vnode_priv_t *)victim->fs_data;
        if (vp)
            vp->unlinked = 1;
        fat32_unlock(p->sb);
        vnode_put(victim);
        return 0;
    }

    vfs_drop_time_meta_identity(dir->mnt, (uint64_t)cluster);
    fat32_drop_meta(sb, (uint64_t)cluster);
    fat32_free_cluster_chain(sb, cluster);
    fat32_unlock(p->sb);
    return 0;
}


int fat32_vn_rename(vnode_t *old_dir, const char *old_name,
                           vnode_t *new_dir, const char *new_name,
                           unsigned int flags) {
    if (flags & ~(RENAME_NOREPLACE))
        return -EINVAL;

    fat32_vnode_priv_t *op = (fat32_vnode_priv_t *)old_dir->fs_data;
    fat32_vnode_priv_t *np = (fat32_vnode_priv_t *)new_dir->fs_data;
    if (!op->is_dir || !np->is_dir) return -ENOTDIR;
    if (op->sb != np->sb) return -EXDEV;

    fat32_sb_t *sb = op->sb;
    fat32_lock(sb);

    int s_is_dir; size_t s_sz; size_t s_doff;
    uint32_t src_cluster = fat32_dir_lookup(sb, op->first_cluster, old_name,
                                            &s_is_dir, &s_sz, &s_doff);
    if (!src_cluster) {
        fat32_unlock(sb);
        return -ENOENT;
    }

    int t_is_dir; size_t t_sz; size_t t_doff;
    uint32_t tgt_cluster = fat32_dir_lookup(sb, np->first_cluster, new_name,
                                            &t_is_dir, &t_sz, &t_doff);

    /* Same inode (rename onto itself) is a no-op. */
    if (tgt_cluster && tgt_cluster == src_cluster) {
        fat32_unlock(sb);
        return 0;
    }

    if (tgt_cluster && flags & RENAME_NOREPLACE) {
        fat32_unlock(sb);
        return -EEXIST;
    }

    /* Replacing a directory with a non-empty one (or with a file) is
     * forbidden; replacing a file with a directory is likewise an error. */
    if (tgt_cluster) {
        if (t_is_dir) {
            if (!s_is_dir) { fat32_unlock(sb); return -EISDIR; }
            int r = fat32_dir_is_empty(sb, tgt_cluster);
            if (r < 0) { fat32_unlock(sb); return r; }
        } else if (s_is_dir) {
            fat32_unlock(sb);
            return -ENOTDIR;
        }
    }

    int r = fat32_create_dirents(sb, np->first_cluster, new_name,
                                 s_is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE,
                                 src_cluster);
    if (r < 0) {
        fat32_unlock(sb);
        return r;
    }

    /* Moving a directory: repoint its ".." entry at the new parent. */
    if (s_is_dir) {
        uint32_t c = src_cluster;
        if (c < 2) c = sb->root_cluster;
        fat32_dirent_t dotdot;
        memset(&dotdot, 0, sizeof(dotdot));
        if (read_raw_dirent(sb, c, 32, &dotdot) > 0) {
            dotdot.fst_clus_hi = (uint16_t)(np->first_cluster >> 16);
            dotdot.fst_clus_lo = (uint16_t)np->first_cluster;
            bcache_write_bytes(sb->bc, cluster_byte_offset(sb, c) + 32,
                               &dotdot, sizeof(dotdot));
        }
    }

    /* Replace the old target (if any) *after* the new entry is in place so
     * a crash leaves at most one name pointing at the source. */
    if (tgt_cluster) {
        fat32_delete_dirents(sb, np->first_cluster, t_doff);
        vnode_t *tvictim = fat32_vcache_remove(sb, (uint64_t)tgt_cluster);
        if (tvictim) {
            fat32_vnode_priv_t *vp = (fat32_vnode_priv_t *)tvictim->fs_data;
            if (vp)
                vp->unlinked = 1;
            vnode_put(tvictim);
        } else {
            vfs_drop_time_meta_identity(new_dir->mnt,
                                        (uint64_t)tgt_cluster);
            fat32_drop_meta(sb, (uint64_t)tgt_cluster);
            fat32_free_cluster_chain(sb, tgt_cluster);
        }
    }

    /* Remove the old source name. */
    fat32_delete_dirents(sb, op->first_cluster, s_doff);

    /* Repoint the cached vnode's parent so ".." resolution and dcache
     * lookups follow the new location.  Defer the old-parent put until the
     * lock is released, mirroring the ext4 rename contract. */
    vnode_t *old_parent = NULL;
    vnode_t *moved = fat32_vcache_find(sb, (uint64_t)src_cluster);
    if (moved && moved->parent && moved->parent != new_dir) {
        old_parent = moved->parent;
        vnode_get(new_dir);
        moved->parent = new_dir;
    }

    fat32_unlock(sb);
    if (old_parent)
        vnode_put(old_parent);
    return 0;
}


int fat32_vn_statfs(vnode_t *vn, kstatfs_t *st)
{
    if (!vn || !vn->fs_data || !st)
        return -EINVAL;
    fat32_sb_t *sb = ((fat32_vnode_priv_t *)vn->fs_data)->sb;
    uint64_t free_clusters = 0;

    fat32_lock(sb);
    for (uint32_t cluster = 2; cluster < sb->total_clusters + 2; cluster++) {
        if (fat_read(sb, cluster) == FAT32_CLUSTER_FREE)
            free_clusters++;
    }
    fat32_unlock(sb);

    st->f_type = 0x4d44;
    st->f_bsize = sb->bytes_per_cluster;
    st->f_frsize = sb->bytes_per_cluster;
    st->f_blocks = sb->total_clusters;
    st->f_bfree = free_clusters;
    st->f_bavail = free_clusters;
    st->f_files = 0;
    st->f_ffree = 0;
    st->f_namelen = 255;
    return 0;
}


int fat32_vn_readpage(vnode_t *vn, uint64_t index,
                             void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)vn->fs_data;
    if (fp->is_dir)
        return -EISDIR;

    memset(data, 0, len);
    uint64_t off = index * PAGE_SIZE;
    if (off >= fp->file_size)
        return 0;
    size_t n = fp->file_size - (size_t)off;
    if (n > len)
        n = len;

    fat32_fctx_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.sb = fp->sb;
    fc.first_cluster = fp->first_cluster;
    fc.file_size = fp->file_size;
    fc.file_off = (size_t)off;

    vfile_t local;
    memset(&local, 0, sizeof(local));
    local.vnode = vn;
    local.flags = O_RDONLY;
    local.offset = (size_t)off;
    local.priv = &fc;
    int r = fat32_fread(&local, (char *)data, n);
    return r;
}


int fat32_vn_writepage(vnode_t *vn, uint64_t index,
                              const void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)vn->fs_data;
    if (fp->is_dir)
        return -EISDIR;

    uint64_t off = index * PAGE_SIZE;
    if (off >= fp->file_size)
        return 0;
    size_t n = fp->file_size - (size_t)off;
    if (n > len)
        n = len;

    fat32_fctx_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.sb = fp->sb;
    fc.first_cluster = fp->first_cluster;
    fc.file_size = fp->file_size;
    fc.is_dir = fp->is_dir;
    fc.file_off = (size_t)off;

    vfile_t vf;
    memset(&vf, 0, sizeof(vf));
    vf.vnode = vn;
    vf.flags = O_RDWR;
    vf.offset = (size_t)off;
    vf.ops = &g_fat32_fops;
    vf.priv = &fc;

    fat32_lock(fp->sb);
    int r = fat32_fwrite_unlocked(&vf, (const char *)data, n);
    fat32_unlock(fp->sb);
    if (r < 0)
        return r;
    return (size_t)r == n ? 0 : -EIO;
}

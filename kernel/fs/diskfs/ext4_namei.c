#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/vfs/stat_perm.h"
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

/* ext4 namei: directory entries and vnode operations. */

int ext4_dir_entry_check(const ext4_dir_entry_t *de, uint32_t off,
                         uint32_t block_size, uint16_t *actual_len)
{
    if (off > block_size || block_size - off < 8)
        return -EIO;
    uint16_t rec_len = de->rec_len;
    if (rec_len < 8 || (rec_len & 3U) || rec_len > block_size - off ||
        de->name_len > rec_len - 8U)
        return -EIO;
    uint16_t actual = (uint16_t)((8U + de->name_len + 3U) & ~3U);
    if (actual > rec_len)
        return -EIO;
    if (actual_len)
        *actual_len = actual;
    return 0;
}

static int ext4_dir_write_block(ext4_sb_info_t *sb, uint64_t physical,
                                const void *block)
{
    return bcache_write_bytes(sb->bc, physical * sb->block_size, block,
                              sb->block_size) < 0 ? -EIO : 0;
}

int ext4_dir_find(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz,
                          const char *name, uint32_t *out_ino, uint8_t *out_ft) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                if (out_ino) *out_ino = de->inode;
                if (out_ft) *out_ft = de->file_type;
                kfree(blk); return 0;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    return -ENOENT;
}


int ext4_dir_add(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t *dsz,
                         const char *name, uint32_t ino, uint8_t ft) {
    uint32_t bs = sb->block_size;
    size_t nl = strlen(name);
    if (nl == 0) return -EINVAL;
    if (nl > 255) return -ENAMETOOLONG;
    uint16_t need = (uint16_t)((8 + nl + 3) & ~3);
    if (need > bs) return -ENAMETOOLONG;
    uint32_t nb = (*dsz + bs - 1) / bs;

    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            uint16_t actual;
            if (ext4_dir_entry_check(de, off, bs, &actual) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->inode == 0 && de->rec_len >= need) {
                uint16_t old = de->rec_len;
                uint16_t left = old - need;
                de->inode = ino; de->name_len = (uint8_t)nl; de->file_type = ft;
                memcpy(de->name, name, nl);
                /* A free tail is itself a directory entry and therefore
                 * needs a complete eight-byte header.  A four-byte tail is
                 * absorbed by the new entry instead of being overwritten. */
                if (left >= 8) {
                    de->rec_len = need;
                    ext4_dir_entry_t *nx = (ext4_dir_entry_t *)(blk + off + need);
                    nx->inode = 0; nx->rec_len = left; nx->name_len = 0; nx->file_type = 0;
                } else {
                    de->rec_len = old;
                }
                int wr = ext4_dir_write_block(sb, p, blk);
                kfree(blk); return wr;
            }
            uint16_t slack = de->rec_len - actual;
            if (slack >= need) {
                uint16_t old = de->rec_len; de->rec_len = actual;
                ext4_dir_entry_t *nx = (ext4_dir_entry_t *)(blk + off + actual);
                nx->inode = ino; nx->rec_len = old - actual;
                nx->name_len = (uint8_t)nl; nx->file_type = ft;
                memcpy(nx->name, name, nl);
                int wr = ext4_dir_write_block(sb, p, blk);
                kfree(blk); return wr;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    uint64_t nb_blk = ext4_alloc_block(sb); if (!nb_blk) return -ENOSPC;
    int gr = ext4_block_grow(sb, di, nb, nb_blk);
    if (gr < 0) { ext4_free_block(sb, nb_blk); return gr; }
    char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
    memset(blk, 0, bs);
    ext4_dir_entry_t *de = (ext4_dir_entry_t *)blk;
    de->inode = ino; de->name_len = (uint8_t)nl; de->file_type = ft;
    memcpy(de->name, name, nl);
    if (bs - need >= 8) {
        de->rec_len = need;
        ext4_dir_entry_t *tail = (ext4_dir_entry_t *)(blk + need);
        tail->inode = 0; tail->rec_len = (uint16_t)(bs - need); tail->name_len = 0;
    } else de->rec_len = (uint16_t)bs;
    int wr = ext4_dir_write_block(sb, nb_blk, blk);
    kfree(blk);
    if (wr < 0) return wr;
    *dsz += bs;
    return 0;
}


int ext4_dir_remove(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz,
                            const char *name) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0, prev = 0; int hp = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                if (hp) ((ext4_dir_entry_t *)(blk + prev))->rec_len += de->rec_len;
                else de->inode = 0;
                int wr = ext4_dir_write_block(sb, p, blk);
                kfree(blk); return wr;
            }
            prev = off; hp = 1; off += de->rec_len;
        }
        kfree(blk);
    }
    return -ENOENT;
}


int ext4_dir_update_entry(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t *dsz,
                                  const char *name, uint32_t ino, uint8_t ft) {
    uint32_t bs = sb->block_size;
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    uint32_t nb = (*dsz + bs - 1) / bs;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b);
        if (!p) continue;
        char *blk = (char *)kmalloc(bs);
        if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                de->inode = ino;
                de->file_type = ft;
                int wr = ext4_dir_write_block(sb, p, blk);
                kfree(blk);
                return wr;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    return -ENOENT;
}


int ext4_inode_remove(ext4_sb_info_t *sb, mount_t *mnt,
                               uint32_t dir_ino __attribute__((unused)),
                               ext4_inode_t *di, const char *name, uint32_t ino,
                               vnode_t **deferred_put) {
    int r = ext4_dir_remove(sb, di, ext4_inode_size(di), name);
    if (r < 0) return r;

    /* Drop one link.  Hard-linked files survive until the last link is gone. */
    ext4_inode_t victim;
    r = ext4_read_inode(sb, ino, &victim);
    if (r < 0) return r;
    if (victim.i_links_count > 1) {
        victim.i_links_count--;
        uint64_t now[2];
        timekeeping_get_realtime(now);
        victim.i_ctime = (uint32_t)now[0];
        ext4_write_inode(sb, ino, &victim);
        return 0;
    }

    /* If a vnode is still alive (open fd, dcache, ...), keep the inode and
     * its blocks; ext4_release_vn() reclaims them when the last reference
     * drops.  The inode number stays allocated until then, so it cannot be
     * recycled while data is still reachable. */
    vnode_t *live = ext4_vnode_cache_remove(sb, ino);
    if (live) {
        ext4_vnode_priv_t *vp = (ext4_vnode_priv_t *)live->fs_data;
        if (vp)
            vp->unlinked = 1;
        if (deferred_put)
            *deferred_put = live;
        return 0;
    }

    ext4_block_truncate(sb, &victim);
    memset(&victim, 0, sizeof(victim));
    victim.i_dtime = 1;
    ext4_write_inode(sb, ino, &victim);
    vfs_drop_time_meta_identity(mnt, ino);
    ext4_free_inode(sb, ino);
    return 0;
}


int ext4_lookup_unlocked(vnode_t *dir, const char *name, vnode_t **out) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

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

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name, &child_ino, &ft);
    if (r < 0) return r;

    ext4_inode_t ci;
    if (ext4_read_inode(p->sb, child_ino, &ci) < 0) return -EIO;

    int type = VFS_FT_REGULAR;
    if (ft == EXT4_FT_DIR) type = VFS_FT_DIR;
    else if (ft == EXT4_FT_SYMLINK) type = VFS_FT_SYMLINK;
    *out = ext4_make_vnode(p->sb, child_ino, ext4_inode_size(&ci), type, dir);
    if (!*out) return -ENOMEM;
    /* parent ref_count bumped in make_vnode */
    return 0;
}


int ext4_stat(vnode_t *vn, kstat_t *st) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    uint64_t sz = p->file_size;
    ext4_inode_t dinode;
    memset(&dinode, 0, sizeof(dinode));
    if (ext4_read_inode(p->sb, p->inode_num, &dinode) == 0) {
        sz = ext4_inode_size(&dinode);
        p->file_size = sz;
        vn->size = sz;
    }
    memset(st, 0, sizeof(*st));
    st->st_ino  = vn->ino;
    st->st_size = sz;
    st->st_blksize = p->sb->block_size;
    st->st_blocks  = (sz + 511) / 512;
    st->st_mode = dinode.i_mode ? dinode.i_mode : vn->mode;
    uint32_t vtype_mode = S_IFREG;
    if (vn->type == VFS_FT_DIR)
        vtype_mode = S_IFDIR;
    else if (vn->type == VFS_FT_SYMLINK)
        vtype_mode = S_IFLNK;
    if ((st->st_mode & S_IFMT) != vtype_mode)
        st->st_mode = (st->st_mode & ~S_IFMT) | vtype_mode;
    st->st_uid = dinode.i_uid;
    st->st_gid = dinode.i_gid;
    st->st_nlink = dinode.i_links_count ? dinode.i_links_count : 1;
    /* Keep metadata fingerprints stable by exposing the on-disk inode times. */
    st->st_atime = dinode.i_atime;
    st->st_mtime = dinode.i_mtime;
    st->st_ctime = dinode.i_ctime;
    return 0;
}


void ext4_release_vn(vnode_t *vn) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (p && p->unlinked) {
        /* Inode was unlinked while still referenced: reclaim its blocks
         * and the inode itself now that the last reference is gone. */
        ext4_sb_info_t *sb = p->sb;
        p->unlinked = 0;
        mutex_lock(&sb->metadata_lock);
        ext4_inode_t victim;
        if (ext4_read_inode(sb, p->inode_num, &victim) == 0) {
            ext4_block_truncate(sb, &victim);
            memset(&victim, 0, sizeof(victim));
            victim.i_dtime = 1;
            ext4_write_inode(sb, p->inode_num, &victim);
        }
        vfs_drop_time_meta_identity(vn->mnt, p->inode_num);
        ext4_free_inode(sb, p->inode_num);
        mutex_unlock(&sb->metadata_lock);
    }
    if (vn->fs_data) { kfree(vn->fs_data); vn->fs_data = NULL; }
    if (vn->parent && vn->parent != vn) vnode_put(vn->parent);
    kfree(vn);
}


int ext4_vn_create_unlocked(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    /* Check if already exists */
    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0)
        return -EEXIST;
    if (lookup != -ENOENT)
        return lookup;

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    /* Initialize new inode */
    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    task_t *cur = proc_current();
    ni.i_mode = S_IFREG | (mode & 07777);
    ni.i_uid = cur ? (uint16_t)cur->cred.fsuid : 0;
    ni.i_gid = (di.i_mode & S_ISGID) ? di.i_gid : (cur ? (uint16_t)cur->cred.fsgid : 0);
    ni.i_links_count = 1;
    if (ext4_write_inode(p->sb, new_ino, &ni) < 0) {
        ext4_free_inode(p->sb, new_ino);
        return -EIO;
    }

    /* Add dir entry */
    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, &dsz, name, new_ino, EXT4_FT_REG_FILE);
    if (r < 0) {
        ext4_free_inode(p->sb, new_ino);
        return r;
    }

    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        if (ext4_write_inode(p->sb, p->inode_num, &di) < 0)
            return -EIO;
        p->file_size = dsz;
    }

    if (out) {
        *out = ext4_make_vnode(p->sb, new_ino, 0, VFS_FT_REGULAR, dir);
        if (!*out) return -ENOMEM;
    }
    return 0;
}


int ext4_vn_mkdir_unlocked(vnode_t *dir, const char *name, int mode) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0)
        return -EEXIST;
    if (lookup != -ENOENT)
        return lookup;

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    /* Allocate a block for the new directory */
    uint64_t blk = ext4_alloc_block(p->sb);
    if (!blk) { ext4_free_inode(p->sb, new_ino); return -ENOSPC; }

    /* Initialize new directory inode */
    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    task_t *cur = proc_current();
    ni.i_mode = S_IFDIR | (mode & 07777);
    if (di.i_mode & S_ISGID)
        ni.i_mode |= S_ISGID;
    ni.i_uid = cur ? (uint16_t)cur->cred.fsuid : 0;
    ni.i_gid = (di.i_mode & S_ISGID) ? di.i_gid : (cur ? (uint16_t)cur->cred.fsgid : 0);
    ext4_inode_set_size(&ni, p->sb->block_size);
    ni.i_links_count = 2; /* . and .. */
    ni.i_flags |= EXT4_EXTENTS_FL;

    /* Write extent for the one block */
    uint8_t *raw = (uint8_t *)&ni + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    hdr.eh_magic = EXT4_EXT_MAGIC; hdr.eh_entries = 1;
    hdr.eh_max = 4; hdr.eh_depth = 0; hdr.eh_generation = 0;
    memcpy(raw, &hdr, sizeof(hdr));
    ext4_extent_t ext;
    ext.ee_block = 0; ext.ee_len = 1;
    ext.ee_start_hi = (uint16_t)(blk >> 32);
    ext.ee_start_lo = (uint32_t)(blk & 0xFFFFFFFF);
    memcpy(raw + sizeof(hdr), &ext, sizeof(ext));

    if (ext4_write_inode(p->sb, new_ino, &ni) < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return -EIO;
    }

    /* Write "." and ".." entries in the new directory block */
    char *buf = (char *)kmalloc(p->sb->block_size);
    if (!buf) { ext4_free_block(p->sb, blk); ext4_free_inode(p->sb, new_ino); return -ENOMEM; }
    memset(buf, 0, p->sb->block_size);

    /* "." entry */
    ext4_dir_entry_t *dot = (ext4_dir_entry_t *)buf;
    dot->inode = new_ino;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->rec_len = 12;
    dot->name[0] = '.';

    /* ".." entry */
    ext4_dir_entry_t *dotdot = (ext4_dir_entry_t *)(buf + 12);
    dotdot->inode = p->inode_num;
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    dotdot->rec_len = (uint16_t)(p->sb->block_size - 12);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';

    int wr = ext4_dir_write_block(p->sb, blk, buf);
    kfree(buf);
    if (wr < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return wr;
    }

    /* Add entry in parent directory */
    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, &dsz, name, new_ino, EXT4_FT_DIR);
    if (r < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return r;
    }

    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        if (ext4_write_inode(p->sb, p->inode_num, &di) < 0)
            return -EIO;
        p->file_size = dsz;
    }
    return 0;
}


int ext4_vn_link_unlocked(vnode_t *dir, const char *name, vnode_t *target) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;
    if (!target || !target->fs_data) return -EINVAL;
    if (target->type == VFS_FT_DIR) return -EPERM;

    ext4_vnode_priv_t *tp = (ext4_vnode_priv_t *)target->fs_data;
    if (tp->sb != p->sb) return -EXDEV;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    /* Linux refuses to hard-link across different mounts; same sb is enough
     * here, but also refuse to re-link to the very same directory entry. */
    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0) {
        if (existing_ino == tp->inode_num) return -EEXIST;
        return -EEXIST;
    }
    if (lookup != -ENOENT)
        return lookup;

    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, tp->inode_num, &inode) < 0) return -EIO;

    inode.i_links_count++;
    uint64_t now[2];
    timekeeping_get_realtime(now);
    inode.i_ctime = (uint32_t)now[0];
    if (ext4_write_inode(p->sb, tp->inode_num, &inode) < 0) return -EIO;

    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, &dsz, name, tp->inode_num, EXT4_FT_REG_FILE);
    if (r < 0) {
        /* Roll back the link count on failure. */
        inode.i_links_count--;
        ext4_write_inode(p->sb, tp->inode_num, &inode);
        return r;
    }
    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }
    return 0;
}


int ext4_vn_link(vnode_t *dir, const char *name, vnode_t *target) {
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_link_unlocked(dir, name, target);
    mutex_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_unlink_unlocked(vnode_t *dir, const char *name, vnode_t **deferred_put) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name, &child_ino, &ft);
    if (r < 0) return r;
    if (ft == EXT4_FT_DIR) return -EISDIR;

    return ext4_inode_remove(p->sb, dir->mnt, p->inode_num, &di, name,
                             child_ino, deferred_put);
}


int ext4_dir_empty(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->inode) {
                if (de->name_len == 1 && de->name[0] == '.') { off += de->rec_len; continue; }
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') { off += de->rec_len; continue; }
                kfree(blk); return -ENOTEMPTY;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    return 0;
}


int ext4_vn_rmdir_unlocked(vnode_t *dir, const char *name, vnode_t **deferred_put) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name, &child_ino, &ft);
    if (r < 0) return r;
    if (ft != EXT4_FT_DIR) return -ENOTDIR;

    ext4_inode_t cdi;
    if (ext4_read_inode(p->sb, child_ino, &cdi) < 0) return -EIO;
    r = ext4_dir_empty(p->sb, &cdi, ext4_inode_size(&cdi));
    if (r < 0) return r;

    return ext4_inode_remove(p->sb, dir->mnt, p->inode_num, &di, name,
                             child_ino, deferred_put);
}


int ext4_vn_rename_unlocked(vnode_t *old_dir, const char *old_name,
                                   vnode_t *new_dir, const char *new_name,
                                   unsigned int flags, vnode_t **deferred_put,
                                   vnode_t **deferred_parent_put) {
    ext4_vnode_priv_t *op = (ext4_vnode_priv_t *)old_dir->fs_data;
    ext4_vnode_priv_t *np = (ext4_vnode_priv_t *)new_dir->fs_data;
    if (op->type != VFS_FT_DIR || np->type != VFS_FT_DIR) return -ENOTDIR;
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;
    if (op->sb == np->sb && op->inode_num == np->inode_num &&
        strcmp(old_name, new_name) == 0)
        return 0;

    ext4_inode_t odi, ndi;
    if (ext4_read_inode(op->sb, op->inode_num, &odi) < 0) return -EIO;
    if (ext4_read_inode(np->sb, np->inode_num, &ndi) < 0) return -EIO;

    /* Find source */
    uint32_t src_ino; uint8_t src_ft;
    int r = ext4_dir_find(op->sb, &odi, ext4_inode_size(&odi), old_name, &src_ino,
                          &src_ft);
    if (r < 0) return r;

    /* Find target */
    uint32_t tgt_ino = 0; uint8_t tgt_ft = 0;
    int tgt_lookup = ext4_dir_find(np->sb, &ndi, ext4_inode_size(&ndi),
                                   new_name, &tgt_ino, &tgt_ft);
    if (tgt_lookup != 0 && tgt_lookup != -ENOENT)
        return tgt_lookup;
    int tgt_exists = tgt_lookup == 0;

    if (flags & RENAME_NOREPLACE) {
        if (tgt_exists) return -EEXIST;
    }

    if (flags & RENAME_EXCHANGE) {
        if (!tgt_exists) return -ENOENT;
        if (old_dir == new_dir && strcmp(old_name, new_name) == 0) return 0;

        uint64_t odsz = ext4_inode_size(&odi);
        uint64_t ndsz = ext4_inode_size(&ndi);

        r = ext4_dir_update_entry(op->sb, &odi, &odsz, old_name, tgt_ino, tgt_ft);
        if (r < 0) return r;
        r = ext4_dir_update_entry(np->sb, &ndi, &ndsz, new_name, src_ino, src_ft);
        if (r < 0) {
            ext4_dir_update_entry(op->sb, &odi, &odsz, old_name, src_ino, src_ft);
            return r;
        }
        if (odsz != ext4_inode_size(&odi)) {
            ext4_inode_set_size(&odi, odsz);
            ext4_write_inode(op->sb, op->inode_num, &odi);
            op->file_size = odsz;
        }
        if (ndsz != ext4_inode_size(&ndi)) {
            ext4_inode_set_size(&ndi, ndsz);
            ext4_write_inode(np->sb, np->inode_num, &ndi);
            np->file_size = ndsz;
        }
        return 0;
    }

    /* Check if target exists — if so, remove it */
    if (tgt_exists) {
        r = ext4_inode_remove(np->sb, new_dir->mnt, np->inode_num, &ndi,
                              new_name, tgt_ino, deferred_put);
        if (r < 0) return r;
        /* Re-read ndi after modification */
        if (ext4_read_inode(np->sb, np->inode_num, &ndi) < 0) return -EIO;
    }

    /* Add new entry in target dir */
    uint64_t ndsz = ext4_inode_size(&ndi);
    r = ext4_dir_add(np->sb, &ndi, &ndsz, new_name, src_ino, src_ft);
    if (r < 0) return r;
    if (ndsz != ext4_inode_size(&ndi)) {
        ext4_inode_set_size(&ndi, ndsz);
        ext4_write_inode(np->sb, np->inode_num, &ndi);
        np->file_size = ndsz;
    }

    if (ext4_read_inode(op->sb, op->inode_num, &odi) < 0)
        return -EIO;
    r = ext4_dir_remove(op->sb, &odi, ext4_inode_size(&odi), old_name);
    if (r < 0) {
        /* Attempt rollback: remove the new entry */
        ext4_dir_remove(np->sb, &ndi, ndsz, new_name);
        return r;
    }

    /* The moved inode keeps its single cached vnode; repoint its parent so
     * that ".." resolution follows the new location.  The old parent
     * reference is dropped by the caller after the lock is released. */
    vnode_t *moved = ext4_vnode_cache_lookup(op->sb, src_ino);
    if (moved && moved->parent && moved->parent != new_dir) {
        if (deferred_parent_put)
            *deferred_parent_put = moved->parent;
        vnode_get(new_dir);
        moved->parent = new_dir;
    }
    if (moved)
        vnode_put(moved);
    return 0;
}


int ext4_readlink(vnode_t *vn, char *buf, size_t sz) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (vn->type != VFS_FT_SYMLINK) return -EINVAL;
    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    size_t len = p->file_size;
    if (len > 60) len = 60; /* fast symlink limit */
    if (len >= sz) len = sz - 1;
    if (len > 0) {
        const char *target = (const char *)inode.i_block.i_data.i_block;
        memcpy(buf, target, len);
    }
    buf[len] = '\0';
    return (int)len;
}


int ext4_vn_symlink_unlocked(vnode_t *dir, const char *name,
                                    const char *target) {    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0)
        return -EEXIST;
    if (lookup != -ENOENT)
        return lookup;

    size_t tlen = strlen(target);
    if (tlen > 60) return -ENAMETOOLONG; /* fast symlink only */

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = S_IFLNK | 0777;
    task_t *cur = proc_current();
    ni.i_uid = cur ? (uint16_t)cur->cred.fsuid : 0;
    ni.i_gid = (di.i_mode & S_ISGID) ? di.i_gid : (cur ? (uint16_t)cur->cred.fsgid : 0);
    ni.i_links_count = 1;
    ext4_inode_set_size(&ni, tlen);
    memcpy(ni.i_block.i_data.i_block, target, tlen);
    ext4_write_inode(p->sb, new_ino, &ni);

    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, &dsz, name, new_ino, EXT4_FT_SYMLINK);
    if (r < 0) {
        ext4_free_inode(p->sb, new_ino);
        return r;
    }
    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }
    return 0;
}


int ext4_vn_truncate_unlocked(vnode_t *vn, size_t size) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (!p) return -EINVAL;
    if (vn->type == VFS_FT_DIR) return -EISDIR;

    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;

    uint64_t old_size = ext4_inode_size(&inode);
    if (size == 0) {
        ext4_block_truncate(p->sb, &inode);
    } else if (size < old_size) {
        /* Reclaim blocks beyond the new EOF instead of leaking them.  The
         * first logical block to drop is the one holding the new EOF. */
        uint32_t bs = p->sb->block_size;
        uint32_t lblk = (uint32_t)((size + bs - 1) / bs);
        if (lblk < (old_size + bs - 1) / bs)
            ext4_block_truncate_at(p->sb, &inode, lblk);
    }

    ext4_inode_set_size(&inode, size);
    ext4_write_inode(p->sb, p->inode_num, &inode);
    p->file_size = size;
    vn->size = size;
    return 0;
}


int ext4_vn_chmod(vnode_t *vn, int mode) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    inode.i_mode = (inode.i_mode & S_IFMT) | (mode & 07777);
    if (ext4_write_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    vn->mode = inode.i_mode;
    return 0;
}


int ext4_vn_chown(vnode_t *vn, int uid, int gid) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    if (uid != -1) inode.i_uid = (uint16_t)uid;
    if (gid != -1) inode.i_gid = (uint16_t)gid;
    if (uid != -1 || gid != -1) {
        inode.i_mode &= ~S_ISUID;
        if (inode.i_mode & S_IXGRP)
            inode.i_mode &= ~S_ISGID;
    }
    if (ext4_write_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    vn->uid = inode.i_uid;
    vn->gid = inode.i_gid;
    vn->mode = inode.i_mode;
    return 0;
}


int ext4_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_lookup_unlocked(dir, name, out);
    mutex_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_create(vnode_t *dir, const char *name, int mode,
                          vnode_t **out)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_create_unlocked(dir, name, mode, out);
    mutex_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_mkdir(vnode_t *dir, const char *name, int mode)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_mkdir_unlocked(dir, name, mode);
    mutex_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_unlink(vnode_t *dir, const char *name)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    vnode_t *deferred = NULL;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_unlink_unlocked(dir, name, &deferred);
    mutex_unlock(&sb->metadata_lock);
    if (deferred)
        vnode_put(deferred);
    return r;
}


int ext4_vn_rmdir(vnode_t *dir, const char *name)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    vnode_t *deferred = NULL;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_rmdir_unlocked(dir, name, &deferred);
    mutex_unlock(&sb->metadata_lock);
    if (deferred)
        vnode_put(deferred);
    return r;
}


int ext4_vn_rename(vnode_t *old_dir, const char *old_name,
                          vnode_t *new_dir, const char *new_name,
                          unsigned int flags)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)old_dir->fs_data)->sb;
    ext4_sb_info_t *new_sb = ((ext4_vnode_priv_t *)new_dir->fs_data)->sb;
    if (sb != new_sb)
        return -EXDEV;
    mutex_lock(&sb->metadata_lock);
    vnode_t *deferred = NULL;
    vnode_t *deferred_parent = NULL;
    int r = ext4_vn_rename_unlocked(old_dir, old_name, new_dir, new_name,
                                    flags, &deferred, &deferred_parent);
    mutex_unlock(&sb->metadata_lock);
    if (deferred)
        vnode_put(deferred);
    if (deferred_parent)
        vnode_put(deferred_parent);
    return r;
}


int ext4_vn_symlink(vnode_t *dir, const char *name,
                           const char *target)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_symlink_unlocked(dir, name, target);
    mutex_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_truncate(vnode_t *vn, size_t size)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)vn->fs_data)->sb;
    mutex_lock(&sb->metadata_lock);
    int r = ext4_vn_truncate_unlocked(vn, size);
    mutex_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_statfs(vnode_t *vn, kstatfs_t *st)
{
    if (!vn || !vn->fs_data || !st)
        return -EINVAL;
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)vn->fs_data)->sb;
    uint64_t free_blocks = 0;
    uint64_t free_inodes = 0;

    mutex_lock(&sb->alloc_lock);
    for (uint32_t g = 0; g < sb->groups_count; g++) {
        free_blocks +=
            (uint64_t)sb->group_descs[g].bg_free_blocks_count_lo |
            ((uint64_t)sb->group_descs[g].bg_free_blocks_count_hi << 16);
        free_inodes +=
            (uint64_t)sb->group_descs[g].bg_free_inodes_count_lo |
            ((uint64_t)sb->group_descs[g].bg_free_inodes_count_hi << 16);
    }
    mutex_unlock(&sb->alloc_lock);

    st->f_type = EXT4_DISK_MAGIC;
    st->f_bsize = sb->block_size;
    st->f_frsize = sb->block_size;
    st->f_blocks = sb->blocks_count;
    st->f_bfree = free_blocks;
    st->f_bavail = free_blocks > sb->reserved_blocks_count
                       ? free_blocks - sb->reserved_blocks_count : 0;
    st->f_files = sb->inodes_count;
    st->f_ffree = free_inodes;
    st->f_namelen = 255;
    return 0;
}

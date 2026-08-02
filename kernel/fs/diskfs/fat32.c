/*
 * A20OS — FAT32 Filesystem Driver
 *
 * Supports:
 *   - Reading/writing regular files
 *   - Directory traversal (8.3 + LFN)
 *   - mkdir, unlink, rename
 *   - FAT cluster chain management
 *
 * Uses bcache_read_bytes / bcache_write_bytes for disk I/O.
 * Integrates with VFS layer via vnode_ops / vfile_ops.
 */

#include "fs/fat32.h"
#include "fs/fat32_internal.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/consts.h"
#include "core/defs.h"
#include "proc/proc.h"

/* ============================================================
 * Low-level cluster/FAT helpers
 * ============================================================ */

uint64_t cluster_to_lba(fat32_sb_t *sb, uint32_t cluster) {
    /* Cluster 2 = first data cluster */
    return (uint64_t)(sb->first_data_sector +
                      (cluster - 2) * sb->sectors_per_cluster);
}

uint64_t cluster_byte_offset(fat32_sb_t *sb, uint32_t cluster) {
    return cluster_to_lba(sb, cluster) * FAT32_SECTOR_SIZE;
}

uint64_t fat_entry_offset(fat32_sb_t *sb, uint32_t cluster) {
    /* Each FAT32 entry is 4 bytes */
    return (uint64_t)(sb->first_fat_sector * FAT32_SECTOR_SIZE + cluster * 4);
}

/* Read the FAT entry for a cluster */
uint32_t fat_read(fat32_sb_t *sb, uint32_t cluster) {
    uint32_t val;
    uint64_t off = fat_entry_offset(sb, cluster);
    bcache_read_bytes(sb->bc, off, &val, 4);
    return val & 0x0FFFFFFF; /* mask upper nibble */
}

/* Write the FAT entry for a cluster */
void fat_write(fat32_sb_t *sb, uint32_t cluster, uint32_t next) {
    /* Read-modify-write to preserve top nibble */
    uint32_t val;
    uint64_t off = fat_entry_offset(sb, cluster);
    bcache_read_bytes(sb->bc, off, &val, 4);
    val = (val & 0xF0000000) | (next & 0x0FFFFFFF);
    bcache_write_bytes(sb->bc, off, &val, 4);
}

/* Follow cluster chain, reading N bytes at file offset */
int fat32_chain_read(fat32_sb_t *sb, uint32_t first_cluster,
                             size_t offset, void *buf, size_t len) {
    /* Find which cluster the offset falls in */
    size_t   bytes_per_cluster = sb->bytes_per_cluster;
    uint32_t skip_clusters     = (uint32_t)(offset / bytes_per_cluster);
    size_t   cluster_off       = offset % bytes_per_cluster;

    uint32_t cluster = first_cluster;
    for (uint32_t i = 0; i < skip_clusters; i++) {
        cluster = fat_read(sb, cluster);
        if (cluster >= FAT32_CLUSTER_END) return 0; /* past EOF */
    }

    char *dst = (char *)buf;
    size_t done = 0;
    while (done < len && cluster < FAT32_CLUSTER_END) {
        uint64_t base  = cluster_byte_offset(sb, cluster) + cluster_off;
        size_t   avail = bytes_per_cluster - cluster_off;
        size_t   chunk = len - done;
        if (chunk > avail) chunk = avail;

        int r = bcache_read_bytes(sb->bc, base, dst + done, chunk);
        if (r < 0) return (int)done;

        done        += chunk;
        cluster_off  = 0;
        cluster      = fat_read(sb, cluster);
    }
    return (int)done;
}

/* Find a free cluster in the FAT */
uint32_t fat32_alloc_cluster(fat32_sb_t *sb) {
    uint32_t first = sb->next_free_cluster;
    uint32_t end = sb->total_clusters + 2;

    if (first < 2 || first >= end)
        first = 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass == 0 ? first : 2;
        uint32_t stop = pass == 0 ? end : first;

        for (uint32_t c = start; c < stop; c++) {
            if (fat_read(sb, c) == FAT32_CLUSTER_FREE) {
                fat_write(sb, c, FAT32_CLUSTER_END_MARK);
                void *zero = kcalloc(1, sb->bytes_per_cluster);
                if (!zero) {
                    fat_write(sb, c, FAT32_CLUSTER_FREE);
                    return 0;
                }
                int r = bcache_write_bytes(sb->bc, cluster_byte_offset(sb, c),
                                           zero, sb->bytes_per_cluster);
                kfree(zero);
                if (r < 0) {
                    fat_write(sb, c, FAT32_CLUSTER_FREE);
                    return 0;
                }
                sb->next_free_cluster = c + 1;
                if (sb->next_free_cluster >= end)
                    sb->next_free_cluster = 2;
                return c;
            }
        }
    }
    return 0;
}

/* Extend cluster chain by one cluster, return new cluster */
uint32_t fat32_extend_chain(fat32_sb_t *sb, uint32_t last_cluster) {
    uint32_t new = fat32_alloc_cluster(sb);
    if (!new) return 0;
    fat_write(sb, last_cluster, new);
    return new;
}

/* ============================================================
 * Directory parsing helpers
 * ============================================================ */

/* Read a single directory entry (raw 32 bytes) at byte offset within dir cluster chain */

/* Build a filename from LFN entries collected, or from 8.3 if no LFN */

/* ---- LFN name assembly ---- */
#define LFN_MAX_SEGS  20


/* Find a file/dir by name in a directory cluster chain.
 * Returns: first cluster, or 0 if not found.
 * *is_dir: 1 if directory, *out_size: file size.
 * *dirent_off: byte offset of the directory entry (for update) */









/* ============================================================
 * VNode operations (FAT32 implementation)
 * ============================================================ */


void fat32_lock(fat32_sb_t *sb) {
    if (sb)
        mutex_lock(&sb->lock);
}

void fat32_unlock(fat32_sb_t *sb) {
    if (sb)
        mutex_unlock(&sb->lock);
}

/* vnode cache — callers must hold sb->lock.
 * The cache owns one vnode reference per entry; remove transfers that
 * reference to the caller, who must vnode_put it after dropping the lock. */
vnode_t *fat32_vcache_find(fat32_sb_t *sb, uint64_t ino) {
    for (int i = 0; i < sb->vcache_count; i++) {
        if (sb->vcache[i].vn && sb->vcache[i].ino == ino)
            return sb->vcache[i].vn;
    }
    return NULL;
}

void fat32_vcache_add(fat32_sb_t *sb, uint64_t ino, vnode_t *vn) {
    if (sb->vcache_count >= FAT32_VCACHE_MAX)
        return;
    vnode_get(vn);
    sb->vcache[sb->vcache_count].vn = vn;
    sb->vcache[sb->vcache_count].ino = ino;
    sb->vcache_count++;
}

vnode_t *fat32_vcache_remove(fat32_sb_t *sb, uint64_t ino) {
    for (int i = 0; i < sb->vcache_count; i++) {
        if (sb->vcache[i].vn && sb->vcache[i].ino == ino) {
            vnode_t *vn = sb->vcache[i].vn;
            sb->vcache[i] = sb->vcache[sb->vcache_count - 1];
            sb->vcache[sb->vcache_count - 1].vn = NULL;
            sb->vcache[sb->vcache_count - 1].ino = 0;
            sb->vcache_count--;
            return vn;
        }
    }
    return NULL;
}

void fat32_free_cluster_chain(fat32_sb_t *sb, uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END) {
        uint32_t next = fat_read(sb, cluster);
        fat_write(sb, cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
}


static fat32_meta_t g_fat32_meta[FAT32_META_MAX];

fat32_meta_t *fat32_get_meta(fat32_sb_t *sb, uint64_t ino, int is_dir, int create) {
    for (int i = 0; i < FAT32_META_MAX; i++) {
        if (g_fat32_meta[i].used && g_fat32_meta[i].sb == sb && g_fat32_meta[i].ino == ino)
            return &g_fat32_meta[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < FAT32_META_MAX; i++) {
        if (!g_fat32_meta[i].used) {
            memset(&g_fat32_meta[i], 0, sizeof(g_fat32_meta[i]));
            g_fat32_meta[i].used = 1;
            g_fat32_meta[i].sb = sb;
            g_fat32_meta[i].ino = ino;
            g_fat32_meta[i].mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0755);
            return &g_fat32_meta[i];
        }
    }
    return NULL;
}

vnode_t *fat32_make_vnode(fat32_sb_t *sb, uint32_t cluster,
                                  size_t size, int is_dir, vnode_t *parent,
                                  uint64_t ino);

/* vnode_ops: lookup */

/* vnode_ops: stat */

/* vnode_ops: release */

/* vnode_ops: mkdir */

/* vnode_ops: create (regular file) */

/* vnode_ops: unlink */

/* vnode_ops: truncate */





/* vnode_ops: rename (file or directory, same volume)
 * FAT has no single rename primitive; we create the new directory entry
 * pointing at the source cluster, then delete the old entry.  If the target
 * already exists it is replaced (unlink the old target first).  Moving a
 * directory additionally rewrites its ".." entry to the new parent.  The
 * operation is not atomic on disk (matches Linux semantics closely enough
 * for a hobby kernel) but every step is under sb->lock so no concurrent
 * rename can interleave. */



vnode_ops_t g_fat32_vnode_ops = {
    .lookup   = fat32_lookup,
    .create   = fat32_vn_create,
    .mkdir    = fat32_vn_mkdir,
    .unlink   = fat32_vn_unlink,
    .rmdir    = fat32_vn_rmdir,
    .rename   = fat32_vn_rename,
    .stat     = fat32_stat,
    .statfs   = fat32_vn_statfs,
    .truncate = fat32_vn_truncate,
    .readpage = fat32_vn_readpage,
    .writepage = fat32_vn_writepage,
    .chmod    = fat32_vn_chmod,
    .chown    = fat32_vn_chown,
    .open     = fat32_open_vnode,
    .release  = fat32_release_vn,
};

vnode_t *fat32_make_vnode(fat32_sb_t *sb, uint32_t cluster,
                                  size_t size, int is_dir, vnode_t *parent,
                                  uint64_t ino) {
    /* Caller holds sb->lock: reuse the cached vnode so that an inode has
     * exactly one live vnode, which unlink/rmdir rely on to defer data
     * freeing until the last reference drops.  The in-memory file_size is
     * authoritative while the vnode is alive (the dirent size lags behind
     * until writeback), so a cache hit must not overwrite it. */
    vnode_t *cached = fat32_vcache_find(sb, ino);
    if (cached) {
        vnode_get(cached);
        return cached;
    }

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino       = ino;
    vn->type      = is_dir ? VFS_FT_DIR : VFS_FT_REGULAR;
    fat32_meta_t *m = fat32_get_meta(sb, ino, is_dir, 1);
    vn->mode      = m ? m->mode : (is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0755));
    vn->uid       = m ? m->uid : 0;
    vn->gid       = m ? m->gid : 0;
    vn->size      = size;
    vnode_ref_init(vn, 1);
    vn->parent    = parent;
    if (parent) vnode_get(parent);
    if (parent)
        vn->mnt = parent->mnt;        /* inherit mount for link()/dcache */
    vn->ops       = &g_fat32_vnode_ops;

    fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)kmalloc(sizeof(fat32_vnode_priv_t));
    if (!fp) { kfree(vn); return NULL; }
    fp->sb            = sb;
    fp->first_cluster = cluster;
    fp->file_size     = size;
    fp->is_dir        = is_dir;
    fp->unlinked      = 0;
    vn->fs_data = fp;
    fat32_vcache_add(sb, ino, vn);
    return vn;
}

/* ============================================================
 * vfile_ops (FAT32 open file operations)
 * ============================================================ */












/* ============================================================
 * Mount / Unmount
 * ============================================================ */

vnode_t *fat32_mount(bcache_t *bc) {
    fat32_sb_t *sb = (fat32_sb_t *)kmalloc(sizeof(fat32_sb_t));
    if (!sb) {
        kdebug("[FAT32] Failed to allocate superblock\n");
        return NULL;
    }
    memset(sb, 0, sizeof(*sb));
    mutex_init(&sb->lock);

    sb->bc = bc;
    fat32_bpb_t bpb;
    if (bcache_read_bytes(sb->bc, 0, &bpb, sizeof(bpb)) < 0) {
        kdebug("[FAT32] Failed to read boot sector\n");
        kfree(sb);
        return NULL;
    }

    /* Verify FAT32 signature */
    if (bpb.bytes_per_sector != 512 && bpb.bytes_per_sector != 4096) {
        if (bpb.bytes_per_sector != 1024 && bpb.bytes_per_sector != 2048) {
            kdebug("[FAT32] Invalid bytes_per_sector: %d\n", bpb.bytes_per_sector);
            kfree(sb);
            return NULL;
        }
    }
    if (bpb.num_fats == 0 || bpb.sectors_per_cluster == 0 ||
        bpb.reserved_sectors == 0 || bpb.fat_size_32 == 0) {
        kdebug("[FAT32] Invalid FAT32 superblock (nft=%d spc=%d rs=%d fsz=%u)\n",
               bpb.num_fats, bpb.sectors_per_cluster,
               bpb.reserved_sectors, bpb.fat_size_32);
        kfree(sb);
        return NULL;
    }

    sb->first_fat_sector   = bpb.reserved_sectors;
    sb->sectors_per_fat    = bpb.fat_size_32;
    sb->first_data_sector  = bpb.reserved_sectors + bpb.num_fats * bpb.fat_size_32;
    sb->root_cluster       = bpb.root_cluster;
    sb->sectors_per_cluster = bpb.sectors_per_cluster;
    sb->bytes_per_cluster  = bpb.sectors_per_cluster * bpb.bytes_per_sector;
    sb->total_clusters     = (bpb.total_sectors_32 - sb->first_data_sector)
                               / bpb.sectors_per_cluster;
    sb->next_free_cluster  = 2;


    kdebug("[FAT32] Mounted: cluster=%d sectors, FAT starts @%d, data @%d, root_cluster=%d\n",
           bpb.sectors_per_cluster,
           sb->first_fat_sector, sb->first_data_sector, sb->root_cluster);

    vnode_t *root = fat32_make_vnode(sb, sb->root_cluster, 0, 1, NULL, (uint64_t)sb->root_cluster);
    if (!root) { kfree(sb); return NULL; }
    root->parent = root; /* root's parent is itself */

    return root;
}

void fat32_unmount(vnode_t *root) {
    if (!root || !root->fs_data) return;
    fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)root->fs_data;
    fat32_sb_t *sb = fp->sb;
    bcache_sync(sb->bc);

    /* Drop all cache-owned vnode references; survivors (still-open files)
     * stay alive on their remaining references and unlinked inodes are
     * reclaimed by release(). */
    vnode_t *held[FAT32_VCACHE_MAX];
    int held_count = 0;
    fat32_lock(sb);
    while (sb->vcache_count > 0 && held_count < FAT32_VCACHE_MAX) {
        uint64_t ino = sb->vcache[sb->vcache_count - 1].ino;
        vnode_t *vn = fat32_vcache_remove(sb, ino);
        if (vn)
            held[held_count++] = vn;
    }
    fat32_unlock(sb);
    for (int i = 0; i < held_count; i++)
        vnode_put(held[i]);

    /* Clear g_fat32_meta entries referencing this sb */
    for (int i = 0; i < (int)(sizeof(g_fat32_meta) / sizeof(g_fat32_meta[0])); i++) {
        if (g_fat32_meta[i].sb == sb)
            memset(&g_fat32_meta[i], 0, sizeof(g_fat32_meta[i]));
    }

    if (root->ops && root->ops->release) root->ops->release(root);
    kfree(sb);
}

/* ============================================================
 * VFS open hook: create vfile for a fat32 vnode
 * Called by vfs.c when opening files on a FAT32 mount
 * ============================================================ */


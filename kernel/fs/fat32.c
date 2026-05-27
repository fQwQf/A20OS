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

static uint64_t cluster_to_lba(fat32_sb_t *sb, uint32_t cluster) {
    /* Cluster 2 = first data cluster */
    return (uint64_t)(sb->first_data_sector +
                      (cluster - 2) * sb->sectors_per_cluster);
}

static uint64_t cluster_byte_offset(fat32_sb_t *sb, uint32_t cluster) {
    return cluster_to_lba(sb, cluster) * FAT32_SECTOR_SIZE;
}

static uint64_t fat_entry_offset(fat32_sb_t *sb, uint32_t cluster) {
    /* Each FAT32 entry is 4 bytes */
    return (uint64_t)(sb->first_fat_sector * FAT32_SECTOR_SIZE + cluster * 4);
}

/* Read the FAT entry for a cluster */
static uint32_t fat_read(fat32_sb_t *sb, uint32_t cluster) {
    uint32_t val;
    uint64_t off = fat_entry_offset(sb, cluster);
    bcache_read_bytes(sb->bc, off, &val, 4);
    return val & 0x0FFFFFFF; /* mask upper nibble */
}

/* Write the FAT entry for a cluster */
static void fat_write(fat32_sb_t *sb, uint32_t cluster, uint32_t next) {
    /* Read-modify-write to preserve top nibble */
    uint32_t val;
    uint64_t off = fat_entry_offset(sb, cluster);
    bcache_read_bytes(sb->bc, off, &val, 4);
    val = (val & 0xF0000000) | (next & 0x0FFFFFFF);
    bcache_write_bytes(sb->bc, off, &val, 4);
}

/* Follow cluster chain, reading N bytes at file offset */
static int fat32_chain_read(fat32_sb_t *sb, uint32_t first_cluster,
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
static uint32_t fat32_alloc_cluster(fat32_sb_t *sb) {
    for (uint32_t c = 2; c < sb->total_clusters + 2; c++) {
        if (fat_read(sb, c) == FAT32_CLUSTER_FREE) {
            fat_write(sb, c, FAT32_CLUSTER_END_MARK);
            return c;
        }
    }
    return 0;
}

/* Extend cluster chain by one cluster, return new cluster */
static uint32_t fat32_extend_chain(fat32_sb_t *sb, uint32_t last_cluster) {
    uint32_t new = fat32_alloc_cluster(sb);
    if (!new) return 0;
    fat_write(sb, last_cluster, new);
    return new;
}

/* ============================================================
 * Directory parsing helpers
 * ============================================================ */

/* Read a single directory entry (raw 32 bytes) at byte offset within dir cluster chain */
static int read_raw_dirent(fat32_sb_t *sb, uint32_t dir_cluster,
                            size_t byte_off, fat32_dirent_t *de) {
    return fat32_chain_read(sb, dir_cluster, byte_off, de, sizeof(*de));
}

/* Build a filename from LFN entries collected, or from 8.3 if no LFN */
static void decode_8_3(const uint8_t *raw, char *out) {
    int i = 0, j = 0;
    /* name part (8 chars) */
    while (i < 8 && raw[i] != ' ' && raw[i] != 0) out[j++] = (char)raw[i++];
    i = 8;
    /* extension (3 chars) */
    if (raw[8] != ' ' && raw[8] != 0) {
        out[j++] = '.';
        while (i < 11 && raw[i] != ' ' && raw[i] != 0) out[j++] = (char)raw[i++];
    }
    out[j] = '\0';
    /* Convert to lowercase */
    for (int k = 0; k < j; k++)
        if (out[k] >= 'A' && out[k] <= 'Z') out[k] += 32;
}

/* ---- LFN name assembly ---- */
#define LFN_MAX_SEGS  20

typedef struct {
    char name[13 * LFN_MAX_SEGS + 1];  /* max LFN = 255 chars */
    int  valid;
} lfn_buf_t;

static void lfn_append_seg(lfn_buf_t *lb, const fat32_lfn_t *lfn) {
    int order = lfn->order & 0x3F; /* strip "last" flag */
    if (order < 1 || order > LFN_MAX_SEGS) return;
    int base = (order - 1) * 13;
    /* Each LFN entry holds 13 UTF-16LE characters */
    int pos = base;
    for (int i = 0; i < 5  && pos < 255; i++, pos++) lb->name[pos] = (char)(lfn->name1[i] & 0xFF);
    for (int i = 0; i < 6  && pos < 255; i++, pos++) lb->name[pos] = (char)(lfn->name2[i] & 0xFF);
    for (int i = 0; i < 2  && pos < 255; i++, pos++) lb->name[pos] = (char)(lfn->name3[i] & 0xFF);
    lb->name[255] = '\0';
    lb->valid = 1;
}

/* Find a file/dir by name in a directory cluster chain.
 * Returns: first cluster, or 0 if not found.
 * *is_dir: 1 if directory, *out_size: file size.
 * *dirent_off: byte offset of the directory entry (for update) */
static uint32_t fat32_dir_lookup(fat32_sb_t *sb, uint32_t dir_cluster,
                                  const char *name, int *is_dir,
                                  size_t *out_size, size_t *dirent_off) {
    char fname[256];
    lfn_buf_t lfn;
    memset(&lfn, 0, sizeof(lfn));

    size_t off = 0;
    while (1) {
        fat32_dirent_t de;
        int r = read_raw_dirent(sb, dir_cluster, off, &de);
        if (r <= 0) break;

        if (de.name[0] == 0x00) break; /* end of directory */
        if ((uint8_t)de.name[0] == 0xE5) { /* deleted entry */
            off += 32;
            memset(&lfn, 0, sizeof(lfn));
            continue;
        }

        if (de.attr == FAT_ATTR_LFN) {
            fat32_lfn_t *lfne = (fat32_lfn_t *)&de;
            lfn_append_seg(&lfn, lfne);
            off += 32;
            continue;
        }

        /* Regular or directory entry */
        if (lfn.valid) {
            /* Use LFN name — strip trailing 0xFF */
            for (int k = 0; k < 255; k++) {
                if (lfn.name[k] == '\0' || (uint8_t)lfn.name[k] == 0xFF)
                    { lfn.name[k] = '\0'; break; }
            }
            strncpy(fname, lfn.name, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
            memset(&lfn, 0, sizeof(lfn));
        } else {
            decode_8_3(de.name, fname);
        }

        /* Compare (case-insensitive) */
        if (strcasecmp(fname, name) == 0) {
            uint32_t cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
            if (is_dir)     *is_dir     = (de.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
            if (out_size)   *out_size   = de.file_size;
            if (dirent_off) *dirent_off = off;
            /* Root dir of FAT32 starts at root_cluster when cluster == 0 */
            if (cluster == 0) cluster = sb->root_cluster;
            return cluster;
        }

        off += 32;
        memset(&lfn, 0, sizeof(lfn));
    }
    return 0; /* not found */
}

static void encode_83_name(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    int name_len = dot ? (int)(dot - name) : (int)strlen(name);
    const char *ext = dot ? dot + 1 : NULL;
    int ni = 0;
    for (int k = 0; k < name_len && ni < 8; k++) {
        char c = name[k];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[ni++] = (uint8_t)c;
    }
    int ei = 8;
    if (ext) {
        for (int k = 0; ext[k] && ei < 11; k++) {
            char c = ext[k];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[ei++] = (uint8_t)c;
        }
    }
}

/* ============================================================
 * VNode operations (FAT32 implementation)
 * ============================================================ */

/* Internal: FAT32 vnode private data */
typedef struct fat32_vnode_priv {
    fat32_sb_t *sb;
    uint32_t    first_cluster;
    size_t      file_size;
    int         is_dir;
} fat32_vnode_priv_t;

static int fat32_vn_writepage(vnode_t *vn, uint64_t index,
                              const void *data, size_t len);
static vfile_ops_t g_fat32_fops;

#define FAT32_META_MAX 1024
typedef struct fat32_meta {
    fat32_sb_t *sb;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    int used;
} fat32_meta_t;

static fat32_meta_t g_fat32_meta[FAT32_META_MAX];

static fat32_meta_t *fat32_get_meta(fat32_sb_t *sb, uint64_t ino, int is_dir, int create) {
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

static vnode_t *fat32_make_vnode(fat32_sb_t *sb, uint32_t cluster,
                                  size_t size, int is_dir, vnode_t *parent,
                                  uint64_t ino);

/* vnode_ops: lookup */
static int fat32_lookup(vnode_t *dir, const char *name, vnode_t **out) {
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

    int is_dir; size_t sz; size_t doff;
    uint32_t cluster = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (!cluster) return -ENOENT;

    /* Assign a unique inode number from cluster */
    *out = fat32_make_vnode(p->sb, cluster, sz, is_dir, dir, (uint64_t)cluster);
    if (!*out) return -ENOMEM;
    /* parent ref_count bumped in make_vnode */
    return 0;
}

/* vnode_ops: stat */
static int fat32_stat(vnode_t *vn, kstat_t *st) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    st->st_ino  = vn->ino;
    st->st_size = p->file_size;
    st->st_blksize = 512;
    st->st_blocks  = (p->file_size + 511) / 512;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_nlink = 1;
    return 0;
}

/* vnode_ops: release */
static void fat32_release_vn(vnode_t *vn) {
    if (vn->fs_data) { kfree(vn->fs_data); vn->fs_data = NULL; }
    vnode_put(vn->parent);
    kfree(vn);
}

/* vnode_ops: mkdir */
static int fat32_vn_mkdir(vnode_t *dir, const char *name, int mode) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;

    /* Check existence */
    int is_dir; size_t sz; size_t doff;
    uint32_t existing = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (existing) return -EEXIST;

    /* Allocate cluster for new directory */
    uint32_t new_cluster = fat32_alloc_cluster(p->sb);
    if (!new_cluster) return -ENOSPC;

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

    /* Write short 8.3 entry in parent directory */
    /* Find free slot in parent dir */
    size_t off = 0;
    while (1) {
        fat32_dirent_t de;
        int r = read_raw_dirent(sb, p->first_cluster, off, &de);
        if (r <= 0) break;
        if (de.name[0] == 0x00 || (uint8_t)de.name[0] == 0xE5) {
            /* Free slot found */
            fat32_dirent_t nde;
            memset(&nde, 0, sizeof(nde));
            encode_83_name(name, nde.name);
            nde.attr = FAT_ATTR_DIRECTORY;
            nde.fst_clus_hi = (uint16_t)(new_cluster >> 16);
            nde.fst_clus_lo = (uint16_t)(new_cluster & 0xFFFF);
            nde.file_size = 0;

            /* Find byte offset of this slot in cluster chain & write */
            uint32_t cluster = p->first_cluster;
            size_t remaining = off;
            while (remaining >= sb->bytes_per_cluster) {
                remaining -= sb->bytes_per_cluster;
                cluster = fat_read(sb, cluster);
            }
            uint64_t write_off = cluster_byte_offset(sb, cluster) + remaining;
            bcache_write_bytes(sb->bc, write_off, &nde, sizeof(nde));
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
            return 0;
        }
        off += 32;
    }
    return -ENOSPC;
}

/* vnode_ops: create (regular file) */
static int fat32_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;

    fat32_sb_t *sb = p->sb;
    uint32_t new_cluster = fat32_alloc_cluster(sb);
    if (!new_cluster) return -ENOSPC;

    /* Find free slot in parent directory */
    size_t off = 0;
    while (1) {
        fat32_dirent_t de;
        int r = read_raw_dirent(sb, p->first_cluster, off, &de);
        if (r <= 0) break;
        if (de.name[0] == 0x00 || (uint8_t)de.name[0] == 0xE5) {
            fat32_dirent_t nde;
            memset(&nde, 0, sizeof(nde));
            encode_83_name(name, nde.name);
            nde.attr = FAT_ATTR_ARCHIVE;
            nde.fst_clus_hi = (uint16_t)(new_cluster >> 16);
            nde.fst_clus_lo = (uint16_t)(new_cluster & 0xFFFF);
            nde.file_size = 0;

            uint32_t cluster = p->first_cluster;
            size_t remaining = off;
            while (remaining >= sb->bytes_per_cluster) {
                remaining -= sb->bytes_per_cluster;
                cluster = fat_read(sb, cluster);
            }
            uint64_t write_off = cluster_byte_offset(sb, cluster) + remaining;
            bcache_write_bytes(sb->bc, write_off, &nde, sizeof(nde));
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
                if (!*out) return -ENOMEM;
            }
            return 0;
        }
        off += 32;
    }
    return -ENOSPC;
}

/* vnode_ops: unlink */
static int fat32_vn_unlink(vnode_t *dir, const char *name) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;

    int is_dir; size_t sz; size_t doff;
    uint32_t cluster = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (!cluster) return -ENOENT;
    if (is_dir) return -EISDIR;

    /* Mark directory entry as deleted */
    fat32_sb_t *sb = p->sb;
    uint32_t dc = p->first_cluster;
    size_t rem = doff;
    while (rem >= sb->bytes_per_cluster) {
        rem -= sb->bytes_per_cluster;
        dc = fat_read(sb, dc);
    }
    uint64_t off = cluster_byte_offset(sb, dc) + rem;
    uint8_t deleted = 0xE5;
    bcache_write_bytes(sb->bc, off, &deleted, 1);

    /* Free cluster chain */
    while (cluster < FAT32_CLUSTER_END) {
        uint32_t next = fat_read(sb, cluster);
        fat_write(sb, cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
    return 0;
}

/* vnode_ops: truncate */
static int fat32_vn_truncate(vnode_t *vn, size_t size) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    if (!p) return -EINVAL;
    if (p->is_dir) return -EISDIR;

    uint32_t old_first_cluster = p->first_cluster;
    if (size == 0) {
        fat32_sb_t *sb = p->sb;
        uint32_t new_cluster = fat32_alloc_cluster(sb);
        if (!new_cluster) return -ENOSPC;

        uint32_t cluster = p->first_cluster;
        while (cluster < FAT32_CLUSTER_END) {
            uint32_t next = fat_read(sb, cluster);
            fat_write(sb, cluster, FAT32_CLUSTER_FREE);
            cluster = next;
        }
        p->first_cluster = new_cluster;
    } else {
        uint32_t cluster = p->first_cluster;
        if (cluster < 2 || cluster >= FAT32_CLUSTER_END) {
            cluster = fat32_alloc_cluster(p->sb);
            if (!cluster) return -ENOSPC;
            p->first_cluster = cluster;
        }

        size_t need_clusters = (size + p->sb->bytes_per_cluster - 1) / p->sb->bytes_per_cluster;
        for (size_t i = 1; i < need_clusters; i++) {
            uint32_t next = fat_read(p->sb, cluster);
            if (next >= FAT32_CLUSTER_END) {
                next = fat32_extend_chain(p->sb, cluster);
                if (!next) return -ENOSPC;
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

    return 0;
}

static int fat32_vn_chmod(vnode_t *vn, int mode) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    fat32_meta_t *m = fat32_get_meta(p->sb, vn->ino, p->is_dir, 1);
    if (!m) return -ENOSPC;
    m->mode = (m->mode & S_IFMT) | (mode & 07777);
    vn->mode = m->mode;
    return 0;
}

static int fat32_vn_chown(vnode_t *vn, int uid, int gid) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    fat32_meta_t *m = fat32_get_meta(p->sb, vn->ino, p->is_dir, 1);
    if (!m) return -ENOSPC;
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
    return 0;
}

static int fat32_dir_is_empty(fat32_sb_t *sb, uint32_t dir_cluster) {
    size_t off = 0;
    int active = 0;
    while (1) {
        fat32_dirent_t de;
        int r = read_raw_dirent(sb, dir_cluster, off, &de);
        if (r <= 0) break;
        if (de.name[0] == 0x00) break;
        off += 32;
        if ((uint8_t)de.name[0] == 0xE5) continue;
        if (de.attr == FAT_ATTR_LFN) continue;
        if (de.attr & FAT_ATTR_VOL_LABEL) continue;
        active++;
    }
    return active <= 2 ? 0 : -ENOTEMPTY;
}

static int fat32_vn_rmdir(vnode_t *dir, const char *name) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)dir->fs_data;
    if (!p->is_dir) return -ENOTDIR;

    int is_dir; size_t sz; size_t doff;
    uint32_t cluster = fat32_dir_lookup(p->sb, p->first_cluster, name, &is_dir, &sz, &doff);
    if (!cluster) return -ENOENT;
    if (!is_dir) return -ENOTDIR;

    fat32_sb_t *sb = p->sb;
    int r = fat32_dir_is_empty(sb, cluster);
    if (r < 0) return r;

    uint32_t dc = p->first_cluster;
    size_t rem = doff;
    while (rem >= sb->bytes_per_cluster) {
        rem -= sb->bytes_per_cluster;
        dc = fat_read(sb, dc);
    }
    uint64_t off = cluster_byte_offset(sb, dc) + rem;
    uint8_t deleted = 0xE5;
    bcache_write_bytes(sb->bc, off, &deleted, 1);

    while (cluster < FAT32_CLUSTER_END) {
        uint32_t next = fat_read(sb, cluster);
        fat_write(sb, cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
    return 0;
}

static vnode_ops_t g_fat32_vnode_ops = {
    .lookup   = fat32_lookup,
    .create   = fat32_vn_create,
    .mkdir    = fat32_vn_mkdir,
    .unlink   = fat32_vn_unlink,
    .rmdir    = fat32_vn_rmdir,
    .rename   = NULL,
    .stat     = fat32_stat,
    .truncate = fat32_vn_truncate,
    .writepage = fat32_vn_writepage,
    .chmod    = fat32_vn_chmod,
    .chown    = fat32_vn_chown,
    .release  = fat32_release_vn,
};

static vnode_t *fat32_make_vnode(fat32_sb_t *sb, uint32_t cluster,
                                  size_t size, int is_dir, vnode_t *parent,
                                  uint64_t ino) {
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
    vn->ops       = &g_fat32_vnode_ops;

    fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)kmalloc(sizeof(fat32_vnode_priv_t));
    if (!fp) { kfree(vn); return NULL; }
    fp->sb            = sb;
    fp->first_cluster = cluster;
    fp->file_size     = size;
    fp->is_dir        = is_dir;
    vn->fs_data = fp;
    return vn;
}

/* ============================================================
 * vfile_ops (FAT32 open file operations)
 * ============================================================ */

/* Internal open-file context */
typedef struct fat32_fctx {
    fat32_sb_t *sb;
    uint32_t    first_cluster;
    size_t      file_size;
    int         is_dir;
    /* Track "current cluster and offset within it" for sequential I/O */
    uint32_t    cur_cluster;
    size_t      cluster_off;     /* offset within cur_cluster data */
    size_t      file_off;        /* total file offset */
    /* For directory iteration (getdents) */
    size_t      dir_byte_off;
    /* For updating directory entry on close */
    uint32_t    parent_cluster;
    size_t      parent_dirent_off;
    int         dirty;           /* file_size changed, needs writeback */
} fat32_fctx_t;

static uint32_t fat32_fctx_cluster_at(fat32_fctx_t *fc, size_t offset, int extend) {
    fat32_sb_t *sb = fc->sb;
    size_t bytes_per_cluster = sb->bytes_per_cluster;
    size_t target_off = offset % bytes_per_cluster;

    if (fc->cur_cluster && fc->cluster_off == target_off)
        return fc->cur_cluster;

    uint32_t cluster = fc->first_cluster;
    if (!cluster)
        return 0;

    size_t skip = offset / bytes_per_cluster;
    for (size_t i = 0; i < skip; i++) {
        uint32_t next = fat_read(sb, cluster);
        if (next >= FAT32_CLUSTER_END) {
            if (!extend)
                return 0;
            next = fat32_extend_chain(sb, cluster);
            if (!next)
                return 0;
        }
        cluster = next;
    }

    fc->cur_cluster = cluster;
    fc->cluster_off = target_off;
    return cluster;
}

static void fat32_fctx_cache_pos(fat32_fctx_t *fc, uint32_t cluster,
                                 size_t cluster_off) {
    if (cluster && cluster_off != 0) {
        fc->cur_cluster = cluster;
        fc->cluster_off = cluster_off;
    } else {
        fc->cur_cluster = 0;
        fc->cluster_off = 0;
    }
}

static int fat32_fread(vfile_t *vf, char *buf, size_t count) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    fat32_sb_t *sb = fc->sb;
    if (fc->is_dir) return -EISDIR;
    size_t fsize = vf->vnode->size;
    fc->file_size = fsize;
    if (fc->file_off >= fsize) return 0;
    size_t remaining = fsize - fc->file_off;
    if (count > remaining) count = remaining;
    if (count == 0) return 0;

    uint32_t cluster = fat32_fctx_cluster_at(fc, fc->file_off, 0);
    if (!cluster)
        return 0;

    char *dst = buf;
    size_t done = 0;
    size_t off = fc->cluster_off;
    while (done < count && cluster < FAT32_CLUSTER_END) {
        size_t avail = sb->bytes_per_cluster - off;
        size_t chunk = count - done;
        if (chunk > avail) chunk = avail;

        uint64_t base = cluster_byte_offset(sb, cluster) + off;
        if (bcache_read_bytes(sb->bc, base, dst + done, chunk) < 0)
            break;

        done += chunk;
        off += chunk;
        if (off == sb->bytes_per_cluster) {
            off = 0;
            if (done < count)
                cluster = fat_read(sb, cluster);
        }
    }

    fc->file_off += done;
    vf->offset = fc->file_off;
    fat32_fctx_cache_pos(fc, cluster, off);
    return (int)done;
}

static int fat32_fwrite(vfile_t *vf, const char *buf, size_t count) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    fat32_sb_t *sb = fc->sb;
    if (fc->is_dir) return -EISDIR;
    if (count == 0) return 0;

    size_t bytes_per_cluster = sb->bytes_per_cluster;
    uint32_t cluster = fat32_fctx_cluster_at(fc, fc->file_off, 1);
    if (!cluster)
        return -ENOSPC;
    size_t off = fc->cluster_off;

    const char *src = buf;
    size_t done = 0;
    while (done < count) {
        size_t avail = bytes_per_cluster - off;
        size_t chunk = count - done;
        if (chunk > avail) chunk = avail;

        uint64_t base = cluster_byte_offset(sb, cluster) + off;
        int r = bcache_write_bytes(sb->bc, base, src + done, chunk);
        if (r < 0) break;

        done += chunk;
        off += chunk;
        if (off == bytes_per_cluster) {
            off = 0;
        }
        if (done < count && off == 0) {
            uint32_t next = fat_read(sb, cluster);
            if (next >= FAT32_CLUSTER_END) {
                next = fat32_extend_chain(sb, cluster);
                if (!next) break;
            }
            cluster = next;
        }
    }

    fc->file_off += done;
    if (fc->file_off > vf->vnode->size) {
        vf->vnode->size = fc->file_off;
        fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)vf->vnode->fs_data;
        if (fp) fp->file_size = fc->file_off;
        fc->file_size = fc->file_off;
        fc->dirty = 1;
    }
    vf->offset += done;
    fat32_fctx_cache_pos(fc, cluster, off);
    return (int)done;
}

static int fat32_vn_writepage(vnode_t *vn, uint64_t index,
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

    int r = fat32_fwrite(&vf, (const char *)data, n);
    if (r < 0)
        return r;
    return (size_t)r == n ? 0 : -EIO;
}

static long fat32_flseek(vfile_t *vf, long offset, int whence) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    fc->file_size = vf->vnode->size;
    long new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END: new_off = (long)fc->file_size + offset; break;
        default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    vf->offset   = (size_t)new_off;
    fc->file_off = (size_t)new_off;
    fc->cur_cluster = (new_off == 0) ? fc->first_cluster : 0;
    fc->cluster_off = 0;
    return new_off;
}

static int fat32_freaddir(vfile_t *vf, void *dirp, size_t count) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    if (!fc->is_dir) return -ENOTDIR;

    char *out    = (char *)dirp;
    size_t total = 0;
    lfn_buf_t lfn;
    memset(&lfn, 0, sizeof(lfn));

    while (1) {
        fat32_dirent_t de;
        int r = fat32_chain_read(fc->sb, fc->first_cluster,
                                  fc->dir_byte_off, &de, sizeof(de));
        if (r <= 0) break;
        if (de.name[0] == 0x00) break;
        fc->dir_byte_off += 32;
        if ((uint8_t)de.name[0] == 0xE5) { memset(&lfn, 0, sizeof(lfn)); continue; }

        if (de.attr == FAT_ATTR_LFN) {
            lfn_append_seg(&lfn, (fat32_lfn_t *)&de);
            continue;
        }
        if (de.attr & FAT_ATTR_VOL_LABEL) { memset(&lfn, 0, sizeof(lfn)); continue; }

        char fname[256];
        if (lfn.valid) {
            for (int k = 0; k < 255; k++) {
                if (lfn.name[k] == '\0' || (uint8_t)lfn.name[k] == 0xFF)
                    { lfn.name[k] = '\0'; break; }
            }
            strncpy(fname, lfn.name, 255);
            fname[255] = '\0';
            memset(&lfn, 0, sizeof(lfn));
        } else {
            decode_8_3(de.name, fname);
        }

        size_t namelen = strlen(fname);
        size_t reclen  = sizeof(vfs_dirent64_t) + namelen + 1;
        reclen = (reclen + 7) & ~7UL; /* 8-byte align */

        if (total + reclen > count) break;

        vfs_dirent64_t *dent = (vfs_dirent64_t *)(out + total);
        uint32_t cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
        dent->d_ino    = cluster ? cluster : fc->sb->root_cluster;
        dent->d_off    = (int64_t)fc->dir_byte_off;
        dent->d_reclen = (uint16_t)reclen;
        dent->d_type   = (de.attr & FAT_ATTR_DIRECTORY) ? 4 : 8; /* DT_DIR / DT_REG */
        memcpy(dent->d_name, fname, namelen + 1);

        total += reclen;
    }

    return (int)total;
}

static int fat32_fclose(vfile_t *vf) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    if (fc && fc->dirty && !fc->is_dir && fc->first_cluster >= 2) {
        fat32_sb_t *sb = fc->sb;
        vnode_t *vn = vf->vnode;
        fat32_vnode_priv_t *parent_fp = (vn && vn->parent)
                                         ? (fat32_vnode_priv_t *)vn->parent->fs_data
                                         : NULL;
        uint32_t search_cluster = parent_fp ? parent_fp->first_cluster : sb->root_cluster;

        size_t off = 0;
        while (1) {
            fat32_dirent_t de;
            int r = fat32_chain_read(sb, search_cluster, off, &de, sizeof(de));
            if (r <= 0) break;
            if (de.name[0] == 0x00) break;
            if ((uint8_t)de.name[0] == 0xE5 || de.attr == FAT_ATTR_LFN) {
                off += 32;
                continue;
            }
            uint32_t clus = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
            if (clus == fc->first_cluster) {
                de.file_size = (uint32_t)fc->file_size;
                uint32_t dc = search_cluster;
                size_t rem = off;
                while (rem >= sb->bytes_per_cluster) {
                    rem -= sb->bytes_per_cluster;
                    dc = fat_read(sb, dc);
                }
                uint64_t write_off = cluster_byte_offset(sb, dc) + rem;
                bcache_write_bytes(sb->bc, write_off, &de, sizeof(de));
                break;
            }
            off += 32;
        }
    }
    if (vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}

static vfile_ops_t g_fat32_fops = {
    .read    = fat32_fread,
    .write   = fat32_fwrite,
    .lseek   = fat32_flseek,
    .readdir = fat32_freaddir,
    .ioctl   = NULL,
    .close   = fat32_fclose,
};

/* ============================================================
 * Mount / Unmount
 * ============================================================ */

vnode_t *fat32_mount(bcache_t *bc) {
    fat32_sb_t *sb = (fat32_sb_t *)kmalloc(sizeof(fat32_sb_t));
    if (!sb) {
        printf("[FAT32] Failed to allocate superblock\n");
        return NULL;
    }
    memset(sb, 0, sizeof(*sb));

    sb->bc = bc;
    fat32_bpb_t bpb;
    if (bcache_read_bytes(sb->bc, 0, &bpb, sizeof(bpb)) < 0) {
        printf("[FAT32] Failed to read boot sector\n");
        kfree(sb);
        return NULL;
    }

    /* Verify FAT32 signature */
    if (bpb.bytes_per_sector != 512 && bpb.bytes_per_sector != 4096) {
        if (bpb.bytes_per_sector != 1024 && bpb.bytes_per_sector != 2048) {
            printf("[FAT32] Invalid bytes_per_sector: %d\n", bpb.bytes_per_sector);
            kfree(sb);
            return NULL;
        }
    }
    if (bpb.num_fats == 0 || bpb.sectors_per_cluster == 0 ||
        bpb.reserved_sectors == 0 || bpb.fat_size_32 == 0) {
        printf("[FAT32] Invalid FAT32 superblock (nft=%d spc=%d rs=%d fsz=%u)\n",
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


    printf("[FAT32] Mounted: cluster=%d sectors, FAT starts @%d, data @%d, root_cluster=%d\n",
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

vfile_t *fat32_open_vnode(vnode_t *vn, int flags) {
    fat32_vnode_priv_t *fp = (fat32_vnode_priv_t *)vn->fs_data;
    fat32_fctx_t *fc = (fat32_fctx_t *)kmalloc(sizeof(fat32_fctx_t));
    if (!fc) return NULL;
    memset(fc, 0, sizeof(*fc));
    fc->sb            = fp->sb;
    fc->first_cluster = fp->first_cluster;
    fc->file_size     = vn->size;
    fc->is_dir        = fp->is_dir;
    fc->file_off      = (flags & O_APPEND) ? vn->size : 0;
    fc->cur_cluster   = (fc->file_off == 0) ? fp->first_cluster : 0;
    fc->cluster_off   = 0;
    fc->dir_byte_off  = 0;

    vfile_t *vf = vfile_alloc();
    if (!vf) { kfree(fc); return NULL; }
    vf->vnode     = vn;
    vnode_get(vn);
    vf->flags     = flags;
    vf->offset    = fc->file_off;
    refcount_set(&vf->ref_count, 1);
    vf->ops       = &g_fat32_fops;
    vf->priv      = fc;
    return vf;
}

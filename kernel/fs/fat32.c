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

static int fat32_dir_write(fat32_sb_t *sb, uint32_t dir_cluster,
                           size_t off, const void *entry) {
    uint32_t cluster = dir_cluster;
    while (off >= sb->bytes_per_cluster) {
        off -= sb->bytes_per_cluster;
        cluster = fat_read(sb, cluster);
        if (cluster < 2 || cluster >= FAT32_CLUSTER_END)
            return -ENOSPC;
    }
    return bcache_write_bytes(sb->bc, cluster_byte_offset(sb, cluster) + off,
                              entry, sizeof(fat32_dirent_t));
}

static int fat32_short_name_exists(fat32_sb_t *sb, uint32_t dir_cluster,
                                   const uint8_t name[11]) {
    for (size_t off = 0;; off += sizeof(fat32_dirent_t)) {
        fat32_dirent_t de;
        int r = read_raw_dirent(sb, dir_cluster, off, &de);
        if (r <= 0 || de.name[0] == 0x00)
            return 0;
        if ((uint8_t)de.name[0] != 0xe5 && de.attr != FAT_ATTR_LFN &&
            memcmp(de.name, name, 11) == 0)
            return 1;
    }
}

static int fat32_make_short_name(fat32_sb_t *sb, uint32_t dir_cluster,
                                 const char *name, uint8_t out[11],
                                 int *needs_lfn) {
    char decoded[32];
    encode_83_name(name, out);
    decode_8_3(out, decoded);
    *needs_lfn = strcasecmp(decoded, name) != 0;
    if (!*needs_lfn)
        return 0;

    const char *dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    for (unsigned int sequence = 1; sequence < 1000; sequence++) {
        char digits[4];
        int ndigits = 0;
        unsigned int value = sequence;
        do {
            digits[ndigits++] = (char)('0' + value % 10);
            value /= 10;
        } while (value && ndigits < (int)sizeof(digits));

        memset(out, ' ', 11);
        size_t prefix_max = 8 - 1 - (size_t)ndigits;
        size_t pos = 0;
        for (size_t i = 0; i < base_len && pos < prefix_max; i++) {
            char c = name[i];
            if (c == ' ' || c == '.')
                continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            out[pos++] = (uint8_t)c;
        }
        if (pos == 0)
            out[pos++] = '_';
        out[pos++] = '~';
        while (ndigits > 0)
            out[pos++] = (uint8_t)digits[--ndigits];

        if (dot) {
            for (size_t i = 0; dot[1 + i] && i < 3; i++) {
                char c = dot[1 + i];
                if (c >= 'a' && c <= 'z') c -= 32;
                out[8 + i] = (uint8_t)c;
            }
        }
        if (!fat32_short_name_exists(sb, dir_cluster, out))
            return 0;
    }
    return -ENOSPC;
}

static uint8_t fat32_short_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static void fat32_lfn_set_char(fat32_lfn_t *lfn, int index, uint16_t value) {
    if (index < 5)
        lfn->name1[index] = value;
    else if (index < 11)
        lfn->name2[index - 5] = value;
    else
        lfn->name3[index - 11] = value;
}

static int fat32_create_dirents(fat32_sb_t *sb, uint32_t dir_cluster,
                                const char *name, uint8_t attr,
                                uint32_t first_cluster) {
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255)
        return -ENAMETOOLONG;

    uint8_t short_name[11];
    int needs_lfn;
    int r = fat32_make_short_name(sb, dir_cluster, name, short_name, &needs_lfn);
    if (r < 0)
        return r;
    size_t lfn_count = needs_lfn ? (name_len + 12) / 13 : 0;
    size_t needed = lfn_count + 1;
    size_t run_start = 0;
    size_t run_length = 0;

    for (size_t off = 0;; off += sizeof(fat32_dirent_t)) {
        fat32_dirent_t de;
        r = read_raw_dirent(sb, dir_cluster, off, &de);
        if (r <= 0) {
            uint32_t last = dir_cluster;
            for (;;) {
                uint32_t next = fat_read(sb, last);
                if (next >= FAT32_CLUSTER_END)
                    break;
                if (next < 2)
                    return -EIO;
                last = next;
            }
            if (!fat32_extend_chain(sb, last))
                return -ENOSPC;
            memset(&de, 0, sizeof(de));
        }
        if (de.name[0] == 0x00 || (uint8_t)de.name[0] == 0xe5) {
            if (run_length++ == 0)
                run_start = off;
            if (run_length == needed)
                break;
        } else {
            run_length = 0;
        }
    }

    uint8_t checksum = fat32_short_checksum(short_name);
    for (size_t disk_index = 0; disk_index < lfn_count; disk_index++) {
        size_t sequence = lfn_count - disk_index;
        fat32_lfn_t lfn;
        memset(&lfn, 0xff, sizeof(lfn));
        lfn.order = (uint8_t)sequence;
        if (sequence == lfn_count)
            lfn.order |= 0x40;
        lfn.attr = FAT_ATTR_LFN;
        lfn.type = 0;
        lfn.checksum = checksum;
        lfn.fst_clus = 0;
        size_t base = (sequence - 1) * 13;
        for (int i = 0; i < 13; i++) {
            size_t index = base + (size_t)i;
            uint16_t value = 0xffff;
            if (index < name_len)
                value = (uint8_t)name[index];
            else if (index == name_len)
                value = 0;
            fat32_lfn_set_char(&lfn, i, value);
        }
        r = fat32_dir_write(sb, dir_cluster,
                            run_start + disk_index * sizeof(fat32_dirent_t), &lfn);
        if (r < 0)
            return r;
    }

    fat32_dirent_t de;
    memset(&de, 0, sizeof(de));
    memcpy(de.name, short_name, sizeof(de.name));
    de.attr = attr;
    de.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    de.fst_clus_lo = (uint16_t)first_cluster;
    return fat32_dir_write(sb, dir_cluster,
                           run_start + lfn_count * sizeof(fat32_dirent_t), &de);
}

static void fat32_delete_dirents(fat32_sb_t *sb, uint32_t dir_cluster,
                                 size_t short_off) {
    uint8_t deleted = 0xe5;
    (void)fat32_dir_write(sb, dir_cluster, short_off, &deleted);
    while (short_off >= sizeof(fat32_dirent_t)) {
        size_t previous = short_off - sizeof(fat32_dirent_t);
        fat32_dirent_t de;
        if (read_raw_dirent(sb, dir_cluster, previous, &de) <= 0 ||
            de.attr != FAT_ATTR_LFN || (uint8_t)de.name[0] == 0xe5)
            break;
        (void)fat32_dir_write(sb, dir_cluster, previous, &deleted);
        short_off = previous;
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
    int         unlinked;    /* directory entry removed; free clusters on release */
} fat32_vnode_priv_t;

static inline void fat32_lock(fat32_sb_t *sb) {
    if (sb)
        mutex_lock(&sb->lock);
}

static inline void fat32_unlock(fat32_sb_t *sb) {
    if (sb)
        mutex_unlock(&sb->lock);
}

/* vnode cache — callers must hold sb->lock.
 * The cache owns one vnode reference per entry; remove transfers that
 * reference to the caller, who must vnode_put it after dropping the lock. */
static vnode_t *fat32_vcache_find(fat32_sb_t *sb, uint64_t ino) {
    for (int i = 0; i < sb->vcache_count; i++) {
        if (sb->vcache[i].vn && sb->vcache[i].ino == ino)
            return sb->vcache[i].vn;
    }
    return NULL;
}

static void fat32_vcache_add(fat32_sb_t *sb, uint64_t ino, vnode_t *vn) {
    if (sb->vcache_count >= FAT32_VCACHE_MAX)
        return;
    vnode_get(vn);
    sb->vcache[sb->vcache_count].vn = vn;
    sb->vcache[sb->vcache_count].ino = ino;
    sb->vcache_count++;
}

static vnode_t *fat32_vcache_remove(fat32_sb_t *sb, uint64_t ino) {
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

static void fat32_free_cluster_chain(fat32_sb_t *sb, uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END) {
        uint32_t next = fat_read(sb, cluster);
        fat_write(sb, cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
}

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

/* vnode_ops: stat */
static int fat32_stat(vnode_t *vn, kstat_t *st) {
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

/* vnode_ops: release */
static void fat32_release_vn(vnode_t *vn) {
    fat32_vnode_priv_t *p = (fat32_vnode_priv_t *)vn->fs_data;
    if (p && p->unlinked) {
        /* Inode was unlinked while still referenced: reclaim its data now
         * that the last reference is gone. */
        fat32_sb_t *sb = p->sb;
        p->unlinked = 0;
        fat32_lock(sb);
        fat32_free_cluster_chain(sb, p->first_cluster);
        fat32_unlock(sb);
    }
    if (vn->fs_data) { kfree(vn->fs_data); vn->fs_data = NULL; }
    if (vn->parent && vn->parent != vn) vnode_put(vn->parent);
    kfree(vn);
}

/* vnode_ops: mkdir */
static int fat32_vn_mkdir(vnode_t *dir, const char *name, int mode) {
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

/* vnode_ops: create (regular file) */
static int fat32_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
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

/* vnode_ops: unlink */
static int fat32_vn_unlink(vnode_t *dir, const char *name) {
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

    fat32_free_cluster_chain(sb, cluster);
    fat32_unlock(p->sb);
    return 0;
}

/* vnode_ops: truncate */
static int fat32_vn_truncate(vnode_t *vn, size_t size) {
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

static int fat32_vn_chmod(vnode_t *vn, int mode) {
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

static int fat32_vn_chown(vnode_t *vn, int uid, int gid) {
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

    fat32_free_cluster_chain(sb, cluster);
    fat32_unlock(p->sb);
    return 0;
}

static vfile_t *fat32_open_vnode(vnode_t *vn, int flags);
static int fat32_vn_readpage(vnode_t *vn, uint64_t index,
                             void *data, size_t len);

static int fat32_vn_statfs(vnode_t *vn, kstatfs_t *st)
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

static vnode_ops_t g_fat32_vnode_ops = {
    .lookup   = fat32_lookup,
    .create   = fat32_vn_create,
    .mkdir    = fat32_vn_mkdir,
    .unlink   = fat32_vn_unlink,
    .rmdir    = fat32_vn_rmdir,
    .rename   = NULL,
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

static vnode_t *fat32_make_vnode(fat32_sb_t *sb, uint32_t cluster,
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

/* Internal open-file context */
typedef struct fat32_fctx {
    fat32_sb_t *sb;
    uint32_t    first_cluster;
    size_t      file_size;
    int         is_dir;
    /* Track "current cluster and offset within it" for sequential I/O */
    uint32_t    cur_cluster;
    size_t      cur_cluster_index;
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
    size_t target_index = offset / bytes_per_cluster;
    size_t target_off = offset % bytes_per_cluster;

    if (fc->cur_cluster && fc->cur_cluster_index == target_index) {
        fc->cluster_off = target_off;
        return fc->cur_cluster;
    }

    uint32_t cluster = fc->first_cluster;
    size_t start_index = 0;
    if (fc->cur_cluster && fc->cur_cluster_index <= target_index) {
        cluster = fc->cur_cluster;
        start_index = fc->cur_cluster_index;
    }
    if (!cluster)
        return 0;

    for (size_t i = start_index; i < target_index; i++) {
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
    fc->cur_cluster_index = target_index;
    fc->cluster_off = target_off;
    return cluster;
}

static void fat32_fctx_cache_pos(fat32_fctx_t *fc, uint32_t cluster,
                                 size_t cluster_index, size_t cluster_off) {
    if (cluster) {
        fc->cur_cluster = cluster;
        fc->cur_cluster_index = cluster_index;
        fc->cluster_off = cluster_off;
    } else {
        fc->cur_cluster = 0;
        fc->cur_cluster_index = 0;
        fc->cluster_off = 0;
    }
}

static int fat32_fread(vfile_t *vf, char *buf, size_t count) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    fat32_sb_t *sb = fc->sb;
    if (fc->is_dir) return -EISDIR;
    fat32_lock(sb);
    size_t fsize = vf->vnode->size;
    fc->file_size = fsize;
    if (fc->file_off >= fsize) {
        fat32_unlock(sb);
        return 0;
    }
    size_t remaining = fsize - fc->file_off;
    if (count > remaining) count = remaining;
    if (count == 0) {
        fat32_unlock(sb);
        return 0;
    }

    uint32_t cluster = fat32_fctx_cluster_at(fc, fc->file_off, 0);
    if (!cluster) {
        fat32_unlock(sb);
        return 0;
    }

    char *dst = buf;
    size_t done = 0;
    size_t cluster_index = fc->file_off / sb->bytes_per_cluster;
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
            cluster_index++;
            if (done < count)
                cluster = fat_read(sb, cluster);
        }
    }

    fc->file_off += done;
    vf->offset = fc->file_off;
    size_t cache_index = (off == 0 && cluster_index > 0) ? cluster_index - 1 : cluster_index;
    fat32_fctx_cache_pos(fc, cluster, cache_index, off);
    fat32_unlock(sb);
    return (int)done;
}

static int fat32_vn_readpage(vnode_t *vn, uint64_t index,
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

static int fat32_fwrite_unlocked(vfile_t *vf, const char *buf, size_t count) {
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
    size_t cluster_index = fc->file_off / bytes_per_cluster;
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
            cluster_index++;
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
    size_t cache_index = (off == 0 && cluster_index > 0) ? cluster_index - 1 : cluster_index;
    fat32_fctx_cache_pos(fc, cluster, cache_index, off);
    return (int)done;
}

static int fat32_fwrite(vfile_t *vf, const char *buf, size_t count) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    fat32_sb_t *sb = fc->sb;
    fat32_lock(sb);
    int r = fat32_fwrite_unlocked(vf, buf, count);
    fat32_unlock(sb);
    return r;
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

    fat32_lock(fp->sb);
    int r = fat32_fwrite_unlocked(&vf, (const char *)data, n);
    fat32_unlock(fp->sb);
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
    fat32_lock(fc->sb);

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

    fat32_unlock(fc->sb);
    return (int)total;
}

static int fat32_fclose(vfile_t *vf) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    if (fc)
        fat32_lock(fc->sb);
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
    if (fc)
        fat32_unlock(fc->sb);
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

static vfile_t *fat32_open_vnode(vnode_t *vn, int flags) {
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
    fc->cur_cluster_index = 0;
    fc->cluster_off   = 0;
    fc->dir_byte_off  = 0;

    vfile_t *vf = vfile_alloc();
    if (!vf) { kfree(fc); return NULL; }
    vf->vnode     = vn;
    vnode_get(vn);
    vf->flags     = flags;
    vf->offset    = fc->file_off;
    vfile_ref_init(vf, 1);
    vf->ops       = &g_fat32_fops;
    vf->priv      = fc;
    return vf;
}

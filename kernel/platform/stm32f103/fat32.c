/*
 * Compact FAT32 file layer. Pure logic over a fat32_io_t sector interface, so
 * it runs on the STM32 (SDIO) and on the host (a FAT32 image file). See
 * fat32.h. Names are matched/created in 8.3 short form; LFN entries are
 * skipped on read.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "fat32.h"

#define ATTR_DIRECTORY 0x10u
#define ATTR_VOLUME_ID 0x08u
#define ATTR_LONG_NAME 0x0Fu
#define ATTR_ARCHIVE 0x20u
#define FAT_EOC 0x0FFFFFF8u
#define FAT_MASK 0x0FFFFFFFu

/* ---- small freestanding helpers ---- */
static void u_memset(void *d, int v, unsigned n) {
    uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)v;
}
static void u_memcpy(void *d, const void *s, unsigned n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--)
        *p++ = *q++;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int rd_sector(fat32_fs_t *fs, uint32_t lba, uint8_t *buf) {
    return fs->io.read(fs->io.ctx, lba, buf, 1) ? FAT32_EIO : FAT32_OK;
}
static int wr_sector(fat32_fs_t *fs, uint32_t lba, const uint8_t *buf) {
    if (!fs->io.write)
        return FAT32_EROFS;
    return fs->io.write(fs->io.ctx, lba, buf, 1) ? FAT32_EIO : FAT32_OK;
}

static uint32_t cluster_lba(const fat32_fs_t *fs, uint32_t cluster) {
    return fs->cluster_begin_lba + (cluster - 2u) * fs->sectors_per_cluster;
}
static uint32_t cluster_bytes(const fat32_fs_t *fs) {
    return fs->sectors_per_cluster * fs->bytes_per_sector;
}

/* ---- FAT access (uses fs->scratch) ---- */
static int fat_get(fat32_fs_t *fs, uint32_t cluster, uint32_t *next) {
    uint32_t idx = cluster * 4u;
    uint32_t sec = fs->fat_begin_lba + idx / fs->bytes_per_sector;
    uint32_t off = idx % fs->bytes_per_sector;
    int r = rd_sector(fs, sec, fs->scratch);
    if (r)
        return r;
    *next = rd32(fs->scratch + off) & FAT_MASK;
    return FAT32_OK;
}
static int fat_set(fat32_fs_t *fs, uint32_t cluster, uint32_t val) {
    uint32_t idx = cluster * 4u;
    uint32_t off = idx % fs->bytes_per_sector;
    for (uint32_t f = 0; f < fs->num_fats; f++) {
        uint32_t sec = fs->fat_begin_lba + f * fs->fat_size_sectors +
                       idx / fs->bytes_per_sector;
        int r = rd_sector(fs, sec, fs->scratch);
        if (r)
            return r;
        uint32_t cur = rd32(fs->scratch + off);
        wr32(fs->scratch + off, (cur & 0xF0000000u) | (val & FAT_MASK));
        r = wr_sector(fs, sec, fs->scratch);
        if (r)
            return r;
    }
    return FAT32_OK;
}

/* Allocate one free cluster, mark EOC. If zero!=0, wipe its data area. */
static int fat_alloc(fat32_fs_t *fs, int zero, uint32_t *out) {
    for (uint32_t c = 2; c < fs->total_clusters + 2u; c++) {
        uint32_t nxt;
        int r = fat_get(fs, c, &nxt);
        if (r)
            return r;
        if (nxt == 0) {
            r = fat_set(fs, c, FAT_EOC);
            if (r)
                return r;
            if (zero) {
                uint8_t z[FAT32_SECTOR_SIZE];
                u_memset(z, 0, sizeof(z));
                for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
                    r = wr_sector(fs, cluster_lba(fs, c) + s, z);
                    if (r)
                        return r;
                }
            }
            *out = c;
            return FAT32_OK;
        }
    }
    return FAT32_ENOSPC;
}

static int free_chain(fat32_fs_t *fs, uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT_EOC) {
        uint32_t next;
        int r = fat_get(fs, cluster, &next);
        if (r)
            return r;
        r = fat_set(fs, cluster, 0);
        if (r)
            return r;
        cluster = next;
    }
    return FAT32_OK;
}

/* ---- 8.3 names ---- */
/* Convert one path component to an 11-byte space-padded uppercase 8.3 name. */
static int to_83(const char *name, unsigned len, uint8_t out[11]) {
    u_memset(out, ' ', 11);
    unsigned i = 0, o = 0;
    /* base */
    while (i < len && name[i] != '.') {
        if (o >= 8)
            return FAT32_EINVAL;
        char c = name[i++];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        out[o++] = (uint8_t)c;
    }
    if (i < len && name[i] == '.') {
        i++;
        unsigned e = 8;
        while (i < len) {
            if (e >= 11)
                return FAT32_EINVAL;
            char c = name[i++];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
            out[e++] = (uint8_t)c;
        }
    }
    if (o == 0)
        return FAT32_EINVAL;
    return FAT32_OK;
}

/* ---- directory scan ---- */
/* Iterate the entries of a directory cluster chain, matching name11. On match
 * copies the 32-byte entry to entry_out and reports its sector + index. dirbuf
 * is a 512-byte scratch owned by the caller. */
static int find_in_dir(fat32_fs_t *fs, uint32_t dir_cluster,
                       const uint8_t name11[11], uint8_t *dirbuf,
                       uint8_t entry_out[32], uint32_t *out_lba,
                       uint32_t *out_idx) {
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT_EOC) {
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t lba = cluster_lba(fs, cluster) + s;
            int r = rd_sector(fs, lba, dirbuf);
            if (r)
                return r;
            for (uint32_t e = 0; e < fs->bytes_per_sector / 32u; e++) {
                uint8_t *ent = dirbuf + e * 32u;
                if (ent[0] == 0x00)
                    return FAT32_ENOENT; /* end of directory */
                if (ent[0] == 0xE5)
                    continue;
                if ((ent[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME)
                    continue;
                if (ent[11] & ATTR_VOLUME_ID)
                    continue;
                int match = 1;
                for (int i = 0; i < 11; i++)
                    if (ent[i] != name11[i]) {
                        match = 0;
                        break;
                    }
                if (match) {
                    if (entry_out)
                        u_memcpy(entry_out, ent, 32);
                    if (out_lba)
                        *out_lba = lba;
                    if (out_idx)
                        *out_idx = e;
                    return FAT32_OK;
                }
            }
        }
        uint32_t next;
        int r = fat_get(fs, cluster, &next);
        if (r)
            return r;
        cluster = next;
    }
    return FAT32_ENOENT;
}

/* Split a path, walk from root through all but the last component (each must be
 * a directory), returning the parent's first cluster + the 8.3 last name. */
static int resolve_parent(fat32_fs_t *fs, const char *path, uint8_t *dirbuf,
                          uint32_t *parent_cluster, uint8_t name11[11]) {
    while (*path == '/')
        path++;
    if (!*path)
        return FAT32_EINVAL;

    uint32_t cluster = fs->root_cluster;
    const char *seg = path;
    for (;;) {
        const char *slash = seg;
        while (*slash && *slash != '/')
            slash++;
        unsigned len = (unsigned)(slash - seg);
        if (len == 0)
            return FAT32_EINVAL;

        /* skip any trailing slashes to see if this is the final component */
        const char *rest = slash;
        while (*rest == '/')
            rest++;
        int is_last = (*rest == '\0');

        int r = to_83(seg, len, name11);
        if (r)
            return r;
        if (is_last) {
            *parent_cluster = cluster;
            return FAT32_OK;
        }
        /* descend into this directory */
        uint8_t ent[32];
        r = find_in_dir(fs, cluster, name11, dirbuf, ent, 0, 0);
        if (r)
            return r;
        if (!(ent[11] & ATTR_DIRECTORY))
            return FAT32_ENOTDIR;
        cluster = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
        if (cluster == 0)
            cluster = fs->root_cluster;
        seg = rest;
    }
}

/* ---- mount ---- */
int fat32_mount(fat32_fs_t *fs, const fat32_io_t *io, uint32_t partition_lba) {
    if (!fs || !io || !io->read)
        return FAT32_EINVAL;
    u_memset(fs, 0, sizeof(*fs));
    fs->io = *io;
    fs->partition_lba = partition_lba;

    uint8_t bs[FAT32_SECTOR_SIZE];
    if (fs->io.read(fs->io.ctx, partition_lba, bs, 1))
        return FAT32_EIO;
    if (rd16(bs + 510) != 0xAA55u)
        return FAT32_EINVAL;

    fs->bytes_per_sector = rd16(bs + 11);
    fs->sectors_per_cluster = bs[13];
    uint32_t reserved = rd16(bs + 14);
    fs->num_fats = bs[16];
    fs->fat_size_sectors = rd32(bs + 36);
    fs->root_cluster = rd32(bs + 44);
    if (fs->bytes_per_sector != FAT32_SECTOR_SIZE || !fs->sectors_per_cluster ||
        !fs->num_fats || !fs->fat_size_sectors || fs->root_cluster < 2)
        return FAT32_EINVAL; /* not a FAT32 volume we can handle */

    uint32_t tot = rd32(bs + 32);
    if (tot == 0)
        tot = rd16(bs + 19);
    fs->fat_begin_lba = partition_lba + reserved;
    fs->cluster_begin_lba =
        partition_lba + reserved + fs->num_fats * fs->fat_size_sectors;
    uint32_t data_sectors =
        tot - (reserved + fs->num_fats * fs->fat_size_sectors);
    fs->total_clusters = data_sectors / fs->sectors_per_cluster;
    return FAT32_OK;
}

/* ---- open (read) ---- */
int fat32_open(fat32_fs_t *fs, const char *path, fat32_file_t *f) {
    if (!fs || !path || !f)
        return FAT32_EINVAL;
    uint8_t dirbuf[FAT32_SECTOR_SIZE];
    uint8_t name11[11];
    uint32_t parent;
    int r = resolve_parent(fs, path, dirbuf, &parent, name11);
    if (r)
        return r;
    uint8_t ent[32];
    uint32_t lba, idx;
    r = find_in_dir(fs, parent, name11, dirbuf, ent, &lba, &idx);
    if (r)
        return r;
    if (ent[11] & ATTR_DIRECTORY)
        return FAT32_EINVAL;

    u_memset(f, 0, sizeof(*f));
    f->fs = fs;
    f->first_cluster = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
    f->size = rd32(ent + 28);
    f->pos = 0;
    f->cur_cluster = f->first_cluster;
    f->dir_lba = lba;
    f->dir_index = idx;
    f->writable = 0;
    return FAT32_OK;
}

/* Ensure f->cur_cluster is the cluster containing f->pos (walk from start). */
static int seek_cluster(fat32_file_t *f) {
    fat32_fs_t *fs = f->fs;
    uint32_t hops = f->pos / cluster_bytes(fs);
    uint32_t cl = f->first_cluster;
    for (uint32_t i = 0; i < hops && cl >= 2 && cl < FAT_EOC; i++) {
        uint32_t next;
        int r = fat_get(fs, cl, &next);
        if (r)
            return r;
        cl = next;
    }
    f->cur_cluster = cl;
    return FAT32_OK;
}

int fat32_read(fat32_file_t *f, void *buf, uint32_t len) {
    if (!f || !buf)
        return FAT32_EINVAL;
    fat32_fs_t *fs = f->fs;
    if (f->pos >= f->size)
        return 0;
    if (len > f->size - f->pos)
        len = f->size - f->pos;

    uint8_t *out = buf;
    uint32_t bpc = cluster_bytes(fs);
    uint32_t total = 0;
    int r = seek_cluster(f);
    if (r)
        return r;

    while (len > 0) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT_EOC)
            break;
        uint32_t within = f->pos % bpc;
        uint32_t sec = within / fs->bytes_per_sector;
        uint32_t off = f->pos % fs->bytes_per_sector;
        uint32_t lba = cluster_lba(fs, f->cur_cluster) + sec;
        r = rd_sector(fs, lba, fs->scratch);
        if (r)
            return r;
        uint32_t n = fs->bytes_per_sector - off;
        if (n > len)
            n = len;
        u_memcpy(out, fs->scratch + off, n);
        out += n;
        f->pos += n;
        len -= n;
        total += n;
        if (f->pos % bpc == 0) { /* advance to next cluster */
            uint32_t next;
            r = fat_get(fs, f->cur_cluster, &next);
            if (r)
                return r;
            f->cur_cluster = next;
        }
    }
    return (int)total;
}

/* Ensure the cluster containing f->pos exists, allocating/linking as needed. */
static int ensure_cluster(fat32_file_t *f) {
    fat32_fs_t *fs = f->fs;
    uint32_t hops = f->pos / cluster_bytes(fs);

    if (f->first_cluster == 0) {
        uint32_t c;
        int r = fat_alloc(fs, 0, &c);
        if (r)
            return r;
        f->first_cluster = c;
        f->dirty = 1;
    }
    uint32_t cl = f->first_cluster;
    for (uint32_t i = 0; i < hops; i++) {
        uint32_t next;
        int r = fat_get(fs, cl, &next);
        if (r)
            return r;
        if (next >= FAT_EOC) {
            uint32_t nc;
            r = fat_alloc(fs, 0, &nc);
            if (r)
                return r;
            r = fat_set(fs, cl, nc);
            if (r)
                return r;
            next = nc;
        }
        cl = next;
    }
    f->cur_cluster = cl;
    return FAT32_OK;
}

int fat32_write(fat32_file_t *f, const void *buf, uint32_t len) {
    if (!f || !buf)
        return FAT32_EINVAL;
    if (!f->writable)
        return FAT32_EROFS;
    fat32_fs_t *fs = f->fs;
    if (!fs->io.write)
        return FAT32_EROFS;

    const uint8_t *in = buf;
    uint32_t bpc = cluster_bytes(fs);
    uint32_t total = 0;

    while (len > 0) {
        int r = ensure_cluster(f);
        if (r)
            return r;
        uint32_t within = f->pos % bpc;
        uint32_t sec = within / fs->bytes_per_sector;
        uint32_t off = f->pos % fs->bytes_per_sector;
        uint32_t lba = cluster_lba(fs, f->cur_cluster) + sec;
        uint32_t n = fs->bytes_per_sector - off;
        if (n > len)
            n = len;
        if (off != 0 || n != fs->bytes_per_sector) {
            r = rd_sector(fs, lba, fs->scratch); /* read-modify-write */
            if (r)
                return r;
        }
        u_memcpy(fs->scratch + off, in, n);
        r = wr_sector(fs, lba, fs->scratch);
        if (r)
            return r;
        in += n;
        f->pos += n;
        len -= n;
        total += n;
        if (f->pos > f->size) {
            f->size = f->pos;
            f->dirty = 1;
        }
    }
    return (int)total;
}

int fat32_seek(fat32_file_t *f, uint32_t pos) {
    if (!f)
        return FAT32_EINVAL;
    if (pos > f->size)
        pos = f->size;
    f->pos = pos;
    return seek_cluster(f);
}

uint32_t fat32_tell(const fat32_file_t *f) { return f ? f->pos : 0; }
uint32_t fat32_size(const fat32_file_t *f) { return f ? f->size : 0; }

/* Patch the file's short dir entry with its first cluster + size. */
static int flush_dir_entry(fat32_file_t *f) {
    fat32_fs_t *fs = f->fs;
    int r = rd_sector(fs, f->dir_lba, fs->scratch);
    if (r)
        return r;
    uint8_t *ent = fs->scratch + f->dir_index * 32u;
    wr16(ent + 20, (uint16_t)(f->first_cluster >> 16));
    wr16(ent + 26, (uint16_t)(f->first_cluster & 0xFFFFu));
    wr32(ent + 28, f->size);
    return wr_sector(fs, f->dir_lba, fs->scratch);
}

int fat32_close(fat32_file_t *f) {
    if (!f)
        return FAT32_EINVAL;
    int r = FAT32_OK;
    if (f->writable && f->dirty)
        r = flush_dir_entry(f);
    f->fs = 0;
    return r;
}

/* Find or extend a free 32-byte slot in a directory; returns its sector+index.
 * dirbuf is caller-owned 512-byte scratch. */
static int alloc_dir_slot(fat32_fs_t *fs, uint32_t dir_cluster, uint8_t *dirbuf,
                          uint32_t *out_lba, uint32_t *out_idx) {
    uint32_t cluster = dir_cluster, prev = dir_cluster;
    while (cluster >= 2 && cluster < FAT_EOC) {
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t lba = cluster_lba(fs, cluster) + s;
            int r = rd_sector(fs, lba, dirbuf);
            if (r)
                return r;
            for (uint32_t e = 0; e < fs->bytes_per_sector / 32u; e++) {
                uint8_t c0 = dirbuf[e * 32u];
                if (c0 == 0x00 || c0 == 0xE5) {
                    *out_lba = lba;
                    *out_idx = e;
                    return FAT32_OK;
                }
            }
        }
        prev = cluster;
        int r = fat_get(fs, cluster, &cluster);
        if (r)
            return r;
    }
    /* directory full -> extend with a fresh zeroed cluster */
    uint32_t nc;
    int r = fat_alloc(fs, 1, &nc);
    if (r)
        return r;
    r = fat_set(fs, prev, nc);
    if (r)
        return r;
    *out_lba = cluster_lba(fs, nc);
    *out_idx = 0;
    return FAT32_OK;
}

static void write_short_entry(uint8_t *ent, const uint8_t name11[11],
                              uint8_t attr, uint32_t cluster, uint32_t size) {
    u_memset(ent, 0, 32);
    u_memcpy(ent, name11, 11);
    ent[11] = attr;
    wr16(ent + 20, (uint16_t)(cluster >> 16));
    wr16(ent + 26, (uint16_t)(cluster & 0xFFFFu));
    wr32(ent + 28, size);
}

int fat32_create(fat32_fs_t *fs, const char *path, fat32_file_t *f) {
    if (!fs || !path || !f)
        return FAT32_EINVAL;
    if (!fs->io.write)
        return FAT32_EROFS;
    uint8_t dirbuf[FAT32_SECTOR_SIZE];
    uint8_t name11[11];
    uint32_t parent;
    int r = resolve_parent(fs, path, dirbuf, &parent, name11);
    if (r)
        return r;

    uint8_t ent[32];
    uint32_t lba, idx;
    r = find_in_dir(fs, parent, name11, dirbuf, ent, &lba, &idx);
    if (r == FAT32_OK) {
        /* exists -> truncate */
        if (ent[11] & ATTR_DIRECTORY)
            return FAT32_EINVAL;
        uint32_t first = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
        if (first >= 2) {
            r = free_chain(fs, first);
            if (r)
                return r;
        }
    } else if (r == FAT32_ENOENT) {
        r = alloc_dir_slot(fs, parent, dirbuf, &lba, &idx);
        if (r)
            return r;
    } else {
        return r;
    }

    /* (re)write the short entry as an empty file */
    r = rd_sector(fs, lba, dirbuf);
    if (r)
        return r;
    write_short_entry(dirbuf + idx * 32u, name11, ATTR_ARCHIVE, 0, 0);
    r = wr_sector(fs, lba, dirbuf);
    if (r)
        return r;

    u_memset(f, 0, sizeof(*f));
    f->fs = fs;
    f->first_cluster = 0;
    f->size = 0;
    f->pos = 0;
    f->cur_cluster = 0;
    f->dir_lba = lba;
    f->dir_index = idx;
    f->writable = 1;
    f->dirty = 0;
    return FAT32_OK;
}

int fat32_append(fat32_fs_t *fs, const char *path, fat32_file_t *f) {
    int r = fat32_open(fs, path, f);
    if (r == FAT32_ENOENT)
        return fat32_create(fs, path, f); /* new file -> empty, writable */
    if (r != FAT32_OK)
        return r;
    f->writable = 1;
    return fat32_seek(f, f->size); /* position at EOF for appending */
}

int fat32_mkdir(fat32_fs_t *fs, const char *path) {
    if (!fs || !path)
        return FAT32_EINVAL;
    if (!fs->io.write)
        return FAT32_EROFS;
    uint8_t dirbuf[FAT32_SECTOR_SIZE];
    uint8_t name11[11];
    uint32_t parent;
    int r = resolve_parent(fs, path, dirbuf, &parent, name11);
    if (r)
        return r;

    uint8_t ent[32];
    r = find_in_dir(fs, parent, name11, dirbuf, ent, 0, 0);
    if (r == FAT32_OK)
        return FAT32_EEXIST;
    if (r != FAT32_ENOENT)
        return r;

    uint32_t newc;
    r = fat_alloc(fs, 1, &newc); /* zeroed cluster */
    if (r)
        return r;

    /* "." and ".." entries in the new directory */
    uint8_t dot[11], dotdot[11];
    u_memset(dot, ' ', 11);
    dot[0] = '.';
    u_memset(dotdot, ' ', 11);
    dotdot[0] = '.';
    dotdot[1] = '.';
    u_memset(dirbuf, 0, fs->bytes_per_sector);
    write_short_entry(dirbuf, dot, ATTR_DIRECTORY, newc, 0);
    uint32_t pp = (parent == fs->root_cluster) ? 0 : parent;
    write_short_entry(dirbuf + 32, dotdot, ATTR_DIRECTORY, pp, 0);
    r = wr_sector(fs, cluster_lba(fs, newc), dirbuf);
    if (r)
        return r;

    /* link into the parent */
    uint32_t lba, idx;
    r = alloc_dir_slot(fs, parent, dirbuf, &lba, &idx);
    if (r)
        return r;
    r = rd_sector(fs, lba, dirbuf);
    if (r)
        return r;
    write_short_entry(dirbuf + idx * 32u, name11, ATTR_DIRECTORY, newc, 0);
    return wr_sector(fs, lba, dirbuf);
}

int fat32_unlink(fat32_fs_t *fs, const char *path) {
    if (!fs || !path)
        return FAT32_EINVAL;
    if (!fs->io.write)
        return FAT32_EROFS;
    uint8_t dirbuf[FAT32_SECTOR_SIZE];
    uint8_t name11[11];
    uint32_t parent;
    int r = resolve_parent(fs, path, dirbuf, &parent, name11);
    if (r)
        return r;
    uint8_t ent[32];
    uint32_t lba, idx;
    r = find_in_dir(fs, parent, name11, dirbuf, ent, &lba, &idx);
    if (r)
        return r;
    if (ent[11] & ATTR_DIRECTORY)
        return FAT32_EINVAL;
    uint32_t first = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
    if (first >= 2) {
        r = free_chain(fs, first);
        if (r)
            return r;
    }
    r = rd_sector(fs, lba, dirbuf);
    if (r)
        return r;
    dirbuf[idx * 32u] = 0xE5;
    return wr_sector(fs, lba, dirbuf);
}

int fat32_stat(fat32_fs_t *fs, const char *path, int *is_dir, uint32_t *size) {
    if (!fs || !path)
        return FAT32_EINVAL;
    uint8_t dirbuf[FAT32_SECTOR_SIZE];
    uint8_t name11[11];
    uint32_t parent;
    int r = resolve_parent(fs, path, dirbuf, &parent, name11);
    if (r)
        return r;
    uint8_t ent[32];
    r = find_in_dir(fs, parent, name11, dirbuf, ent, 0, 0);
    if (r)
        return r;
    if (is_dir)
        *is_dir = (ent[11] & ATTR_DIRECTORY) ? 1 : 0;
    if (size)
        *size = rd32(ent + 28);
    return FAT32_OK;
}

#endif /* CONFIG_BOARD_STM32F103 */

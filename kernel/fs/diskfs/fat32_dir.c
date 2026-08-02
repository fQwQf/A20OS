#include "fs/fat32.h"
#include "fs/fat32_internal.h"
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

/* FAT32 directory entry helpers: 8.3 + LFN encoding and lookup. */

int read_raw_dirent(fat32_sb_t *sb, uint32_t dir_cluster,
                            size_t byte_off, fat32_dirent_t *de) {
    return fat32_chain_read(sb, dir_cluster, byte_off, de, sizeof(*de));
}


void decode_8_3(const uint8_t *raw, char *out) {
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


void lfn_append_seg(lfn_buf_t *lb, const fat32_lfn_t *lfn) {
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


uint32_t fat32_dir_lookup(fat32_sb_t *sb, uint32_t dir_cluster,
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


void encode_83_name(const char *name, uint8_t out[11]) {
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


int fat32_dir_write(fat32_sb_t *sb, uint32_t dir_cluster,
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


int fat32_short_name_exists(fat32_sb_t *sb, uint32_t dir_cluster,
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


int fat32_make_short_name(fat32_sb_t *sb, uint32_t dir_cluster,
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


uint8_t fat32_short_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}


void fat32_lfn_set_char(fat32_lfn_t *lfn, int index, uint16_t value) {
    if (index < 5)
        lfn->name1[index] = value;
    else if (index < 11)
        lfn->name2[index - 5] = value;
    else
        lfn->name3[index - 11] = value;
}


int fat32_create_dirents(fat32_sb_t *sb, uint32_t dir_cluster,
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


void fat32_delete_dirents(fat32_sb_t *sb, uint32_t dir_cluster,
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


int fat32_dir_is_empty(fat32_sb_t *sb, uint32_t dir_cluster) {
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

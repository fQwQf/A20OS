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

/* FAT32 file I/O. */

uint32_t fat32_fctx_cluster_at(fat32_fctx_t *fc, size_t offset, int extend) {
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


void fat32_fctx_cache_pos(fat32_fctx_t *fc, uint32_t cluster,
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


int fat32_fread(vfile_t *vf, char *buf, size_t count) {
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


int fat32_fwrite_unlocked(vfile_t *vf, const char *buf, size_t count) {
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


int fat32_fwrite(vfile_t *vf, const char *buf, size_t count) {
    fat32_fctx_t *fc = (fat32_fctx_t *)vf->priv;
    fat32_sb_t *sb = fc->sb;
    fat32_lock(sb);
    int r = fat32_fwrite_unlocked(vf, buf, count);
    fat32_unlock(sb);
    return r;
}


long fat32_flseek(vfile_t *vf, long offset, int whence) {
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


int fat32_freaddir(vfile_t *vf, void *dirp, size_t count) {
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


int fat32_fclose(vfile_t *vf) {
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

vfile_ops_t g_fat32_fops = {
    .read    = fat32_fread,
    .write   = fat32_fwrite,
    .lseek   = fat32_flseek,
    .readdir = fat32_freaddir,
    .ioctl   = NULL,
    .close   = fat32_fclose,
};


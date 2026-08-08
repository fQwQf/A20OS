#ifndef _FAT32_INTERNAL_H
#define _FAT32_INTERNAL_H

#include "fs/fat32.h"

#include "fs/vfs.h"
#include "fs/file.h"
#include "core/sync.h"

struct vnode;
struct vfile;

#define LFN_MAX_SEGS  20


typedef struct {
    char name[13 * LFN_MAX_SEGS + 1];  /* max LFN = 255 chars */
    int  valid;
} lfn_buf_t;

/* Internal: FAT32 vnode private data */
typedef struct fat32_vnode_priv {
    fat32_sb_t *sb;
    uint32_t    first_cluster;
    size_t      file_size;
    int         is_dir;
    int         unlinked;    /* directory entry removed; free clusters on release */
} fat32_vnode_priv_t;


#define FAT32_META_MAX 1024
typedef struct fat32_meta {
    fat32_sb_t *sb;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    int used;
} fat32_meta_t;

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

uint64_t cluster_to_lba(fat32_sb_t *sb, uint32_t cluster);
uint64_t cluster_byte_offset(fat32_sb_t *sb, uint32_t cluster);
uint64_t fat_entry_offset(fat32_sb_t *sb, uint32_t cluster);
uint32_t fat_read(fat32_sb_t *sb, uint32_t cluster);
void fat_write(fat32_sb_t *sb, uint32_t cluster, uint32_t next);
int fat32_chain_read(fat32_sb_t *sb, uint32_t first_cluster,
                             size_t offset, void *buf, size_t len);
uint32_t fat32_alloc_cluster(fat32_sb_t *sb);
uint32_t fat32_extend_chain(fat32_sb_t *sb, uint32_t last_cluster);
int read_raw_dirent(fat32_sb_t *sb, uint32_t dir_cluster,
                            size_t byte_off, fat32_dirent_t *de);
void decode_8_3(const uint8_t *raw, char *out);
void lfn_append_seg(lfn_buf_t *lb, const fat32_lfn_t *lfn);
uint32_t fat32_dir_lookup(fat32_sb_t *sb, uint32_t dir_cluster,
                                  const char *name, int *is_dir,
                                  size_t *out_size, size_t *dirent_off);
void encode_83_name(const char *name, uint8_t out[11]);
int fat32_dir_write(fat32_sb_t *sb, uint32_t dir_cluster,
                           size_t off, const void *entry);
int fat32_short_name_exists(fat32_sb_t *sb, uint32_t dir_cluster,
                                   const uint8_t name[11]);
int fat32_make_short_name(fat32_sb_t *sb, uint32_t dir_cluster,
                                 const char *name, uint8_t out[11],
                                 int *needs_lfn);
uint8_t fat32_short_checksum(const uint8_t name[11]);
void fat32_lfn_set_char(fat32_lfn_t *lfn, int index, uint16_t value);
int fat32_create_dirents(fat32_sb_t *sb, uint32_t dir_cluster,
                                const char *name, uint8_t attr,
                                uint32_t first_cluster);
void fat32_delete_dirents(fat32_sb_t *sb, uint32_t dir_cluster,
                                 size_t short_off);
void fat32_lock(fat32_sb_t *sb);
void fat32_unlock(fat32_sb_t *sb);
vnode_t *fat32_vcache_find(fat32_sb_t *sb, uint64_t ino);
void fat32_vcache_add(fat32_sb_t *sb, uint64_t ino, vnode_t *vn);
vnode_t *fat32_vcache_remove(fat32_sb_t *sb, uint64_t ino);
void fat32_free_cluster_chain(fat32_sb_t *sb, uint32_t cluster);
int fat32_vn_writepage(vnode_t *vn, uint64_t index,
                              const void *data, size_t len);
fat32_meta_t *fat32_get_meta(fat32_sb_t *sb, uint64_t ino, int is_dir, int create);
void fat32_drop_meta(fat32_sb_t *sb, uint64_t ino);
vnode_t *fat32_make_vnode(fat32_sb_t *sb, uint32_t cluster,
                                  size_t size, int is_dir, vnode_t *parent,
                                  uint64_t ino);
int fat32_lookup(vnode_t *dir, const char *name, vnode_t **out);
int fat32_stat(vnode_t *vn, kstat_t *st);
void fat32_release_vn(vnode_t *vn);
int fat32_vn_mkdir(vnode_t *dir, const char *name, int mode);
int fat32_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out);
int fat32_vn_unlink(vnode_t *dir, const char *name);
int fat32_vn_truncate(vnode_t *vn, size_t size);
int fat32_vn_chmod(vnode_t *vn, int mode);
int fat32_vn_chown(vnode_t *vn, int uid, int gid);
int fat32_dir_is_empty(fat32_sb_t *sb, uint32_t dir_cluster);
int fat32_vn_rmdir(vnode_t *dir, const char *name);
int fat32_vn_rename(vnode_t *old_dir, const char *old_name,
                           vnode_t *new_dir, const char *new_name,
                           unsigned int flags);
int fat32_vn_readpage(vnode_t *vn, uint64_t index,
                             void *data, size_t len);
int fat32_vn_statfs(vnode_t *vn, kstatfs_t *st);
uint32_t fat32_fctx_cluster_at(fat32_fctx_t *fc, size_t offset, int extend);
void fat32_fctx_cache_pos(fat32_fctx_t *fc, uint32_t cluster,
                                 size_t cluster_index, size_t cluster_off);
int fat32_fread(vfile_t *vf, char *buf, size_t count);
int fat32_fwrite_unlocked(vfile_t *vf, const char *buf, size_t count);
int fat32_fwrite(vfile_t *vf, const char *buf, size_t count);
long fat32_flseek(vfile_t *vf, long offset, int whence);
int fat32_freaddir(vfile_t *vf, void *dirp, size_t count);
int fat32_fclose(vfile_t *vf);
vnode_t *fat32_mount(bcache_t *bc);
void fat32_unmount(vnode_t *root);
vfile_t *fat32_open_vnode(vnode_t *vn, int flags);

extern vnode_ops_t g_fat32_vnode_ops;
extern vfile_ops_t g_fat32_fops;
#endif /* _FAT32_INTERNAL_H */

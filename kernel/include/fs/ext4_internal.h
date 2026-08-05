#ifndef _EXT4_INTERNAL_H
#define _EXT4_INTERNAL_H

#include "fs/ext4.h"

struct vfile;
struct vnode;

/*
 * Internal helpers shared across fs/diskfs/ext4*.c translation units.
 * The public on-disk structures and ext4_mount/ext4_unmount live in ext4.h.
 */

/* Flattened extent used by the extent-truncate coalescing helpers. */
typedef struct {
    uint32_t start;
    uint32_t len;
    uint64_t phys;
} ext4_flatext_t;

#define EXT4_MAX_FLATEXT (4 * 512)   /* 4 depth-1 leaves × max entries */

static inline uint64_t ext4_inode_size(const ext4_inode_t *in) {
    return ((uint64_t)in->i_size_high << 32) | in->i_size_lo;
}
static inline void ext4_inode_set_size(ext4_inode_t *in, uint64_t size) {
    in->i_size_lo  = (uint32_t)(size & 0xffffffffu);
    in->i_size_high = (uint32_t)(size >> 32);
}

int ext4_vn_writepage(vnode_t *vn, uint64_t index,
                             const void *data, size_t len)
;
int ext4_vn_readpage(vnode_t *vn, uint64_t index,
                            void *data, size_t len)
;
ext4_inode_t *ext4_fctx_inode(ext4_fctx_t *fc) ;
void ext4_fctx_inode_dirty(ext4_fctx_t *fc) ;
uint64_t ext4_block_map_cached(ext4_fctx_t *fc, ext4_inode_t *inode,
                                       uint32_t lblk) ;
/* Returns a caller-owned vnode reference on a cache hit. */
vnode_t *ext4_vnode_cache_lookup(ext4_sb_info_t *sb, uint32_t ino) ;
void ext4_vnode_cache_insert(ext4_sb_info_t *sb, uint32_t ino, vnode_t *vn) ;
vnode_t *ext4_vnode_cache_remove(ext4_sb_info_t *sb, uint32_t ino) ;
int ext4_read_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *out) ;
int ext4_write_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *inp) ;
void ext4_writeback_gd(ext4_sb_info_t *sb, uint32_t group) ;
int ext4_bitmap_alloc(ext4_sb_info_t *sb, uint64_t bm_blk, uint32_t max) ;
void ext4_bitmap_free(ext4_sb_info_t *sb, uint64_t bm_blk, uint32_t bit) ;
uint64_t ext4_alloc_block(ext4_sb_info_t *sb) ;
void ext4_free_block(ext4_sb_info_t *sb, uint64_t phys) ;
uint32_t ext4_alloc_inode(ext4_sb_info_t *sb) ;
void ext4_free_inode(ext4_sb_info_t *sb, uint32_t ino) ;
uint64_t ext4_extent_leaf_search(ext4_extent_t *ex, int cnt, uint32_t lblk) ;
uint64_t ext4_extent_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) ;
int ext4_extent_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                             uint32_t lblk, uint64_t pb) ;
void ext4_extent_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) ;
int ext4_extent_collect(ext4_sb_info_t *sb, const ext4_inode_t *inode,
                               ext4_flatext_t *out, int max) ;
void ext4_extent_free_tree(ext4_sb_info_t *sb, const ext4_inode_t *inode) ;
int ext4_extent_truncate_at(ext4_sb_info_t *sb, ext4_inode_t *inode,
                                   uint32_t lblk) ;
uint64_t ext4_indirect_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) ;
int ext4_indirect_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                               uint32_t lblk, uint64_t phys) ;
void ext4_indirect_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) ;
void ext4_indirect_truncate_at(ext4_sb_info_t *sb, ext4_inode_t *inode,
                                      uint32_t lblk) ;
uint64_t ext4_block_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) ;
int ext4_block_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                            uint32_t lblk, uint64_t phys) ;
void ext4_block_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) ;
void ext4_block_truncate_at(ext4_sb_info_t *sb, ext4_inode_t *inode,
                                   uint32_t lblk) ;
int ext4_dir_find(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz,
                          const char *name, uint32_t *out_ino, uint8_t *out_ft) ;
int ext4_dir_add(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t *dsz,
                         const char *name, uint32_t ino, uint8_t ft) ;
int ext4_dir_remove(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz,
                            const char *name) ;
int ext4_dir_update_entry(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t *dsz,
                                  const char *name, uint32_t ino, uint8_t ft) ;
int ext4_inode_remove(ext4_sb_info_t *sb, uint32_t dir_ino __attribute__((unused)),
                              ext4_inode_t *di, const char *name, uint32_t ino,
                              vnode_t **deferred_put) ;
int ext4_lookup_unlocked(vnode_t *dir, const char *name, vnode_t **out) ;
int ext4_stat(vnode_t *vn, kstat_t *st) ;
void ext4_release_vn(vnode_t *vn) ;
int ext4_vn_create_unlocked(vnode_t *dir, const char *name, int mode, vnode_t **out) ;
int ext4_vn_mkdir_unlocked(vnode_t *dir, const char *name, int mode) ;
int ext4_vn_link_unlocked(vnode_t *dir, const char *name, vnode_t *target) ;
int ext4_vn_link(vnode_t *dir, const char *name, vnode_t *target) ;
int ext4_vn_unlink_unlocked(vnode_t *dir, const char *name, vnode_t **deferred_put) ;
int ext4_dir_empty(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz) ;
int ext4_vn_rmdir_unlocked(vnode_t *dir, const char *name, vnode_t **deferred_put) ;
int ext4_vn_rename_unlocked(vnode_t *old_dir, const char *old_name,
                                   vnode_t *new_dir, const char *new_name,
                                   unsigned int flags, vnode_t **deferred_put,
                                   vnode_t **deferred_parent_put) ;
int ext4_readlink(vnode_t *vn, char *buf, size_t sz) ;
int ext4_vn_symlink_unlocked(vnode_t *dir, const char *name,
                                    const char *target);
int ext4_vn_truncate_unlocked(vnode_t *vn, size_t size) ;
int ext4_vn_chmod(vnode_t *vn, int mode) ;
int ext4_vn_chown(vnode_t *vn, int uid, int gid) ;
int ext4_lookup(vnode_t *dir, const char *name, vnode_t **out)
;
int ext4_vn_create(vnode_t *dir, const char *name, int mode,
                          vnode_t **out)
;
int ext4_vn_mkdir(vnode_t *dir, const char *name, int mode)
;
int ext4_vn_unlink(vnode_t *dir, const char *name)
;
int ext4_vn_rmdir(vnode_t *dir, const char *name)
;
int ext4_vn_rename(vnode_t *old_dir, const char *old_name,
                          vnode_t *new_dir, const char *new_name,
                          unsigned int flags)
;
int ext4_vn_symlink(vnode_t *dir, const char *name,
                           const char *target)
;
int ext4_vn_truncate(vnode_t *vn, size_t size)
;
int ext4_vn_statfs(vnode_t *vn, kstatfs_t *st)
;
void ext4_fctx_refresh_size(vfile_t *vf, ext4_fctx_t *fc) ;
void ext4_fctx_set_size(vfile_t *vf, ext4_fctx_t *fc, uint64_t size) ;
int ext4_fread(vfile_t *vf, char *buf, size_t count) ;
int ext4_fwrite(vfile_t *vf, const char *buf, size_t count) ;
long ext4_flseek(vfile_t *vf, long offset, int whence) ;
int ext4_freaddir(vfile_t *vf, void *dirp, size_t count) ;
int ext4_fclose(vfile_t *vf) ;
vnode_t *ext4_make_vnode(ext4_sb_info_t *sb, uint32_t ino, uint32_t sz,
                                 int type, vnode_t *parent) ;
vnode_t *ext4_mount(bcache_t *bc) ;
void ext4_unmount(vnode_t *root) ;
vfile_t *ext4_open_vnode(vnode_t *vn, int flags) ;

extern vnode_ops_t g_ext4_vnode_ops;
extern vfile_ops_t g_ext4_fops;
#endif /* _EXT4_INTERNAL_H */

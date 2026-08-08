#ifndef _NTFS_INTERNAL_H
#define _NTFS_INTERNAL_H

#include "fs/ntfs.h"
#include "fs/ntfs_format.h"

#include "core/consts.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "fs/file.h"

struct vnode;
struct vfile;

/* Byte/VLI helpers live in ntfs_format.h; keep the local names. */
#define nget16 nf_get16
#define nget32 nf_get32
#define nget64 nf_get64
#define nput16 nf_put16
#define nput32 nf_put32
#define nput64 nf_put64
#define nvli_len nf_vli_len
#define nvli_len_s nf_vli_len_s
#define nenc_vli nf_encode_vli

#define nput64 nf_put64
#define nvli_len nf_vli_len
#define nvli_len_s nf_vli_len_s
#define nenc_vli nf_encode_vli

/* ------------------------------------------------------------------ */
/* On-disk constants                                                   */
/* ------------------------------------------------------------------ */

#define NTFS_AT_STD_INFO     0x10
#define NTFS_AT_ATTR_LIST    0x20
#define NTFS_AT_FILE_NAME    0x30
#define NTFS_AT_DATA         0x80
#define NTFS_AT_INDEX_ROOT   0x90
#define NTFS_AT_INDEX_ALLOC  0xA0
#define NTFS_AT_BITMAP       0xB0
#define NTFS_AT_END          0xFFFFFFFF

#define NTFS_REC_IN_USE 0x0001
#define NTFS_REC_IS_DIR 0x0002

#define NTFS_IDX_ENTRY_NODE 0x01
#define NTFS_IDX_ENTRY_LAST 0x02

#define NTFS_ATTR_COMPRESSED 0x0001
#define NTFS_ATTR_ENCRYPTED  0x4000

#define NTFS_MFT_REC_BITMAP 6
#define NTFS_MFT_REC_ROOT   5

#define NTFS_FILE_NAME_POSIX 1
#define NTFS_ATTR_HEADER_RES 24


/* In-memory structures                                                */
/* ------------------------------------------------------------------ */

typedef struct ntfs_sb {
    struct bcache *bc;
    mutex_t lock;

    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint64_t bytes_per_cluster;
    uint64_t total_clusters;
    uint64_t mft_lcn;
    uint64_t mft_record_size;
    uint64_t index_record_size;

    ntfs_run_t *mft_runs;       /* $MFT unnamed $DATA run list */
    uint32_t    mft_run_count;
    uint64_t    mft_data_size;
} ntfs_sb_t;

typedef struct ntfs_vnode_priv {
    ntfs_sb_t *sb;
    uint64_t   mft_index;
    uint32_t   seq;
    int        is_dir;
    /* Cached $DATA location (regular files only). */
    int        data_resident;
    uint32_t   data_res_off;    /* offset within MFT record */
    uint32_t   data_res_len;
    ntfs_run_t *runs;
    uint32_t   run_count;
    uint64_t   data_size;
} ntfs_vnode_priv_t;

typedef struct ntfs_dir_entry {
    char     name[256];
    int      is_dir;
    uint64_t ref;
    uint64_t size;
} ntfs_dir_entry_t;

typedef struct ntfs_fctx {
    ntfs_sb_t *sb;
    uint64_t   mft_index;
    int        is_dir;
    size_t     file_off;
    /* Directory entry cache for readdir. */
    ntfs_dir_entry_t *dirents;
    uint32_t   dirent_count;
    uint32_t   dirent_off;
} ntfs_fctx_t;

typedef int (*ntfs_entry_visit)(const uint8_t *entry, uint16_t entry_len,
                                uint16_t flags, void *ctx);

/* Collect context used by ntfs_collect_entry / ntfs_read_directory. */
typedef struct ntfs_collect_ctx {
    ntfs_sb_t *sb;
    ntfs_dir_entry_t *entries;
    uint32_t max;
    uint32_t *count;
} ntfs_collect_ctx_t;

void ntfs_lock(ntfs_sb_t *sb);
void ntfs_unlock(ntfs_sb_t *sb);
int ntfs_write_file(ntfs_vnode_priv_t *fp, uint64_t off,
                           const void *buf, size_t len);
int ntfs_stream_read(ntfs_sb_t *sb, const ntfs_run_t *runs,
                            uint32_t count, uint64_t size, uint64_t off,
                            void *buf, size_t len);
int ntfs_stream_write(ntfs_sb_t *sb, const ntfs_run_t *runs,
                             uint32_t count, uint64_t off, const void *buf,
                             size_t len);
void ntfs_unfixup(uint8_t *buf, uint16_t bps, uint16_t usa_off,
                         uint16_t usa_count);
void ntfs_fixup(uint8_t *buf, uint16_t bps, uint16_t usa_off,
                       uint16_t usa_count);
int ntfs_build_mft_runs(ntfs_sb_t *sb, uint8_t *rec0);
int ntfs_read_record(ntfs_sb_t *sb, uint64_t index, uint8_t *rec);
int ntfs_write_record(ntfs_sb_t *sb, uint64_t index, uint8_t *rec);
uint8_t *ntfs_find_attr(uint8_t *rec, uint64_t rec_size, uint32_t type,
                               int unnamed);
int ntfs_parse_runs(uint8_t *attr, uint32_t cap, ntfs_run_t *out,
                           uint32_t *out_count, uint64_t *data_size);
int ntfs_resolve_data(ntfs_vnode_priv_t *fp);
int ntfs_load_bmap(ntfs_sb_t *sb, uint8_t **out, uint64_t *out_size);
int ntfs_save_bmap(ntfs_sb_t *sb, uint8_t *data, uint64_t size);
uint64_t ntfs_alloc_clusters(ntfs_sb_t *sb, uint64_t count);
void ntfs_free_clusters(ntfs_sb_t *sb, uint64_t lcn, uint64_t count);
int64_t ntfs_find_free_record(ntfs_sb_t *sb, uint64_t start);
void ntfs_free_mft_record(ntfs_sb_t *sb, uint64_t index);
int ntfs_build_file_name_attr(uint8_t *out, size_t cap,
                                     uint64_t parent_ref, const char *name,
                                     int is_dir, uint64_t data_size);
void ntfs_walk_node(const uint8_t *node, uint32_t node_size,
                           ntfs_entry_visit visit, void *ctx);
int ntfs_entry_info(const uint8_t *e, uint16_t elen, char *name,
                           size_t name_cap, int *is_dir, uint64_t *ref,
                           uint64_t *size);
int ntfs_collect_entry(const uint8_t *e, uint16_t elen, uint16_t flags,
                              void *ctx);
int ntfs_read_directory(ntfs_vnode_priv_t *fp, ntfs_dir_entry_t **out,
                               uint32_t *out_count);
int ntfs_build_index_entry(uint8_t *buf, size_t cap, uint64_t ref,
                                  const char *name, int is_dir,
                                  uint64_t data_size, int is_last);
int ntfs_index_insert(ntfs_vnode_priv_t *fp, const char *name,
                             uint64_t ref, int is_dir, uint64_t data_size);
int ntfs_index_remove(ntfs_vnode_priv_t *fp, const char *name);
int ntfs_ensure_data_runs(ntfs_vnode_priv_t *fp, uint64_t need_end);
int ntfs_convert_resident_to_nonresident(ntfs_vnode_priv_t *fp);
vnode_t *ntfs_make_vnode(ntfs_sb_t *sb, uint64_t mft_index,
                                uint32_t seq, int is_dir, vnode_t *parent);
void ntfs_release_vn(vnode_t *vn);
int ntfs_lookup(vnode_t *dir, const char *name, vnode_t **out);
int ntfs_stat(vnode_t *vn, kstat_t *st);
int ntfs_statfs(vnode_t *vn, kstatfs_t *st);
void ntfs_update_file_name_size(ntfs_sb_t *sb, uint64_t mft_index,
                                       uint64_t size, int is_dir);
int ntfs_update_file_name(ntfs_sb_t *sb, uint64_t mft_index,
                                 uint64_t parent_ref, const char *name,
                                 int is_dir, uint64_t data_size);
int ntfs_create_entry(ntfs_vnode_priv_t *dirfp, const char *name,
                             int mode, int is_dir, vnode_t **out);
int ntfs_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out);
int ntfs_vn_mkdir(vnode_t *dir, const char *name, int mode);
int ntfs_vn_unlink(vnode_t *dir, const char *name);
int ntfs_vn_rmdir(vnode_t *dir, const char *name);
int ntfs_vn_rename(vnode_t *old_dir, const char *old_name,
                          vnode_t *new_dir, const char *new_name,
                          unsigned int flags);
int ntfs_vn_readpage(vnode_t *vn, uint64_t index, void *data, size_t len);
int ntfs_vn_writepage(vnode_t *vn, uint64_t index, const void *data, size_t len);
int ntfs_vn_truncate(vnode_t *vn, size_t size);
vfile_t *ntfs_open_vnode(vnode_t *vn, int flags);
int ntfs_fread(vfile_t *vf, char *buf, size_t count);
int ntfs_fwrite(vfile_t *vf, const char *buf, size_t count);
long ntfs_flseek(vfile_t *vf, long offset, int whence);
int ntfs_freaddir(vfile_t *vf, void *dirp, size_t count);
int ntfs_fclose(vfile_t *vf);
vnode_t *ntfs_mount(struct bcache *bc);
void ntfs_unmount(vnode_t *root);

extern vnode_ops_t g_ntfs_vnode_ops;
extern vfile_ops_t g_ntfs_fops;
#endif /* _NTFS_INTERNAL_H */

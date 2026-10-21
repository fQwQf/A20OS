#ifndef _FS_UXFS_OPS_STATE_H
#define _FS_UXFS_OPS_STATE_H

#include "core/types.h"

/*
 * UXFS per-operation decision engine.
 *
 * The request construction (which opcode, ino, arg0/arg1, name and payload a
 * LOOKUP/CREATE/MKDIR/UNLINK/LINK/RMDIR/RENAME/TRUNCATE/GETATTR/STATFS/READ/
 * WRITE sends), the response decoding (how out0/out1/out2 become a vnode, a
 * kstat, a byte count), and the ordered local state transitions (canonical
 * identity forget-before-create, iget with the decoded type/mode/size, page
 * cache truncation plus size set on truncate, short-read zero fill, writepage
 * tail clamping, offset/size growth on write) are owned by
 * kernel/fs/uxfs/uxfs_ops_state.rs.
 *
 * The adapter supplies only the mechanical half through a20_uxfs_ops: the
 * request-header init (which allocates the sb's next req_id), the RPC transport
 * call (already Rust-owned admission/deadline underneath), vnode accessors,
 * canonical lookup-or-create / identity-forget / page-cache-truncate, and the
 * lifetime negotiation gate for LINK.
 *
 * Keep the field order in sync with the Rust mirror (layout asserted in
 * uxfs.c).
 */

struct ufs_req_hdr;
struct ufs_resp_hdr;

struct a20_uxfs_ops {
    /* --- transport (mechanical) --- */
    void (*req_init)(void *ctx, struct ufs_req_hdr *req, uint32_t opcode,
                     uint64_t ino, uint64_t arg0, uint64_t arg1,
                     const char *name, uint32_t payload_len);
    int  (*rpc)(void *ctx, const struct ufs_req_hdr *req, const char *name,
                const void *payload, struct ufs_resp_hdr *resp,
                void *resp_payload, uint32_t resp_cap,
                uint32_t *resp_payload_len);

    /* --- vnode accessors (mechanical) --- */
    void    *(*vpriv_sb)(void *vnode);
    uint64_t (*vn_ino)(void *vnode);
    uint64_t (*vn_size)(void *vnode);
    void     (*vn_set_size)(void *vnode, uint64_t sz);
    void    *(*vn_parent)(void *vnode);

    /* --- vnode lifecycle (mechanical allocation / identity index) --- */
    void    *(*iget)(void *ctx, uint64_t ino, int vfs_type, uint32_t mode,
                     uint64_t size, void *parent);
    void     (*identity_forget)(void *ctx, uint64_t ino);
    void     (*vnode_get)(void *vnode);
    void     (*page_cache_truncate)(void *vnode, uint64_t sz);

    /* --- lifetime gate (LINK only) --- */
    int      (*negotiate_lifetime)(void *ctx);
    int      (*lifetime_is_v2)(void *ctx);
};

/* Namespace / attribute / data-plane entry points.  Each returns 0 or a
 * negative errno; the vnode-returning forms hand the caller one reference. */
int a20_uxfs_op_lookup(const struct a20_uxfs_ops *ops, void *dir,
                       const char *name, void **out);
int a20_uxfs_op_stat(const struct a20_uxfs_ops *ops, void *vn, void *kstat);
int a20_uxfs_op_statfs(const struct a20_uxfs_ops *ops, void *vn, void *kstatfs);
int a20_uxfs_op_create(const struct a20_uxfs_ops *ops, void *dir,
                       const char *name, uint32_t mode, void **out);
int a20_uxfs_op_mkdir(const struct a20_uxfs_ops *ops, void *dir,
                      const char *name, uint32_t mode);
int a20_uxfs_op_unlink(const struct a20_uxfs_ops *ops, void *dir,
                       const char *name);
int a20_uxfs_op_link(const struct a20_uxfs_ops *ops, void *dir,
                     const char *name, void *target);
int a20_uxfs_op_rmdir(const struct a20_uxfs_ops *ops, void *dir,
                      const char *name);
int a20_uxfs_op_rename(const struct a20_uxfs_ops *ops, void *old_dir,
                       const char *old_name, void *new_dir,
                       const char *new_name);
int a20_uxfs_op_truncate(const struct a20_uxfs_ops *ops, void *vn,
                         uint64_t size);
int a20_uxfs_op_sync_vnode(const struct a20_uxfs_ops *ops, void *vn);
int a20_uxfs_op_readpage(const struct a20_uxfs_ops *ops, void *vn,
                         uint64_t index, void *data, uint64_t len);
int a20_uxfs_op_writepage(const struct a20_uxfs_ops *ops, void *vn,
                          uint64_t index, const void *data, uint64_t len);

/*
 * Open-file (data-plane) decision engine.
 *
 * The per-open-file offset/cookie arithmetic, the short-read zero fill, the
 * write size growth and the wire-directory -> vfs_dirent64_t translation are
 * owned by the same Rust module.  The adapter supplies only the fctx/vfile
 * field accessors plus the request/RPC mechanics through a20_uxfs_fops.
 */
struct uxfs_fctx;

struct a20_uxfs_fops {
    void (*req_init)(void *ctx, struct ufs_req_hdr *req, uint32_t opcode,
                     uint64_t ino, uint64_t arg0, uint64_t arg1,
                     const char *name, uint32_t payload_len);
    int  (*rpc)(void *ctx, const struct ufs_req_hdr *req, const char *name,
                const void *payload, struct ufs_resp_hdr *resp,
                void *resp_payload, uint32_t resp_cap,
                uint32_t *resp_payload_len);
    void    *(*fc_sb)(void *fctx);
    uint64_t (*fc_ino)(const void *fctx);
    uint64_t (*fc_off)(const void *fctx);
    void     (*fc_set_off)(void *fctx, uint64_t off);
    int      (*fc_is_dir)(const void *fctx);
    void    *(*vf_vnode)(void *vfile);
    void     (*vf_set_offset)(void *vfile, uint64_t off);
    uint64_t (*vn_size)(void *vnode);
    void     (*vn_set_size)(void *vnode, uint64_t sz);
};

int a20_uxfs_file_read(const struct a20_uxfs_fops *fops, void *fctx,
                       void *vfile, char *buf, size_t count);
int a20_uxfs_file_write(const struct a20_uxfs_fops *fops, void *fctx,
                        void *vfile, const char *buf, size_t count);
int64_t a20_uxfs_file_lseek(const struct a20_uxfs_fops *fops, void *fctx,
                            void *vfile, int64_t offset, int whence);
int a20_uxfs_file_readdir(const struct a20_uxfs_fops *fops, void *fctx,
                          void *vfile, void *dirp, size_t count,
                          uint8_t *scratch, uint32_t scratch_cap);

/*
 * Mount/serve/unmount namespace + block-device ownership engine.
 *
 * The slot-selection/reservation transaction, the server-generation and
 * lifetime reset, root-vnode creation, mount-object construction and tree link
 * with their exact unwinds, and the shutdown-outcome decision tree live in
 * kernel/fs/uxfs/uxfs_ops_state.rs.  The adapter supplies the static sb table
 * base/stride/count, the mount-table lock, single-field accessors through the
 * asserted sb prefix layout, and the mechanical VFS/identity/proc/channel
 * calls.
 */
struct a20_uxfs_ns {
    void    *(*sb_table)(void);
    uint64_t (*sb_stride)(void);
    int      (*sb_count)(void);
    uint64_t (*lock_mounts)(void);
    void     (*unlock_mounts)(uint64_t flags);
    void    *(*block_device)(int block_index);
    int      (*device_has_native_mount)(void *dev);
    int      (*path_is_dir)(const char *path);
    void    *(*iget)(void *sb, uint64_t ino, int vfs_type, uint32_t mode,
                     uint64_t size, void *parent);
    void    *(*mount_alloc)(void);
    void     (*mount_init)(void *mnt, const char *path, uint32_t flags,
                           void *root, void *sb);
    void     (*mount_link_tree)(void *mnt);
    void     (*dcache_invalidate_all)(void *unused);
    void     (*identity_register_peek)(void *unused);
    void     (*identity_clear_sb)(void *sb);
    void     (*vnode_get)(void *vnode);
    void     (*vnode_put)(void *vnode);
    void    *(*vnode_fs_data)(void *vnode);
    void     (*lifetime_reset)(void *lifetime);
    void    *(*proc_get)(void *task);
    void     (*proc_put)(void *task);
    void     (*slot_init_req_lock)(void *sb);
    void     (*detach_server)(void *sb);
    int      (*peer_closed)(void *ep);
    uint64_t (*now_ns)(void);
    int      (*negotiate_lifetime_until)(void *sb, uint64_t deadline_ns);
    int      (*shutdown_rpc)(void *sb, uint64_t deadline_ns, int *retry_safe);
    void     (*mount_log)(const char *path, int block_index);
    /* Whether the serving task that owns this mount is still alive.  A killed
     * service never touches the channel again, so its peer_closed flag stays
     * clear and unmount would otherwise wait out the whole deadline. */
    int      (*server_alive)(void *sb);
};

int  a20_uxfs_ns_device_owned(const struct a20_uxfs_ns *ns, void *dev);
void *a20_uxfs_ns_owned_block_dev(const struct a20_uxfs_ns *ns, void *task,
                                  int block_index);
int  a20_uxfs_ns_serve_mount(const struct a20_uxfs_ns *ns, const char *path,
                             void *ep, void *server, int block_index,
                             uint32_t serve_flags);
int  a20_uxfs_ns_unmount(const struct a20_uxfs_ns *ns, void *root);

/*
 * Open-file lifecycle engine (UXFS open/close).
 *
 * a20_uxfs_file_open reserves the local fctx/vfile before the server-side
 * lifetime exists, then binds the OPEN token/confirm mode and the O_APPEND
 * start offset.  a20_uxfs_file_close owns the close-token state machine: the
 * writeback gate, the CLOSE/CONFIRM ACK accounting, when a retry is safe, and
 * when the fctx may be retired.  The adapter supplies allocation, the vfile/
 * fctx field accessors and the lifetime close/confirm RPCs.
 */
struct a20_uxfs_lifecycle {
    void    *(*fc_alloc)(void);
    void    *(*vfile_alloc)(void);
    void     (*vfile_free)(void *vfile);
    void     (*kfree)(void *p);
    void    *(*vn_vpriv)(void *vnode);
    void    *(*vpriv_sb)(void *vpriv);
    int      (*vpriv_is_dir)(void *vpriv);
    uint64_t (*vn_ino)(const void *vnode);
    uint64_t (*vn_size)(const void *vnode);
    int      (*lifetime_open)(void *sb, uint64_t ino, uint64_t flags,
                              uint64_t *token);
    int      (*lifetime_confirm_mode)(void *sb);
    void     (*fc_init)(void *fctx, void *sb, uint64_t ino, uint64_t token,
                        int confirm_mode, int is_dir, uint64_t off);
    void     (*vf_init)(void *vfile, void *vnode, int flags, int capable,
                        uint64_t off, void *priv);
    void    *(*fc_sb)(void *fctx);
    uint64_t (*fc_ino)(const void *fctx);
    uint64_t (*fc_close_token)(const void *fctx);
    int      (*fc_close_confirm_mode)(const void *fctx);
    int      (*fc_close_accepted)(const void *fctx);
    void     (*fc_set_close_accepted)(void *fctx, int v);
    void    *(*vf_vnode)(void *vfile);
    void    *(*vf_priv)(void *vfile);
    void     (*vf_set_close_attempt)(void *vfile, int sent, int acked,
                                     int retry_safe);
    void     (*fc_release)(void *vfile, void *fctx);
    int      (*pc_writeback)(void *vnode);
    int      (*lifetime_close)(void *sb, uint64_t ino, uint64_t token,
                               int *acked, int *request_sent);
    int      (*lifetime_close_confirm)(void *sb, uint64_t ino, uint64_t token,
                                       int *acked, int *request_sent);
    void     (*trace)(void *vfile, void *fctx);
};

void *a20_uxfs_file_open(const struct a20_uxfs_lifecycle *lc, void *vn,
                         int flags);
int   a20_uxfs_file_close(const struct a20_uxfs_lifecycle *lc, void *vf);

#endif /* _FS_UXFS_OPS_STATE_H */

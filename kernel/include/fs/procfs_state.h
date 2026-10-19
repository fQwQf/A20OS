#ifndef _FS_PROCFS_STATE_H
#define _FS_PROCFS_STATE_H

#include "core/types.h"

/*
 * procfs read/seek offset state and directory-cursor state.
 *
 * The transitions are owned by kernel/fs/procfs/procfs_state.rs; procfs.c keeps
 * the vnode/priv plumbing, the getdents64 buffer, the fdtable/tasklist lookups
 * and the memcpy/snprintf bridges, and calls in with the locks its own contract
 * requires already held.  Keep the struct order in sync with the Rust mirror
 * (layout asserted in procfs_state.rs and in the host harness).
 */

/* Directory variants: the cursor advance rule and per-entry d_ino/d_off/d_type
 * policy differ between them. */
#define PROCFS_DIR_FD     0u
#define PROCFS_DIR_FDINFO 1u
#define PROCFS_DIR_TASK   2u
#define PROCFS_DIR_STATIC 3u

typedef struct procfs_dir_entry {
    uint64_t key;        /* sparse cursor key (fd number, or raw pid) */
    int64_t  shown;      /* id rendered into d_name/d_ino when name is NULL */
    const char *name;    /* literal NUL-terminated name, or NULL */
    uint8_t  type_hint;  /* DT_* the lookup already knows; 0 lets the engine pick */
} procfs_dir_entry_t;

/* Fill `out` with the entry at `cursor`.  Return 1 when present, 0 at end of
 * directory, or a negative errno.  `ctx` is the adapter's lookup state. */
typedef int (*procfs_dir_next_fn)(void *ctx, uint64_t cursor,
                                  procfs_dir_entry_t *out);

/* Emit one getdents64 batch: returns bytes emitted (or negative errno) and
 * writes the resumed cursor through `out_cursor`. */
int64_t a20_procfs_readdir_plan(uint32_t mode, uint64_t start, size_t count,
                                void *ctx, procfs_dir_next_fn next,
                                void *out, uint64_t *out_cursor);

/* lseek arithmetic against a rendered procfs file; returns the new offset
 * (clamped at 0) or -EINVAL. */
int64_t a20_procfs_seek_plan(uint64_t content_len, uint64_t cur, int64_t off,
                             int whence);

/* Buffered-content read plan: returns the byte count and writes the resumed
 * offset through `out_offset`. */
int64_t a20_procfs_read_plan(uint64_t content_len, uint64_t offset, size_t count,
                             uint64_t *out_offset);

#endif /* _FS_PROCFS_STATE_H */

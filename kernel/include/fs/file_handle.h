#ifndef _FS_FILE_HANDLE_H
#define _FS_FILE_HANDLE_H

/*
 * Opaque file handle registry (name_to_handle_at / open_by_handle_at).
 *
 * A20OS does not have per-filesystem export operations, so handles are
 * kernel-side: a handle value is an index into a small table that holds a
 * referenced vnode plus a generation cookie.  name_to_handle_at() mints a
 * handle; open_by_handle_at() revalidates the generation and installs a new
 * fd.  Handles never dereference freed vnodes because the table holds its own
 * reference.
 */

#include "core/types.h"

struct vnode;

/* Mint a handle for @vn.  Returns the opaque handle value or 0 on failure. */
uint64_t file_handle_mint(struct vnode *vn);

/* Look up a handle; returns a referenced vnode (caller must vnode_put) or
 * NULL if the handle is stale. */
struct vnode *file_handle_get(uint64_t handle);

/* Drop a handle (e.g. after the filesystem is torn down). */
void file_handle_drop(uint64_t handle);

#endif /* _FS_FILE_HANDLE_H */

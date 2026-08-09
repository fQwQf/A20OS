#ifndef _FS_FSCONTEXT_H
#define _FS_FSCONTEXT_H

/*
 * Filesystem context / new-mount-API helpers.
 *
 * The fsopen(2) family (fsopen, fsconfig, fsmount, fspick, open_tree,
 * move_mount, mount_setattr) is implemented on top of the existing mount
 * table.  An fs context is an anonymous file that accumulates a source
 * (device), filesystem type and mount options; fsmount() calls the existing
 * vfs_mount() with those parameters and returns a mount fd.  open_tree()
 * returns an O_PATH-style fd referencing a vnode tree.
 */

#include "core/types.h"

struct vnode;

/* Create a new fs context file for @fstype.  Returns a global VFS fd or a
 * negative errno. */
int fscontext_create(const char *fstype);

/* Configure a context fd: @key is "source", "type" or a mount option string.
 * @value is the value (NULL for the type key).  Returns 0 or errno. */
int fscontext_config(int gfd, const char *key, const char *value);

/* Realize the context as a mount at @target.  Returns a mount fd (global VFS
 * fd opened O_PATH on the mount root) or a negative errno. */
int fscontext_fsmount(int gfd, int flags, int mnt_flags, const char *target);

/* open_tree(2): return an O_PATH-style global fd for @vn (referenced). */
int fscontext_open_tree_fd(struct vnode *vn);

#endif /* _FS_FSCONTEXT_H */

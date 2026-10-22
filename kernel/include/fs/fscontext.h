#ifndef _FS_FSCONTEXT_H
#define _FS_FSCONTEXT_H

/*
 * Filesystem context / new-mount-API helpers.
 *
 * The fsopen(2) family (fsopen, fsconfig, fsmount, fspick, open_tree,
 * move_mount, mount_setattr) is implemented on top of the existing mount
 * table.  An fs context is an anonymous file that accumulates a source
 * (device), filesystem type and mount options; fsmount() with a target calls
 * the existing vfs_mount() with those parameters, and without one it builds
 * a detached mount and returns a mount fd for move_mount(2) to attach.
 * open_tree() returns an O_PATH-style fd referencing a vnode tree.
 */

#include "core/types.h"

struct vnode;
struct mount;

/* Create a new fs context file for @fstype.  Returns a global VFS fd or a
 * negative errno. */
int fscontext_create(const char *fstype);

/* Configure a context fd: @key is "source", "type" or a mount option string.
 * @value is the value (NULL for the type key).  Returns 0 or errno. */
int fscontext_config(int gfd, const char *key, const char *value);

/* Realize the context as a mount.  With a non-NULL @target this mounts there
 * and returns an O_PATH fd on the mount root; with @target == NULL it returns
 * a detached mount fd, which is also what the fsmount(2) syscall always asks
 * for.  Returns a global VFS fd or a negative errno. */
int fscontext_fsmount(int gfd, int flags, int mnt_flags, const char *target);

/* If @gfd is a detached mount fd, return the mount it refers to; NULL
 * otherwise.  The fd keeps owning the mount, so the result is only valid
 * while the caller still holds @gfd. */
struct mount *fscontext_mountfd_get(int gfd);

/* open_tree(2): return an O_PATH-style global fd for @vn (referenced). */
int fscontext_open_tree_fd(struct vnode *vn);

#endif /* _FS_FSCONTEXT_H */

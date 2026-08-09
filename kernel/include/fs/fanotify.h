#ifndef _FS_FANOTIFY_H
#define _FS_FANOTIFY_H

/*
 * A20OS fanotify subsystem (ABI-independent).
 *
 * fanotify is the filesystem-monitoring interface used by antivirus/indexers
 * to watch whole mount trees and receive file IDs.  A20OS implements the
 * subset needed by musl-based tooling: a notif-only instance (FAN_CLASS_NOTIF)
 * with FID reporting, ADD/REMOVE marks on paths, and the standard
 * fanotify_event_metadata + FID read format.
 *
 * The notification backend is shared with inotify (kernel/fs/inotify.c); this
 * header exposes the fanotify-specific instance creation and marking entry
 * points.  The ABI layer (kernel/abi/linux/sys_fanotify.c) translates the
 * Linux wire flags and copies user pathnames before calling these functions.
 */

#include "core/types.h"

/* fanotify_init(2) flags. */
#define FAN_CLOEXEC            0x00000001
#define FAN_NONBLOCK           0x00000002
#define FAN_CLASS_NOTIF        0x00000000
#define FAN_CLASS_CONTENT      0x00000004
#define FAN_CLASS_PRE_CONTENT  0x00000008
#define FAN_ALL_CLASS_BITS     (FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT)
#define FAN_REPORT_FID         0x00000200
#define FAN_REPORT_DIR_FID     0x00000400
#define FAN_REPORT_NAME        0x00000800
#define FAN_REPORT_DFID_NAME   (FAN_REPORT_DIR_FID | FAN_REPORT_NAME)

/* fanotify_mark(2) flags. */
#define FAN_MARK_ADD           0x00000001
#define FAN_MARK_REMOVE        0x00000002
#define FAN_MARK_DONT_FOLLOW   0x00000004
#define FAN_MARK_ONLYDIR       0x00000008
#define FAN_MARK_IGNORED_MASK  0x00000020

/* Mark masks (orthogonal to the inotify event bits). */
#define FAN_EVENT_ON_CHILD     0x08000000
#define FAN_ONDIR              0x40000000

#define FAN_NOFD               (-1)
#define FANOTIFY_METADATA_VERSION 3
#define FAN_EVENT_INFO_TYPE_FID    1
#define FAN_EVENT_INFO_TYPE_DFID   2
#define FAN_EVENT_INFO_TYPE_DFID_NAME 3

/* Create a fanotify instance file.  Returns a global VFS fd or negative errno. */
int fanotify_create_file(int flags, int event_f_flags);

/* Add/remove a mark for @path (kernel NUL-terminated).  @dfd is the dirfd for
 * relative paths (-1 for absolute).  Returns 0 or a negative errno. */
int fanotify_mark(int gfd, unsigned flags, uint64_t mask, int dfd,
                  const char *path);

#endif /* _FS_FANOTIFY_H */

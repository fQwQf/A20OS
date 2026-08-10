#ifndef _DEVFS_INTERNAL_H
#define _DEVFS_INTERNAL_H

/*
 * Shared internal decls for the devfs backend split:
 *   kernel/fs/devfs/devfs.c      node table, vnode ops, dirs, class, mount
 *   kernel/fs/devfs/devfs_mem.c  /dev/null, zero, random, full, kmsg
 *   kernel/fs/devfs/devfs_rtc.c  /dev/rtc, /dev/rtc0
 *   kernel/fs/devfs/tty.c        console/stdio backing
 *
 * Only devfs.c exposes the public API (fs/devfs.h).  The *_ops tables from the
 * other files are referenced here so devfs.c can wire them into its node
 * dispatch and is_*_vfile helpers without owning their implementation.
 */

#include "fs/vfs.h"

/* Node kinds (shared with devfs.c). */
enum {
    DEVFS_ROOT,
    DEVFS_MISC,
    DEVFS_NULL,
    DEVFS_ZERO,
    DEVFS_RANDOM,
    DEVFS_FULL,
    DEVFS_KMSG,
    DEVFS_TTY,
    DEVFS_RTC,
    DEVFS_LOOP,
    DEVFS_LOOP_CTRL,
    DEVFS_PTMX,
    DEVFS_PTS_DIR,
    DEVFS_PTS,
    DEVFS_SHM_DIR,
    DEVFS_FB,
    DEVFS_INPUT,
    DEVFS_CLASS,
    DEVFS_DRI_DIR,
    DEVFS_DRM,
    DEVFS_SND_DIR,
    DEVFS_ALSA_CTL,
    DEVFS_ALSA_PCM,
};

/* shared helpers defined in devfs_mem.c */
int devfs_null_read(vfile_t *vf, char *buf, size_t count);
int devfs_null_write(vfile_t *vf, const char *buf, size_t count);

/* ops tables defined in devfs_mem.c */
extern vfile_ops_t g_devfs_null_ops;
extern vfile_ops_t g_devfs_zero_ops;
extern vfile_ops_t g_devfs_random_ops;
extern vfile_ops_t g_devfs_full_ops;
extern vfile_ops_t g_devfs_kmsg_ops;

/* ops table defined in devfs_rtc.c */
extern vfile_ops_t g_devfs_rtc_ops;
int devfs_rtc_ioctl(unsigned long req, void *arg);

/* shared no-op lseek (defined in devfs.c) */
long devfs_noop_lseek(vfile_t *vf, long offset, int whence);

#endif /* _DEVFS_INTERNAL_H */

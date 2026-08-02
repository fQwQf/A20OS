#ifndef _FS_TTY_H
#define _FS_TTY_H

#include "core/types.h"

struct vfile;

/*
 * Console TTY line discipline and termios handling, split out of fs/devfs/devfs.c.
 *
 * Owns the single virtual console state (termios/winsize/VT mode) plus the
 * per-pid line-buffer hand-off used to serialize interleaved console writes.
 * devfs.c dispatches the DEVFS_TTY file operations to these entry points.
 */

void tty_console_init(void);
int  tty_console_read(struct vfile *vf, char *buf, size_t count);
int  tty_console_write(struct vfile *vf, const char *buf, size_t count);
int  tty_console_ioctl(unsigned long req, void *arg);

#endif /* _FS_TTY_H */

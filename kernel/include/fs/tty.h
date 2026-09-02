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

/*
 * xterm mouse protocol support.
 *
 * When a user-space program enables mouse tracking (by writing
 * "\033[?1000h", "\033[?1003h", or "\033[?1006h" to the console),
 * the TTY layer converts input events from the kernel input subsystem
 * into SGR-encoded mouse escape sequences ("\033[<button;col;rowM" for
 * press, "\033[<button;col;rowm" for release) and injects them into the
 * console read path so they appear on stdin alongside keyboard input.
 *
 * tty_push_mouse_event() is called from the input multiplexer when a
 * mouse-relevant event (EV_REL or EV_KEY with a BTN_MOUSE_* code) is
 * observed and mouse tracking is active.
 */
int  tty_mouse_mode_active(void);
void tty_push_mouse_event(uint16_t type, uint16_t code, int32_t value);

#endif /* _FS_TTY_H */

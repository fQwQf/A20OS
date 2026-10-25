#ifndef _FS_TTY_H
#define _FS_TTY_H

#include "core/types.h"

#define KTTY_NCCS 19

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[KTTY_NCCS];
} ktty_termios_t;

typedef struct {
    uint16_t row, col, xpixel, ypixel;
} ktty_winsize_t;

struct vfile;

/* Kernel-buffer operations: shared by typed Native controls and device code.
 * No userspace pointer or architecture-specific ioctl layout crosses here. */
int tty_vfile_get_termios(struct vfile *vf, ktty_termios_t *out);
int tty_vfile_set_termios(struct vfile *vf, const ktty_termios_t *in, int action);
int tty_vfile_get_winsize(struct vfile *vf, ktty_winsize_t *out);
int tty_vfile_set_winsize(struct vfile *vf, const ktty_winsize_t *in);
int tty_vfile_flush(struct vfile *vf, int queue);
int tty_console_get_termios(ktty_termios_t *out);
int tty_console_set_termios(const ktty_termios_t *in, int action);
int tty_console_get_winsize(ktty_winsize_t *out);
int tty_console_set_winsize(const ktty_winsize_t *in);
int tty_console_flush(int queue);
int pty_get_termios(int index, ktty_termios_t *out);
int pty_set_termios(int index, const ktty_termios_t *in, int action, int master);
int pty_get_winsize(int index, ktty_winsize_t *out);
int pty_set_winsize(int index, const ktty_winsize_t *in);
int pty_flush(int index, int queue, int master);

long arch_tty_termios_to_user(void *dst, const ktty_termios_t *src);
long arch_tty_termios_from_user(ktty_termios_t *dst, const void *src);

struct vfile;

/*
 * Console TTY line discipline and termios handling, split out of fs/devfs/devfs.c.
 *
 * Owns the single virtual console state (termios/winsize/VT mode) and serializes
 * userspace writes with one mutex.
 * devfs.c dispatches the DEVFS_TTY file operations to these entry points.
 */

void tty_console_init(void);
int  tty_console_read(struct vfile *vf, char *buf, size_t count);
int  tty_console_write(struct vfile *vf, const char *buf, size_t count);
int  tty_console_ioctl(unsigned long req, void *arg);

/*
 * ISIG, applied on the receive path.
 *
 * The serial driver calls tty_console_isig_signal() for every byte it takes off
 * the wire, before queueing it, and raises whatever it returns once it has
 * dropped its RX lock.  A line discipline decides what a byte means as it
 * arrives -- that is what lets Ctrl-C stop a foreground child that never reads
 * the console, and it is why this cannot live in tty_console_read(): a reader
 * that is not reading is exactly the case ISIG exists for.
 *
 * tty_console_isig_signal() takes no locks (the driver holds its RX lock when
 * it asks); tty_console_isig_raise() does, so it must be called unlocked.
 */
int  tty_console_isig_signal(char c);
void tty_console_isig_raise(int sig);

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

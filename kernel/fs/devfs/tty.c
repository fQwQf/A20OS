#include "fs/tty_control.h"
#include "fs/tty.h"
#include "drivers/char/uart.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/sync.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"
#include "core/ioctl.h"
#include "core/bootargs.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/char/virtio_console.h"

extern void uart_putc(char c);
extern int  uart_getc(void);
/* The console's foreground process group; the discipline needs it to know who
 * an ISIG signal is for.  Same accessor the TIOCSPGRP/TIOCGPGRP path uses
 * (kernel/abi/linux/sys_fs.c). */
extern int  uart_get_foreground_pgid(void);
/* Owned by kernel/core/klog.c and kernel/core/printf.c: the one lock every
 * console byte passes under.  Declared extern the same way printf.c:9 does. */
extern spinlock_t klog_console_lock;

/*
 * Console TTY line discipline, termios state and the stdin/stdout console
 * file operations, split out of fs/devfs.c.  Owns the single virtual console
 * (g_dev_tty) and serializes userspace console writes; devfs.c only dispatches
 * to these via fs/tty.h.
 */

/* Bound interrupt-off time while allowing whole short records in one burst. */
#define TTY_EMIT_CHUNK 256
static mutex_t g_tty_write_lock = MUTEX_INIT;
/* The reference is owned by g_tty_write_lock. Each operation additionally
 * leases the current binding so unload cannot free its ops or DMA buffers. */
static class_device_t *g_tty_bulk_console;
static int g_tty_use_bulk_console;
static int g_tty_bulk_ready;

__attribute__((weak)) unsigned long arch_linux_ioctl_request(unsigned long req)
{
    return req;
}

__attribute__((weak)) long arch_tty_termios_to_user(void *dst,
                                                   const ktty_termios_t *src)
{
    return copy_to_user(dst, src, sizeof(*src));
}

__attribute__((weak)) long arch_tty_termios_from_user(ktty_termios_t *dst,
                                                     const void *src)
{
    return copy_from_user(dst, src, sizeof(*dst));
}

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} kwinsize_t;

typedef struct {
    ktty_termios_t termios;
    kwinsize_t winsize;
    int kbmode;      /* K_XLATE / K_RAW — keyboard mode of the console */
    int kdmode;      /* KD_TEXT / KD_GRAPHICS — display mode of the console */
    int vt_process;  /* VT mode: 0 = VT_AUTO, 1 = VT_PROCESS */
    /* xterm mouse protocol state */
    uint8_t  mouse_enabled;   /* bit 0: 1000h (X10), bit 1: 1002h (btn), bit 2: 1003h (any) */
    uint8_t  mouse_sgr;       /* 1 = SGR mode (1006h), 0 = X10 default */
    int32_t  mouse_x;         /* accumulated absolute X (pixels) */
    int32_t  mouse_y;         /* accumulated absolute Y (pixels) */
    uint8_t  mouse_btn;       /* current button state (bitmask: bit0=L, bit1=R, bit2=M) */
    /* ring buffer for mouse escape sequence bytes pending on stdin */
    uint8_t  mouse_buf[128];
    int      mouse_head;
    int      mouse_tail;
} tty_state_t;

static tty_state_t g_dev_tty;

/* ---- mouse escape-sequence ring buffer helpers ---- */

static int mouse_buf_empty(void)
{
    return g_dev_tty.mouse_head == g_dev_tty.mouse_tail;
}

static void mouse_buf_push_byte(uint8_t b)
{
    int next = (g_dev_tty.mouse_head + 1) % (int)sizeof(g_dev_tty.mouse_buf);
    if (next == g_dev_tty.mouse_tail)
        return; /* full — drop */
    g_dev_tty.mouse_buf[g_dev_tty.mouse_head] = b;
    g_dev_tty.mouse_head = next;
}

static int mouse_buf_pop_byte(void)
{
    if (mouse_buf_empty())
        return -1;
    uint8_t b = g_dev_tty.mouse_buf[g_dev_tty.mouse_tail];
    g_dev_tty.mouse_tail = (g_dev_tty.mouse_tail + 1) % (int)sizeof(g_dev_tty.mouse_buf);
    return b;
}

/*
 * Append a decimal integer to the mouse byte buffer as ASCII digits.
 * Used to encode row/column values in SGR mouse sequences.
 */
static void mouse_buf_push_int(int val)
{
    char tmp[12];
    int len = 0;
    if (val < 0) { mouse_buf_push_byte('-'); val = -val; }
    if (val == 0) { mouse_buf_push_byte('0'); return; }
    while (val > 0 && len < (int)sizeof(tmp)) {
        tmp[len++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = len - 1; i >= 0; i--)
        mouse_buf_push_byte((uint8_t)tmp[i]);
}

int tty_mouse_mode_active(void)
{
    return g_dev_tty.mouse_enabled != 0;
}

void tty_push_mouse_event(uint16_t type, uint16_t code, int32_t value)
{
    if (!g_dev_tty.mouse_enabled)
        return;

    if (type == 0x02 /* EV_REL */) {
        if (code == 0 /* REL_X */)
            g_dev_tty.mouse_x += value;
        else if (code == 1 /* REL_Y */)
            g_dev_tty.mouse_y += value;
        else if (code == 8 /* REL_WHEEL */) {
            /* wheel: encode as button 64/65 up/down */
            int btn = value > 0 ? 64 : 65;
            int col = g_dev_tty.mouse_x;
            int row = g_dev_tty.mouse_y;
            if (col < 0) col = 0;
            if (row < 0) row = 0;
            if (g_dev_tty.mouse_sgr) {
                mouse_buf_push_byte('\033');
                mouse_buf_push_byte('[');
                mouse_buf_push_byte('<');
                mouse_buf_push_int(btn);
                mouse_buf_push_byte(';');
                mouse_buf_push_int(col + 1);
                mouse_buf_push_byte(';');
                mouse_buf_push_int(row + 1);
                mouse_buf_push_byte('M');
            }
        }
        return;
    }

    if (type == 0x01 /* EV_KEY */) {
        uint32_t btn_code = code;
        int pressed = value != 0;
        int btn;
        if (btn_code == 0x110 /* BTN_LEFT */)   btn = 0;
        else if (btn_code == 0x111 /* BTN_RIGHT */)  btn = 1;
        else if (btn_code == 0x112 /* BTN_MIDDLE */) btn = 2;
        else return;

        if (pressed)
            g_dev_tty.mouse_btn |= (1u << btn);
        else
            g_dev_tty.mouse_btn &= ~(1u << btn);

        int col = g_dev_tty.mouse_x;
        int row = g_dev_tty.mouse_y;
        if (col < 0) col = 0;
        if (row < 0) row = 0;

        if (g_dev_tty.mouse_sgr) {
            /* SGR: \033[<button;col;rowM (press) or \033[<button;col;rowm (release) */
            mouse_buf_push_byte('\033');
            mouse_buf_push_byte('[');
            mouse_buf_push_byte('<');
            mouse_buf_push_int(btn + g_dev_tty.mouse_btn);
            mouse_buf_push_byte(';');
            mouse_buf_push_int(col + 1);
            mouse_buf_push_byte(';');
            mouse_buf_push_int(row + 1);
            mouse_buf_push_byte(pressed ? 'M' : 'm');
        } else {
            /* X10 legacy: \033[M<btn+32><col+32><row+32> */
            mouse_buf_push_byte('\033');
            mouse_buf_push_byte('[');
            mouse_buf_push_byte('M');
            mouse_buf_push_byte((uint8_t)(btn + 32));
            mouse_buf_push_byte((uint8_t)(col + 1 + 32));
            mouse_buf_push_byte((uint8_t)(row + 1 + 32));
        }
    }
}

/* termios lflag bits (Linux values) used by the console discipline. */
#define DEVFS_ISIG   0x00000001U
#define DEVFS_ECHO   0x00000008U

/* c_cc[] indices, Linux numbering; the same layout fill_default_termios()
 * populates below. */
#define DEVFS_VINTR  0
#define DEVFS_VQUIT  1
#define DEVFS_VSUSP  10

/* The signal an ISIG control character stands for, or 0 if this byte is data.
 * Only the three characters that have a standard signal are mapped; anything
 * else falls through to the reader as an ordinary byte.
 *
 * Called on the receive path, so it takes no locks: a line discipline decides
 * what a byte means as it arrives, not when someone gets around to reading it.
 * That is the whole reason the check cannot live in tty_console_read() -- a
 * foreground child that never reads the console (sleep, a compute loop) would
 * then never see the key, and Ctrl-C would not stop it. */
int tty_console_isig_signal(char c)
{
    const ktty_termios_t *tio = &g_dev_tty.termios;
    if (!(tio->c_lflag & DEVFS_ISIG))
        return 0;
    if (c == (char)tio->c_cc[DEVFS_VINTR]) return SIGINT;
    if (c == (char)tio->c_cc[DEVFS_VQUIT]) return SIGQUIT;
    if (c == (char)tio->c_cc[DEVFS_VSUSP]) return SIGTSTP;
    return 0;
}

/* Deliver an ISIG signal to the console's foreground process group.  Separate
 * from the decision above because this takes the UART's rx_lock (through
 * uart_get_foreground_pgid), so callers must be outside that lock. */
void tty_console_isig_raise(int sig)
{
    tty_control_signal_foreground(TTY_CONSOLE_ID, sig);
}

static void fill_default_termios(ktty_termios_t *tio) {
    memset(tio, 0, sizeof(*tio));
    tio->c_iflag = 0x500;
    tio->c_oflag = 0x5;
    tio->c_cflag = 0xBF;
    tio->c_lflag = 0x8a3b;
    tio->c_cc[0] = 3;
    tio->c_cc[1] = 28;
    tio->c_cc[2] = 127;
    tio->c_cc[3] = 21;
    tio->c_cc[4] = 4;
    tio->c_cc[5] = 0;
    tio->c_cc[6] = 1;
    tio->c_cc[8] = 17;
    tio->c_cc[9] = 19;
    tio->c_cc[10] = 26;
    tio->c_cc[12] = 18;
    tio->c_cc[13] = 15;
    tio->c_cc[14] = 23;
    tio->c_cc[15] = 22;
}

void tty_console_init(void) {
    tty_state_t *tty = &g_dev_tty;
    memset(tty, 0, sizeof(*tty));
    fill_default_termios(&tty->termios);
    tty->winsize.ws_row = 24;
    tty->winsize.ws_col = 80;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;
    tty->kbmode = 0x01;    /* K_XLATE */
    tty->kdmode = 0x00;    /* KD_TEXT */
    tty->vt_process = 0;   /* VT_AUTO */
    const char *args = bootargs_get();
    const char option[] = "a20.console=vport0";
    while (args && *args) {
        while (*args == ' ' || *args == '\t') args++;
        const char *word = args;
        while (*args && *args != ' ' && *args != '\t') args++;
        if ((size_t)(args - word) == sizeof(option) - 1 &&
            !memcmp(word, option, sizeof(option) - 1))
            g_tty_use_bulk_console = 1;
    }
}

int tty_console_read(vfile_t *vf, char *buf, size_t count) {
    if (count == 0 || tty_control_revoked(vf)) return 0;
    int access = tty_control_access(vf, 0, 0);
    if (access < 0) return access;

    /* drain pending mouse escape-sequence bytes first */
    if (g_dev_tty.mouse_enabled && !mouse_buf_empty()) {
        size_t i = 0;
        while (i < count && !mouse_buf_empty()) {
            int b = mouse_buf_pop_byte();
            if (b < 0) break;
            buf[i++] = (char)b;
        }
        return (int)i;
    }

    for (;;) {
        int c = uart_getc_vfile(vf);
        if (c == -EIO && tty_control_revoked(vf)) return 0;
        /* -ERESTARTSYS from an interrupted wait: hand it to the caller so the
         * syscall can be restarted or reported as EINTR, the way pty reads
         * already do (kernel/drivers/char/pty.c).  ISIG control characters
         * never reach here: the receive path consumes them. */
        if (c < 0) return c;
        if (c == '\r') c = '\n';
        if (g_dev_tty.termios.c_lflag & DEVFS_ECHO) {
            uint64_t kf = spin_lock_irqsave(&klog_console_lock);
            uart_putc((char)c);
            spin_unlock_irqrestore(&klog_console_lock, kf);
        }
        buf[0] = (char)c;
        return 1;
    }
}

/*
 * Scan a console write buffer for xterm private-mode escape sequences
 * ("\033[?NNNh" / "\033[?NNNl") that enable/disable mouse tracking.
 * Returns 1 if mouse mode was toggled, 0 otherwise.
 */
static int tty_parse_mouse_mode(const char *buf, size_t count)
{
    for (size_t i = 0; i + 4 < count; i++) {
        if (buf[i] != '\033' || buf[i+1] != '[' || buf[i+2] != '?')
            continue;
        size_t j = i + 3;
        int num = 0;
        int digits = 0;
        while (j < count && buf[j] >= '0' && buf[j] <= '9' && digits < 6) {
            num = num * 10 + (buf[j] - '0');
            digits++;
            j++;
        }
        if (digits == 0 || j >= count)
            continue;
        char mode = buf[j];
        if (mode == 'h' || mode == 'l') {
            int enable = (mode == 'h');
            switch (num) {
            case 1000: g_dev_tty.mouse_enabled = enable ? 1 : 0; break;
            case 1002: g_dev_tty.mouse_enabled = enable ? 2 : 0; break;
            case 1003: g_dev_tty.mouse_enabled = enable ? 4 : 0; break;
            case 1006: g_dev_tty.mouse_sgr = enable ? 1 : 0; break;
            }
        }
    }
    return 0;
}

int tty_console_write(vfile_t *vf, const char *buf, size_t count) {
    if (!count) return 0;
    int access = tty_control_access(vf, 1, 0);
    if (access < 0) return access;
    /* One owner for the entire userspace write. Kernel records use the same
     * console spinlock for each bounded burst, so they cannot splice bytes
     * into a short line. No PID lookup or pending per-task buffers are needed. */
    mutex_lock(&g_tty_write_lock);
    if (tty_control_revoked(vf)) { mutex_unlock(&g_tty_write_lock); return -EIO; }
    tty_parse_mouse_mode(buf, count);
    if (g_tty_use_bulk_console) {
        if (!g_tty_bulk_console)
            g_tty_bulk_console = class_device_get_by_name("vport0");
        const void *class_ops;
        if (g_tty_bulk_console &&
            !class_device_ops_begin(g_tty_bulk_console, DEV_CLASS_CHAR, &class_ops)) {
            const char_dev_ops_t *ops = class_ops;
            if (!g_tty_bulk_ready) {
                /* A stale raw-port module can bind successfully but has no
                 * MULTIPORT handshake. Verify the wire contract before ever
                 * handing it terminal bytes. */
                if (!ops->ioctl || ops->ioctl(g_tty_bulk_console->dev,
                                             VPORT_IOCTL_CONSOLE_READY, NULL)) {
                    class_device_call_end(g_tty_bulk_console);
                    class_device_put(g_tty_bulk_console);
                    g_tty_bulk_console = NULL;
                    goto serial_console;
                }
                g_tty_bulk_ready = 1;
            }
            size_t sent = 0;
            int error = -ENOSYS;
            while (sent < count && ops->write) {
                if (signal_task_has_unblocked(proc_current())) {
                    error = -ERESTARTSYS;
                    break;
                }
                int n = ops->write(g_tty_bulk_console->dev, buf + sent, count - sent);
                if (n <= 0) { error = n; break; }
                sent += (size_t)n;
            }
            class_device_call_end(g_tty_bulk_console);
            mutex_unlock(&g_tty_write_lock);
            /* A timeout may leave a DMA frame in flight. Never replay that
             * frame onto the UART: only the device can retire its ownership. */
            return sent ? (int)sent : error;
        }
        if (g_tty_bulk_console) {
            class_device_put(g_tty_bulk_console);
            g_tty_bulk_console = NULL;
            g_tty_bulk_ready = 0;
        }
        /* Early boot or unplug: the serial console remains available. */
    }
serial_console:
    for (size_t i = 0; i < count; ) {
        size_t n = count - i;
        if (n > TTY_EMIT_CHUNK) {
            n = TTY_EMIT_CHUNK;
            /* Prefer the last newline, retaining multiple short lines in one
             * burst while avoiding a split record where possible. */
            for (size_t j = n; j > 0; j--)
                if (buf[i + j - 1] == '\n') { n = j; break; }
        }
        uint64_t kf = spin_lock_irqsave(&klog_console_lock);
        uart_write(buf + i, n);
        spin_unlock_irqrestore(&klog_console_lock, kf);
        i += n;
    }
    mutex_unlock(&g_tty_write_lock);
    return (int)count;
}

#define DEVFS_KDSETMODE    0x4B3A
#define DEVFS_KDGETMODE    0x4B3B
#define DEVFS_KDGKBMODE    0x4B44
#define DEVFS_KDSKBMODE    0x4B45
#define DEVFS_KDGKBTYPE    0x4B33
#define DEVFS_KB_101       0x02
#define DEVFS_VT_OPENQRY   0x5600
#define DEVFS_VT_GETMODE   0x5601
#define DEVFS_VT_SETMODE   0x5602
#define DEVFS_VT_GETSTATE  0x5603
#define DEVFS_VT_RELDISP   0x5605
#define DEVFS_VT_ACTIVATE  0x5606
#define DEVFS_VT_WAITACTIVE 0x5607
#define DEVFS_TIOCSWINSZ   0x5414

typedef struct __attribute__((packed)) {
    uint8_t mode;
    uint8_t waitv;
    int16_t relsig;
    int16_t acqsig;
    int16_t frsig;
} devfs_vt_mode_t;

typedef struct {
    uint16_t v_active;
    uint16_t v_signal;
    uint16_t v_state;
} devfs_vt_stat_t;

static spinlock_t g_tty_settings_lock = SPINLOCK_INIT;

int tty_console_get_termios(ktty_termios_t *out) {
    uint64_t flags = spin_lock_irqsave(&g_tty_settings_lock);
    *out = g_dev_tty.termios;
    spin_unlock_irqrestore(&g_tty_settings_lock, flags);
    return 0;
}
int tty_console_flush(int queue) {
    if (queue < 0 || queue > 2) return -EINVAL;
    if (queue == 0 || queue == 2)
        for (unsigned n = 0; n < 4096 && uart_try_getc() >= 0; n++) {}
    return 0;
}

static int tty_console_drain(void) {
    int result = 0;
    mutex_lock(&g_tty_write_lock);
    const void *class_ops;
    if (g_tty_bulk_console &&
        !class_device_ops_begin(g_tty_bulk_console, DEV_CLASS_CHAR, &class_ops)) {
        const char_dev_ops_t *ops = class_ops;
        result = ops->ioctl ? ops->ioctl(g_tty_bulk_console->dev, VPORT_IOCTL_DRAIN, NULL) : -ENOTTY;
        class_device_call_end(g_tty_bulk_console);
    } else {
        uart_flush();
    }
    mutex_unlock(&g_tty_write_lock);
    return result;
}

int tty_console_set_termios(const ktty_termios_t *in, int action) {
    if (action < 0 || action > 2) return -EINVAL;
    if (action) {
        int result = tty_console_drain();
        if (result) return result;
    }
    if (action == 2) tty_console_flush(0);
    uint64_t flags = spin_lock_irqsave(&g_tty_settings_lock);
    g_dev_tty.termios = *in;
    spin_unlock_irqrestore(&g_tty_settings_lock, flags);
    return 0;
}
int tty_console_get_winsize(ktty_winsize_t *out) {
    uint64_t flags = spin_lock_irqsave(&g_tty_settings_lock);
    memcpy(out, &g_dev_tty.winsize, sizeof(*out));
    spin_unlock_irqrestore(&g_tty_settings_lock, flags);
    return 0;
}
int tty_console_set_winsize(const ktty_winsize_t *in) {
    uint64_t flags = spin_lock_irqsave(&g_tty_settings_lock);
    memcpy(&g_dev_tty.winsize, in, sizeof(*in));
    spin_unlock_irqrestore(&g_tty_settings_lock, flags);
    return 0;
}

int tty_console_ioctl(unsigned long req, void *arg) {
    /* tcdrain passes the value 1, not a userspace pointer. A zero argument
     * requests a break, which this virtual console does not implement. */
    if (req == TCSBRK)
        return (uintptr_t)arg ? tty_console_drain() : -EOPNOTSUPP;
    /* Value-passing requests (no pointer dereference): the argument is the
     * mode/VT number itself, so NULL is a legitimate value (K_RAW/KD_TEXT). */
    if (req == DEVFS_KDSKBMODE) {
        g_dev_tty.kbmode = (int)(uintptr_t)arg;
        return 0;
    }
    if (req == DEVFS_KDSETMODE) {
        g_dev_tty.kdmode = (int)(uintptr_t)arg;
        return 0;
    }
    if (req == DEVFS_VT_RELDISP || req == DEVFS_VT_ACTIVATE ||
        req == DEVFS_VT_WAITACTIVE) {
        return 0;
    }
    if (!arg)
        return -EFAULT;
    if (req == TCGETS)
        return arch_tty_termios_to_user(arg, &g_dev_tty.termios) < 0 ? -EFAULT : 0;
    if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
        ktty_termios_t tio;
        if (arch_tty_termios_from_user(&tio, arg) < 0)
            return -EFAULT;
        g_dev_tty.termios = tio;
        return 0;
    }
    if (req == TIOCGWINSZ) {
        if (copy_to_user(arg, &g_dev_tty.winsize, sizeof(g_dev_tty.winsize)) < 0)
            return -EFAULT;
        return 0;
    }
    if (req == DEVFS_TIOCSWINSZ) {
        kwinsize_t ws;
        if (copy_from_user(&ws, arg, sizeof(ws)) < 0)
            return -EFAULT;
        g_dev_tty.winsize = ws;
        return 0;
    }
    /* ---- console KD/VT ioctls: single-VT semantics ---- */
    if (req == DEVFS_KDGKBMODE) {
        if (copy_to_user(arg, &g_dev_tty.kbmode, sizeof(int)) < 0) return -EFAULT;
        return 0;
    }
    if (req == DEVFS_KDGETMODE) {
        if (copy_to_user(arg, &g_dev_tty.kdmode, sizeof(int)) < 0) return -EFAULT;
        return 0;
    }
    if (req == DEVFS_KDGKBTYPE) {
        char kbtype = DEVFS_KB_101;
        if (copy_to_user(arg, &kbtype, sizeof(kbtype)) < 0) return -EFAULT;
        return 0;
    }
    if (req == DEVFS_VT_OPENQRY) {
        int free_vt = 1;
        if (copy_to_user(arg, &free_vt, sizeof(free_vt)) < 0) return -EFAULT;
        return 0;
    }
    if (req == DEVFS_VT_GETMODE) {
        devfs_vt_mode_t vm = {0};
        vm.mode = (uint8_t)g_dev_tty.vt_process;
        if (copy_to_user(arg, &vm, sizeof(vm)) < 0) return -EFAULT;
        return 0;
    }
    if (req == DEVFS_VT_SETMODE) {
        devfs_vt_mode_t vm;
        if (copy_from_user(&vm, arg, sizeof(vm)) < 0) return -EFAULT;
        g_dev_tty.vt_process = vm.mode;
        return 0;
    }
    if (req == DEVFS_VT_GETSTATE) {
        devfs_vt_stat_t vs = {0};
        vs.v_active = 1;
        vs.v_state = 1;
        if (copy_to_user(arg, &vs, sizeof(vs)) < 0) return -EFAULT;
        return 0;
    }
    return -ENOTTY;
}

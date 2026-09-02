#include "fs/tty.h"
#include "fs/file.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/sync.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "core/ioctl.h"

extern void uart_putc(char c);
extern int  uart_getc(void);

/*
 * Console TTY line discipline, termios state and the stdin/stdout console
 * file operations, split out of fs/devfs.c.  Owns the single virtual console
 * (g_dev_tty) plus the per-pid line-buffer hand-off used to serialize
 * interleaved console writes; devfs.c only dispatches to these via fs/tty.h.
 */

#define TTY_LINE_SLOTS 16
#define TTY_LINE_BUF_SIZE 256

typedef struct tty_line_buffer {
    int pid;
    size_t len;
    char data[TTY_LINE_BUF_SIZE];
} tty_line_buffer_t;

static mutex_t g_tty_write_lock = MUTEX_INIT;
static int g_tty_line_owner = -1;
static tty_line_buffer_t g_tty_line_buffers[TTY_LINE_SLOTS];

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
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[KTTY_NCCS];
    uint8_t  c_line;
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} ppc64_tty_termios_t;

_Static_assert(sizeof(ppc64_tty_termios_t) == 44,
               "PPC64 termios must be 44 bytes");

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
}

int tty_console_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    if (count == 0) return 0;

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

    int c = uart_getc();
    if (c < 0) return 0;
    if (c == '\r') c = '\n';
    if (g_dev_tty.termios.c_lflag & 0x00000008U)
        uart_putc((char)c);
    buf[0] = (char)c;
    return 1;
}

static tty_line_buffer_t *tty_line_buffer_for(int pid) {
    tty_line_buffer_t *free_slot = NULL;
    for (int i = 0; i < TTY_LINE_SLOTS; i++) {
        if (g_tty_line_buffers[i].len > 0 && g_tty_line_buffers[i].pid == pid)
            return &g_tty_line_buffers[i];
        if (!free_slot && g_tty_line_buffers[i].len == 0)
            free_slot = &g_tty_line_buffers[i];
    }
    if (free_slot) {
        free_slot->pid = pid;
        free_slot->len = 0;
    }
    return free_slot;
}

static void tty_write_owned_char(int pid, char c) {
    if (g_tty_line_owner < 0)
        g_tty_line_owner = pid;
    uart_putc(c);
    if (c == '\n')
        g_tty_line_owner = -1;
}

static int tty_line_owner_live_locked(void) {
    if (g_tty_line_owner < 0)
        return 0;
    task_t *owner = proc_find_get(g_tty_line_owner);
    int live =
        owner && owner->state != PROC_ZOMBIE && owner->state != PROC_UNUSED;
    proc_put(owner);
    return live;
}

static void tty_drain_pending_locked(void) {
    int progress = 1;
    while (g_tty_line_owner < 0 && progress) {
        progress = 0;
        for (int i = 0; i < TTY_LINE_SLOTS; i++) {
            tty_line_buffer_t *b = &g_tty_line_buffers[i];
            if (b->len == 0) continue;
            int pid = b->pid;
            size_t n = b->len;
            char tmp[TTY_LINE_BUF_SIZE];
            memcpy(tmp, b->data, n);
            b->len = 0;
            progress = 1;
            for (size_t j = 0; j < n; j++)
                tty_write_owned_char(pid, tmp[j]);
            /* A buffered fragment that does not end in '\n' (e.g. a shell
             * prompt) must not keep the line owner claimed: that would stop
             * the drain here and park every other pending buffer forever,
             * silently dropping their console output. */
            g_tty_line_owner = -1;
        }
    }
}

static void tty_release_dead_owner_locked(void) {
    if (g_tty_line_owner >= 0 && !tty_line_owner_live_locked()) {
        g_tty_line_owner = -1;
        tty_drain_pending_locked();
    }
}

static void tty_buffer_pending_char(int pid, char c) {
    tty_line_buffer_t *b = tty_line_buffer_for(pid);
    if (!b) {
        uart_putc(c);
        if (c == '\n')
            g_tty_line_owner = -1;
        return;
    }
    if (b->len >= TTY_LINE_BUF_SIZE) {
        for (size_t i = 0; i < b->len; i++)
            uart_putc(b->data[i]);
        b->len = 0;
    }
    b->data[b->len++] = c;
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
    (void)vf;
    task_t *t = proc_current();
    int pid = t ? t->pid : -1;

    tty_parse_mouse_mode(buf, count);

    mutex_lock(&g_tty_write_lock);
    tty_release_dead_owner_locked();
    for (size_t i = 0; i < count; i++) {
        char c = buf[i];
        if (pid < 0 || g_tty_line_owner < 0 || g_tty_line_owner == pid) {
            tty_write_owned_char(pid, c);
        } else {
            tty_buffer_pending_char(pid, c);
        }
    }
    /* Ownership is per write() call, not per '\n': a newline-less write
     * (a shell prompt, a progress spinner) must not leave the line claimed
     * forever, otherwise every other task's console output is parked in the
     * pending buffers and the drain is never reached. */
    g_tty_line_owner = -1;
    tty_drain_pending_locked();
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

int tty_console_ioctl(unsigned long req, void *arg) {
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
    if (req == TCGETS || req == PPC64_TCGETS) {
        if (req == PPC64_TCGETS) {
            ppc64_tty_termios_t ppc = {
                .c_iflag = g_dev_tty.termios.c_iflag,
                .c_oflag = g_dev_tty.termios.c_oflag,
                .c_cflag = g_dev_tty.termios.c_cflag,
                .c_lflag = g_dev_tty.termios.c_lflag,
                .c_line = g_dev_tty.termios.c_line,
                .c_ispeed = 0,
                .c_ospeed = 0,
            };
            memcpy(ppc.c_cc, g_dev_tty.termios.c_cc, KTTY_NCCS);
            if (copy_to_user(arg, &ppc, sizeof(ppc)) < 0) return -EFAULT;
            return 0;
        }
        if (copy_to_user(arg, &g_dev_tty.termios, sizeof(g_dev_tty.termios)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TCSETS || req == TCSETSW || req == TCSETSF ||
        req == PPC64_TCSETS || req == PPC64_TCSETSW || req == PPC64_TCSETSF) {
        if (req == PPC64_TCSETS || req == PPC64_TCSETSW || req == PPC64_TCSETSF) {
            ppc64_tty_termios_t ppc;
            if (copy_from_user(&ppc, arg, sizeof(ppc)) < 0) return -EFAULT;
            g_dev_tty.termios.c_iflag = ppc.c_iflag;
            g_dev_tty.termios.c_oflag = ppc.c_oflag;
            g_dev_tty.termios.c_cflag = ppc.c_cflag;
            g_dev_tty.termios.c_lflag = ppc.c_lflag;
            g_dev_tty.termios.c_line = ppc.c_line;
            memcpy(g_dev_tty.termios.c_cc, ppc.c_cc, KTTY_NCCS);
            return 0;
        }
        ktty_termios_t tio;
        if (copy_from_user(&tio, arg, sizeof(tio)) < 0) return -EFAULT;
        g_dev_tty.termios = tio;
        return 0;
    }
    if (req == TIOCGWINSZ || req == PPC64_TIOCGWINSZ) {
        if (copy_to_user(arg, &g_dev_tty.winsize, sizeof(g_dev_tty.winsize)) < 0) return -EFAULT;
        return 0;
    }
    if (req == DEVFS_TIOCSWINSZ || req == PPC64_TIOCSWINSZ) {
        kwinsize_t ws;
        if (copy_from_user(&ws, arg, sizeof(ws)) < 0) return -EFAULT;
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

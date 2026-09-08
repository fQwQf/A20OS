#include "core/poll.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/ioctl.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "core/errno.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "proc/signal.h"

#define MAX_PTYS       64
#define PTY_BUF_SIZE   4096

#define TIOCGPTN       0x80045430
#define TIOCSPTLCK     0x40045431
#define TIOCGPTP       0x80045434
#define TCGETS         0x5401
#define TCSETS         0x5402
#define TCSETSW        0x5403
#define TCSETSF        0x5404
#define TIOCGWINSZ     0x5413
#define TIOCSWINSZ     0x5414
#define TIOCSCTTY      0x540E
#define TIOCNOTTY      0x5422
#define FIONBIO        0x5421
#define PTY_NCCS       32

/* termios flag bits used by the line discipline (Linux values). */
#define PTY_OPOST      0x1
#define PTY_ONLCR      0x4
#define PTY_ICRNL      0x100
#define PTY_ICANON     0x2
#define PTY_ECHO       0x8
#define PTY_ECHOE      0x10
#define PTY_ECHOK      0x20
#define PTY_CC_VERASE  2
#define PTY_CC_VKILL   3
#define PTY_CC_VEOF    4

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[PTY_NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} pty_termios_t;

typedef struct {
    /* LOCK_ORDER: per-pair lock protects ring buffers, master/slave refs,
     * locked flag, nonblock flags, and window size. No nesting with
     * g_pty_alloc_lock or other kernel locks. */
    spinlock_t  lock;
    char       *m2s_buf;
    char       *s2m_buf;
    size_t      m2s_head, m2s_tail, m2s_used;
    size_t      s2m_head, s2m_tail, s2m_used;
    int         in_use;
    int         locked;
    int         master_refs;
    int         slave_refs;
    uint16_t    ws_row, ws_col;
    int         pgrp;           /* foreground process group (TIOCGPGRP/SPGRP) */
    int         master_nonblock;
    int         slave_nonblock;
    int         packet_mode;    /* TIOCPKT: master reads get a status prefix */
    /* Canonical-mode (ICANON) line editing state: master input is cooked
     * into canon_buf and slave reads complete lines from it. */
    char       *canon_buf;
    size_t      canon_len;
    size_t      canon_lines;
    int         master_waiting;
    int         slave_waiting;
    wait_queue_t master_readers;
    wait_queue_t slave_readers;
    pty_termios_t termios;
} pty_pair_t;

static pty_pair_t g_ptys[MAX_PTYS];
/* LOCK_ORDER: g_pty_alloc_lock protects in_use during allocation and initial
 * buffer setup only. Never nested under or over the per-pair lock. */
static spinlock_t g_pty_alloc_lock;

static void pty_fill_default_termios(pty_termios_t *tio) {
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
    tio->c_cc[6] = 1;
    tio->c_cc[8] = 17;
    tio->c_cc[9] = 19;
    tio->c_cc[10] = 26;
    tio->c_cc[12] = 18;
    tio->c_cc[13] = 15;
    tio->c_cc[14] = 23;
    tio->c_cc[15] = 22;
}

void pty_init(void) {
    /* LOCK_ORDER: initialize the allocation lock before any pty_alloc() call. */
    spin_init(&g_pty_alloc_lock);
    for (int i = 0; i < MAX_PTYS; i++) {
        memset(&g_ptys[i], 0, sizeof(g_ptys[i]));
        /* LOCK_ORDER: initialize each per-pair lock at boot. */
        spin_init(&g_ptys[i].lock);
    }
}

static int pty_alloc(void) {
    /* LOCK_ORDER: acquire g_pty_alloc_lock for allocation only;
     * per-pair lock is not held. */
    uint64_t flags = spin_lock_irqsave(&g_pty_alloc_lock);
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!g_ptys[i].in_use) {
            g_ptys[i].in_use = 1;
            g_ptys[i].m2s_buf = (char *)kmalloc(PTY_BUF_SIZE);
            g_ptys[i].s2m_buf = (char *)kmalloc(PTY_BUF_SIZE);
            g_ptys[i].canon_buf = (char *)kmalloc(PTY_BUF_SIZE);
            if (!g_ptys[i].m2s_buf || !g_ptys[i].s2m_buf || !g_ptys[i].canon_buf) {
                if (g_ptys[i].m2s_buf) kfree(g_ptys[i].m2s_buf);
                if (g_ptys[i].s2m_buf) kfree(g_ptys[i].s2m_buf);
                if (g_ptys[i].canon_buf) kfree(g_ptys[i].canon_buf);
                g_ptys[i].m2s_buf = NULL;
                g_ptys[i].s2m_buf = NULL;
                g_ptys[i].canon_buf = NULL;
                g_ptys[i].in_use = 0;
                spin_unlock_irqrestore(&g_pty_alloc_lock, flags);
                return -ENOMEM;
            }
            g_ptys[i].m2s_head = g_ptys[i].m2s_tail = g_ptys[i].m2s_used = 0;
            g_ptys[i].s2m_head = g_ptys[i].s2m_tail = g_ptys[i].s2m_used = 0;
            g_ptys[i].canon_len = 0;
            g_ptys[i].canon_lines = 0;
            g_ptys[i].locked = 0;
            g_ptys[i].master_refs = 1;
            g_ptys[i].slave_refs = 0;
            g_ptys[i].ws_row = 24;
            g_ptys[i].ws_col = 80;
            g_ptys[i].master_nonblock = 0;
            g_ptys[i].slave_nonblock = 0;
            g_ptys[i].packet_mode = 0;
            g_ptys[i].master_waiting = 0;
            g_ptys[i].slave_waiting = 0;
            wait_queue_init(&g_ptys[i].master_readers);
            wait_queue_init(&g_ptys[i].slave_readers);
            pty_fill_default_termios(&g_ptys[i].termios);
            spin_unlock_irqrestore(&g_pty_alloc_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&g_pty_alloc_lock, flags);
    return -ENOSPC;
}

static void pty_maybe_free_locked(pty_pair_t *pty) {
    if (pty->master_refs != 0 || pty->slave_refs != 0 ||
        pty->master_waiting != 0 || pty->slave_waiting != 0)
        return;

    if (pty->m2s_buf) {
        kfree(pty->m2s_buf);
        pty->m2s_buf = NULL;
    }
    if (pty->s2m_buf) {
        kfree(pty->s2m_buf);
        pty->s2m_buf = NULL;
    }
    if (pty->canon_buf) {
        kfree(pty->canon_buf);
        pty->canon_buf = NULL;
    }
    pty->in_use = 0;
}

static size_t ring_write(char *buf, size_t cap, size_t *head, size_t *used,
                         const char *data, size_t count) {
    size_t avail = cap - *used;
    size_t n = count < avail ? count : avail;
    for (size_t i = 0; i < n; i++) {
        buf[*head] = data[i];
        *head = (*head + 1) % cap;
    }
    *used += n;
    return n;
}

static size_t ring_read(char *buf, size_t cap, size_t *tail, size_t *used,
                        char *out, size_t count) {
    size_t n = *used < count ? *used : count;
    for (size_t i = 0; i < n; i++) {
        out[i] = buf[*tail];
        *tail = (*tail + 1) % cap;
    }
    *used -= n;
    return n;
}

/* Echo one input byte back toward the master, applying ONLCR to newlines.
 * Caller holds the per-pair lock. */
static void pty_echo_byte_locked(pty_pair_t *pty, char ch) {
    if (ch == '\n' &&
        (pty->termios.c_oflag & (PTY_OPOST | PTY_ONLCR)) == (PTY_OPOST | PTY_ONLCR)) {
        ring_write(pty->s2m_buf, PTY_BUF_SIZE, &pty->s2m_head,
                   &pty->s2m_used, "\r\n", 2);
        return;
    }
    ring_write(pty->s2m_buf, PTY_BUF_SIZE, &pty->s2m_head,
               &pty->s2m_used, &ch, 1);
}

/* Feed one byte of master-side input through the line discipline.  In
 * ICANON mode the byte is cooked into canon_buf (ERASE/KILL/EOF/CRNL);
 * otherwise it lands in the raw m2s ring.  Caller holds the per-pair lock. */
static void pty_input_byte_locked(pty_pair_t *pty, char ch) {
    pty_termios_t *tio = &pty->termios;
    if (tio->c_lflag & PTY_ICANON) {
        if (ch == '\r' && (tio->c_iflag & PTY_ICRNL))
            ch = '\n';
        if (ch == (char)tio->c_cc[PTY_CC_VERASE]) {
            if (pty->canon_len > 0) {
                pty->canon_len--;
                if (tio->c_lflag & PTY_ECHOE)
                    ring_write(pty->s2m_buf, PTY_BUF_SIZE, &pty->s2m_head,
                               &pty->s2m_used, "\b \b", 3);
            }
            return;
        }
        if (ch == (char)tio->c_cc[PTY_CC_VKILL]) {
            pty->canon_len = 0;
            if (tio->c_lflag & PTY_ECHOK)
                pty_echo_byte_locked(pty, '\n');
            return;
        }
        if (ch == (char)tio->c_cc[PTY_CC_VEOF]) {
            /* Flush the pending line without a newline; an empty pending
             * line makes the reader see EOF (a zero-length read). */
            pty->canon_lines++;
            return;
        }
        if (pty->canon_len >= PTY_BUF_SIZE)
            pty->canon_lines++;   /* line-full: force-terminate, drop nothing */
        if (pty->canon_len < PTY_BUF_SIZE)
            pty->canon_buf[pty->canon_len++] = ch;
        if (tio->c_lflag & PTY_ECHO)
            pty_echo_byte_locked(pty, ch);
        if (ch == '\n')
            pty->canon_lines++;
        return;
    }
    ring_write(pty->m2s_buf, PTY_BUF_SIZE, &pty->m2s_head,
               &pty->m2s_used, &ch, 1);
    if (tio->c_lflag & PTY_ECHO)
        pty_echo_byte_locked(pty, ch);
}

/* Bytes the slave can read right now: cooked lines in ICANON mode, raw
 * ring bytes otherwise.  Caller holds the per-pair lock. */
static size_t pty_slave_readable_locked(pty_pair_t *pty) {
    if (pty->termios.c_lflag & PTY_ICANON)
        return pty->canon_lines;
    return pty->m2s_used;
}

static int pty_wait_interruptible_locked(pty_pair_t *pty, wait_queue_t *wq,
                                         int *waiting, uint64_t *pty_flags) {
    task_t *task = proc_current();
    if (!task) {
        spin_unlock_irqrestore(&pty->lock, *pty_flags);
        proc_yield();
        *pty_flags = spin_lock_irqsave(&pty->lock);
        return 0;
    }
    if (signal_task_has_unblocked(task))
        return -ERESTARTSYS;

    spin_unlock_irqrestore(&pty->lock, *pty_flags);
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    *pty_flags = spin_lock_irqsave(&pty->lock);
    if (!token.task)
        return 0;

    int should_wait =
        (wq == &pty->master_readers) ?
            (pty->s2m_used == 0 && pty->slave_refs != 0) :
            (pty_slave_readable_locked(pty) == 0 && pty->master_refs != 0);
    int interrupted = signal_task_has_unblocked(task);
    if (!should_wait || interrupted) {
        spin_unlock_irqrestore(&pty->lock, *pty_flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        *pty_flags = spin_lock_irqsave(&pty->lock);
        return interrupted ? -ERESTARTSYS : 0;
    }

    wait_queue_entry_t entry = {0};
    bool linked = wait_queue_link(wq, &entry, token, 0);
    (*waiting)++;

    spin_unlock_irqrestore(&pty->lock, *pty_flags);
    proc_wake_reason_t reason;
    if (linked)
        reason = proc_park_commit(token);
    else {
        (void)proc_park_cancel(token);
        reason = PROC_WAKE_CANCEL;
    }
    wait_queue_unlink(wq, &entry);
    proc_park_finish(token);
    *pty_flags = spin_lock_irqsave(&pty->lock);
    (*waiting)--;

    if (proc_wake_reason_is_task_interrupt(reason) ||
        signal_task_has_unblocked(task))
        return -ERESTARTSYS;
    return 0;
}

int pty_master_read(int idx, char *buf, size_t count, int nonblock) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    if (count == 0) return 0;

    pty_pair_t *pty = &g_ptys[idx];
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return -EIO;
    }
    while (pty->s2m_used == 0) {
        if (pty->slave_refs == 0) {
            spin_unlock_irqrestore(&pty->lock, flags);
            return 0;
        }
        if (nonblock || pty->master_nonblock) {
            spin_unlock_irqrestore(&pty->lock, flags);
            return -EAGAIN;
        }
        int result = pty_wait_interruptible_locked(
            pty, &pty->master_readers, &pty->master_waiting, &flags);
        if (result < 0) {
            pty_maybe_free_locked(pty);
            spin_unlock_irqrestore(&pty->lock, flags);
            return result;
        }
    }
    size_t n;
    if (pty->packet_mode) {
        buf[0] = 0; /* TIOCPKT_DATA: no flow-control/termios events exist */
        n = 1;
        if (count > 1)
            n += ring_read(pty->s2m_buf, PTY_BUF_SIZE,
                           &pty->s2m_tail, &pty->s2m_used,
                           buf + 1, count - 1);
    } else {
        n = ring_read(pty->s2m_buf, PTY_BUF_SIZE,
                      &pty->s2m_tail, &pty->s2m_used,
                      buf, count);
    }
    spin_unlock_irqrestore(&pty->lock, flags);
    return (int)n;
}

int pty_master_write(int idx, const char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to feed input through the line
     * discipline (canonical cooking or raw ring) plus echo. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (!g_ptys[idx].in_use) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EIO;
    }
    if (g_ptys[idx].slave_refs == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EPIPE;
    }
    size_t old_s2m = g_ptys[idx].s2m_used;
    for (size_t i = 0; i < count; i++)
        pty_input_byte_locked(&g_ptys[idx], buf[i]);
    int echoed = g_ptys[idx].s2m_used != old_s2m;
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    wait_queue_wake_all(&g_ptys[idx].slave_readers, 0, PROC_WAKE_EVENT);
    if (echoed)
        wait_queue_wake_all(&g_ptys[idx].master_readers, 0, PROC_WAKE_EVENT);
    return (int)count;
}

int pty_slave_read(int idx, char *buf, size_t count, int nonblock) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    if (count == 0) return 0;

    pty_pair_t *pty = &g_ptys[idx];
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return -EIO;
    }
    while (pty_slave_readable_locked(pty) == 0) {
        if (pty->master_refs == 0) {
            spin_unlock_irqrestore(&pty->lock, flags);
            return 0;
        }
        if (nonblock || pty->slave_nonblock) {
            spin_unlock_irqrestore(&pty->lock, flags);
            return -EAGAIN;
        }
        int result = pty_wait_interruptible_locked(
            pty, &pty->slave_readers, &pty->slave_waiting, &flags);
        if (result < 0) {
            pty_maybe_free_locked(pty);
            spin_unlock_irqrestore(&pty->lock, flags);
            return result;
        }
    }
    size_t n;
    if (pty->termios.c_lflag & PTY_ICANON) {
        /* One cooked line per read: up to and including the newline, or
         * the whole pending buffer for a VEOF-flushed (newline-less) line. */
        size_t line_len = 0;
        while (line_len < pty->canon_len && pty->canon_buf[line_len] != '\n')
            line_len++;
        if (line_len < pty->canon_len)
            line_len++;
        n = line_len < count ? line_len : count;
        memcpy(buf, pty->canon_buf, n);
        pty->canon_len -= n;
        memmove(pty->canon_buf, pty->canon_buf + n, pty->canon_len);
        if (n == line_len && pty->canon_lines > 0)
            pty->canon_lines--;
    } else {
        n = ring_read(pty->m2s_buf, PTY_BUF_SIZE,
                      &pty->m2s_tail, &pty->m2s_used,
                      buf, count);
    }
    spin_unlock_irqrestore(&pty->lock, flags);
    return (int)n;
}

int pty_master_poll(int idx, short events) {
    if (idx < 0 || idx >= MAX_PTYS) return POLLNVAL;
    pty_pair_t *pty = &g_ptys[idx];
    int revents = 0;
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        revents = POLLNVAL;
    } else {
        if ((events & POLLIN) && (pty->s2m_used > 0 || pty->slave_refs == 0))
            revents |= POLLIN;
        if ((events & POLLOUT) && pty->slave_refs > 0 &&
            pty->m2s_used < PTY_BUF_SIZE)
            revents |= POLLOUT;
        if (pty->slave_refs == 0)
            revents |= POLLHUP;
    }
    spin_unlock_irqrestore(&pty->lock, flags);
    return revents;
}

int pty_slave_poll(int idx, short events) {
    if (idx < 0 || idx >= MAX_PTYS) return POLLNVAL;
    pty_pair_t *pty = &g_ptys[idx];
    int revents = 0;
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        revents = POLLNVAL;
    } else {
        if ((events & POLLIN) &&
            (pty_slave_readable_locked(pty) > 0 || pty->master_refs == 0))
            revents |= POLLIN;
        if ((events & POLLOUT) && pty->master_refs > 0 &&
            pty->s2m_used < PTY_BUF_SIZE)
            revents |= POLLOUT;
        if (pty->master_refs == 0)
            revents |= POLLHUP;
    }
    spin_unlock_irqrestore(&pty->lock, flags);
    return revents;
}

int pty_slave_write(int idx, const char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to write into slave-to-master ring
     * (applying OPOST/ONLCR output processing). */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (!g_ptys[idx].in_use) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EIO;
    }
    if (g_ptys[idx].master_refs == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EPIPE;
    }
    size_t n;
    if ((g_ptys[idx].termios.c_oflag & (PTY_OPOST | PTY_ONLCR)) ==
        (PTY_OPOST | PTY_ONLCR)) {
        n = 0;
        for (size_t i = 0; i < count; i++) {
            size_t w;
            if (buf[i] == '\n')
                w = ring_write(g_ptys[idx].s2m_buf, PTY_BUF_SIZE,
                               &g_ptys[idx].s2m_head, &g_ptys[idx].s2m_used,
                               "\r\n", 2);
            else
                w = ring_write(g_ptys[idx].s2m_buf, PTY_BUF_SIZE,
                               &g_ptys[idx].s2m_head, &g_ptys[idx].s2m_used,
                               buf + i, 1);
            if (w == 0)
                break;
            n++;
        }
    } else {
        n = ring_write(g_ptys[idx].s2m_buf, PTY_BUF_SIZE,
                       &g_ptys[idx].s2m_head, &g_ptys[idx].s2m_used,
                       buf, count);
    }
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    if (n > 0)
        wait_queue_wake_all(&g_ptys[idx].master_readers, 0, PROC_WAKE_EVENT);
    return (int)n;
}

int pty_master_ioctl(int idx, unsigned long req, void *arg) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return -EIO;
    if (req == TIOCGPTN) {
        int n = idx;
        if (copy_to_user(arg, &n, sizeof(n)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCSPTLCK) {
        int lock;
        if (copy_from_user(&lock, arg, sizeof(lock)) < 0) return -EFAULT;
        /* LOCK_ORDER: acquire per-pair lock to update locked flag. */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].locked = lock;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == TIOCGPTP) {
        int n = idx;
        if (copy_to_user(arg, &n, sizeof(n)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCGWINSZ) {
        uint16_t ws[4];
        /* LOCK_ORDER: acquire per-pair lock to read window size (master side). */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        ws[0] = g_ptys[idx].ws_row;
        ws[1] = g_ptys[idx].ws_col;
        ws[2] = 0;
        ws[3] = 0;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, ws, sizeof(ws)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCSWINSZ) {
        uint16_t ws[4];
        if (copy_from_user(ws, arg, sizeof(ws)) < 0) return -EFAULT;
        /* LOCK_ORDER: acquire per-pair lock to update window size (master side). */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].ws_row = ws[0];
        g_ptys[idx].ws_col = ws[1];
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == FIONBIO) {
        int nb;
        if (copy_from_user(&nb, arg, sizeof(nb)) < 0) return -EFAULT;
        /* LOCK_ORDER: acquire per-pair lock to update master nonblock flag. */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].master_nonblock = nb;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == TIOCPKT) {
        /* Packet mode: every master read is prefixed with a status byte
         * (TIOCPKT_DATA == 0 for plain data).  Required by VTE-based
         * terminals (xfce4-terminal, GNOME Terminal); without it they
         * fail pty setup with ENOTTY ("Failed to open PTY: Not a tty"). */
        int on;
        if (copy_from_user(&on, arg, sizeof(on)) < 0) return -EFAULT;
        /* LOCK_ORDER: acquire per-pair lock to update packet mode flag. */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].packet_mode = !!on;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == FIONREAD) {
        int avail;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        avail = (int)g_ptys[idx].s2m_used;   /* bytes the master can read */
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, &avail, sizeof(avail)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCINQ) {
        int avail;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        avail = (int)g_ptys[idx].s2m_used;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, &avail, sizeof(avail)) < 0) return -EFAULT;
        return 0;
    }
    return -ENOTTY;
}

int pty_slave_ioctl(int idx, unsigned long req, void *arg) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return -EIO;
    if (req == TCGETS) {
        pty_termios_t termios;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        termios = g_ptys[idx].termios;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, &termios, sizeof(termios)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
        pty_termios_t termios;
        if (copy_from_user(&termios, arg, sizeof(termios)) < 0) return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].termios = termios;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == TIOCGWINSZ) {
        uint16_t ws[4];
        /* LOCK_ORDER: acquire per-pair lock to read window size (slave side). */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        ws[0] = g_ptys[idx].ws_row;
        ws[1] = g_ptys[idx].ws_col;
        ws[2] = 0;
        ws[3] = 0;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, ws, sizeof(ws)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCSWINSZ) {
        uint16_t ws[4];
        if (copy_from_user(ws, arg, sizeof(ws)) < 0) return -EFAULT;
        /* LOCK_ORDER: acquire per-pair lock to update window size (slave side). */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].ws_row = ws[0];
        g_ptys[idx].ws_col = ws[1];
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == TIOCSCTTY) return 0;
    if (req == TIOCNOTTY) return 0;
    if (req == TIOCGPGRP) {
        int pgrp;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        pgrp = g_ptys[idx].pgrp ? g_ptys[idx].pgrp : 0;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, &pgrp, sizeof(pgrp)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCSPGRP) {
        int pgrp;
        if (copy_from_user(&pgrp, arg, sizeof(pgrp)) < 0) return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].pgrp = pgrp;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == TCFLSH) {
        /* Flush input (0), output (1), or both (2) ring buffers. */
        int mode = (int)(uintptr_t)arg;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        if (mode == 0 || mode == 2) {
            g_ptys[idx].m2s_used = 0;
            g_ptys[idx].m2s_head = g_ptys[idx].m2s_tail = 0;
            g_ptys[idx].canon_len = 0;
            g_ptys[idx].canon_lines = 0;
        }
        if (mode == 1 || mode == 2) {
            g_ptys[idx].s2m_used = 0;
            g_ptys[idx].s2m_head = g_ptys[idx].s2m_tail = 0;
        }
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    if (req == TIOCOUTQ) {
        int avail;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        avail = (int)g_ptys[idx].s2m_used;   /* written by slave, unread by master */
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, &avail, sizeof(avail)) < 0) return -EFAULT;
        return 0;
    }
    if (req == FIONREAD || req == TIOCINQ) {
        int avail;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        avail = (g_ptys[idx].termios.c_lflag & PTY_ICANON) ?
                (g_ptys[idx].canon_lines > 0 ? (int)g_ptys[idx].canon_len : 0) :
                (int)g_ptys[idx].m2s_used;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (copy_to_user(arg, &avail, sizeof(avail)) < 0) return -EFAULT;
        return 0;
    }
    if (req == TIOCSTI) {
        /* Inject a single byte into the terminal input (simulate typing). */
        char ch;
        if (copy_from_user(&ch, arg, sizeof(ch)) < 0) return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        pty_input_byte_locked(&g_ptys[idx], ch);
        int echoed = g_ptys[idx].s2m_used > 0;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        wait_queue_wake_all(&g_ptys[idx].slave_readers, 0, PROC_WAKE_EVENT);
        if (echoed)
            wait_queue_wake_all(&g_ptys[idx].master_readers, 0, PROC_WAKE_EVENT);
        return 0;
    }
    if (req == FIONBIO) {
        int nb;
        if (copy_from_user(&nb, arg, sizeof(nb)) < 0) return -EFAULT;
        /* LOCK_ORDER: acquire per-pair lock to update slave nonblock flag. */
        uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
        g_ptys[idx].slave_nonblock = nb;
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return 0;
    }
    return -ENOTTY;
}

int pty_alloc_and_open(void) {
    return pty_alloc();
}

void pty_master_close(int idx) {
    if (idx < 0 || idx >= MAX_PTYS) return;
    pty_pair_t *pty = &g_ptys[idx];
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return;
    }
    if (pty->master_refs > 0)
        pty->master_refs--;
    pty_maybe_free_locked(pty);
    spin_unlock_irqrestore(&pty->lock, flags);
    wait_queue_wake_all(&pty->slave_readers, 0, PROC_WAKE_EVENT);
}

void pty_slave_close(int idx) {
    if (idx < 0 || idx >= MAX_PTYS) return;
    pty_pair_t *pty = &g_ptys[idx];
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return;
    }
    if (pty->slave_refs > 0)
        pty->slave_refs--;
    pty_maybe_free_locked(pty);
    spin_unlock_irqrestore(&pty->lock, flags);
    wait_queue_wake_all(&pty->master_readers, 0, PROC_WAKE_EVENT);
}

int pty_slave_open(int idx) {
    if (idx < 0 || idx >= MAX_PTYS)
        return -EIO;

    pty_pair_t *pty = &g_ptys[idx];
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return -EIO;
    }
    if (pty->locked) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return -EACCES;
    }
    pty->slave_refs++;
    spin_unlock_irqrestore(&pty->lock, flags);
    return 0;
}

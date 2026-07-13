#include "core/defs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
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
    int         master_nonblock;
    int         slave_nonblock;
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
            if (!g_ptys[i].m2s_buf || !g_ptys[i].s2m_buf) {
                if (g_ptys[i].m2s_buf) kfree(g_ptys[i].m2s_buf);
                if (g_ptys[i].s2m_buf) kfree(g_ptys[i].s2m_buf);
                g_ptys[i].m2s_buf = NULL;
                g_ptys[i].s2m_buf = NULL;
                g_ptys[i].in_use = 0;
                spin_unlock_irqrestore(&g_pty_alloc_lock, flags);
                return -ENOMEM;
            }
            g_ptys[i].m2s_head = g_ptys[i].m2s_tail = g_ptys[i].m2s_used = 0;
            g_ptys[i].s2m_head = g_ptys[i].s2m_tail = g_ptys[i].s2m_used = 0;
            g_ptys[i].locked = 0;
            g_ptys[i].master_refs = 1;
            g_ptys[i].slave_refs = 0;
            g_ptys[i].ws_row = 24;
            g_ptys[i].ws_col = 80;
            g_ptys[i].master_nonblock = 0;
            g_ptys[i].slave_nonblock = 0;
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

    wait_queue_entry_t entry = {0};
    entry.task = task;

    uint64_t wait_flags = spin_lock_irqsave(&wq->lock);
    entry.next = wq->head;
    entry.prev = NULL;
    if (wq->head)
        wq->head->prev = &entry;
    wq->head = &entry;
    (*waiting)++;
    task->state = PROC_BLOCKED;
    spin_unlock_irqrestore(&wq->lock, wait_flags);

    spin_unlock_irqrestore(&pty->lock, *pty_flags);
    sched();
    wait_queue_finish(wq, &entry);
    *pty_flags = spin_lock_irqsave(&pty->lock);
    (*waiting)--;

    if (signal_task_has_unblocked(task))
        return -ERESTARTSYS;
    return 0;
}

int pty_master_read(int idx, char *buf, size_t count) {
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
        if (pty->master_nonblock) {
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
    size_t n = ring_read(pty->s2m_buf, PTY_BUF_SIZE,
                         &pty->s2m_tail, &pty->s2m_used,
                         buf, count);
    spin_unlock_irqrestore(&pty->lock, flags);
    return (int)n;
}

int pty_master_write(int idx, const char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to write into master-to-slave ring. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (!g_ptys[idx].in_use) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EIO;
    }
    if (g_ptys[idx].slave_refs == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EPIPE;
    }
    size_t n = ring_write(g_ptys[idx].m2s_buf, PTY_BUF_SIZE,
                          &g_ptys[idx].m2s_head, &g_ptys[idx].m2s_used,
                          buf, count);
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    if (n > 0)
        wait_queue_wake_all(&g_ptys[idx].slave_readers);
    return (int)n;
}

int pty_slave_read(int idx, char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    if (count == 0) return 0;

    pty_pair_t *pty = &g_ptys[idx];
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    if (!pty->in_use) {
        spin_unlock_irqrestore(&pty->lock, flags);
        return -EIO;
    }
    while (pty->m2s_used == 0) {
        if (pty->master_refs == 0) {
            spin_unlock_irqrestore(&pty->lock, flags);
            return 0;
        }
        if (pty->slave_nonblock) {
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
    size_t n = ring_read(pty->m2s_buf, PTY_BUF_SIZE,
                         &pty->m2s_tail, &pty->m2s_used,
                         buf, count);
    spin_unlock_irqrestore(&pty->lock, flags);
    return (int)n;
}

int pty_slave_write(int idx, const char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to write into slave-to-master ring. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (!g_ptys[idx].in_use) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EIO;
    }
    if (g_ptys[idx].master_refs == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EPIPE;
    }
    size_t n = ring_write(g_ptys[idx].s2m_buf, PTY_BUF_SIZE,
                          &g_ptys[idx].s2m_head, &g_ptys[idx].s2m_used,
                          buf, count);
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    if (n > 0)
        wait_queue_wake_all(&g_ptys[idx].master_readers);
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
    wait_queue_wake_all(&pty->slave_readers);
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
    wait_queue_wake_all(&pty->master_readers);
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

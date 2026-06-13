#include "core/defs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
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
} pty_pair_t;

static pty_pair_t g_ptys[MAX_PTYS];
/* LOCK_ORDER: g_pty_alloc_lock protects in_use during allocation and initial
 * buffer setup only. Never nested under or over the per-pair lock. */
static spinlock_t g_pty_alloc_lock;

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
            spin_unlock_irqrestore(&g_pty_alloc_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&g_pty_alloc_lock, flags);
    return -ENOSPC;
}

static void pty_release(int idx) {
    if (idx < 0 || idx >= MAX_PTYS) return;
    /* LOCK_ORDER: acquire per-pair lock to update reference counts and
     * optionally clear in_use; backing vfile reference is dropped after release. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (g_ptys[idx].master_refs > 0) g_ptys[idx].master_refs--;
    if (g_ptys[idx].slave_refs > 0) g_ptys[idx].slave_refs--;
    if (g_ptys[idx].master_refs == 0 && g_ptys[idx].slave_refs == 0) {
        if (g_ptys[idx].m2s_buf) { kfree(g_ptys[idx].m2s_buf); g_ptys[idx].m2s_buf = NULL; }
        if (g_ptys[idx].s2m_buf) { kfree(g_ptys[idx].s2m_buf); g_ptys[idx].s2m_buf = NULL; }
        g_ptys[idx].in_use = 0;
    }
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
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

int pty_master_read(int idx, char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to read from slave-to-master ring. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (g_ptys[idx].s2m_used == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (g_ptys[idx].master_nonblock) return -EAGAIN;
        return 0;
    }
    size_t n = ring_read(g_ptys[idx].s2m_buf, PTY_BUF_SIZE,
                         &g_ptys[idx].s2m_tail, &g_ptys[idx].s2m_used,
                         buf, count);
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    return (int)n;
}

int pty_master_write(int idx, const char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to write into master-to-slave ring. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (g_ptys[idx].slave_refs == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EPIPE;
    }
    size_t n = ring_write(g_ptys[idx].m2s_buf, PTY_BUF_SIZE,
                          &g_ptys[idx].m2s_head, &g_ptys[idx].m2s_used,
                          buf, count);
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    return (int)n;
}

int pty_slave_read(int idx, char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to read from master-to-slave ring. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (g_ptys[idx].m2s_used == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        if (g_ptys[idx].slave_nonblock) return -EAGAIN;
        return 0;
    }
    size_t n = ring_read(g_ptys[idx].m2s_buf, PTY_BUF_SIZE,
                         &g_ptys[idx].m2s_tail, &g_ptys[idx].m2s_used,
                         buf, count);
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
    return (int)n;
}

int pty_slave_write(int idx, const char *buf, size_t count) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return -EIO;
    /* LOCK_ORDER: acquire per-pair lock to write into slave-to-master ring. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    if (g_ptys[idx].master_refs == 0) {
        spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
        return -EPIPE;
    }
    size_t n = ring_write(g_ptys[idx].s2m_buf, PTY_BUF_SIZE,
                          &g_ptys[idx].s2m_head, &g_ptys[idx].s2m_used,
                          buf, count);
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
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
    if (req == TCGETS || req == TCSETS || req == TCSETSW || req == TCSETSF) return 0;
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
    pty_release(idx);
}

void pty_slave_close(int idx) {
    if (idx < 0 || idx >= MAX_PTYS) return;
    pty_release(idx);
}

void pty_slave_ref(int idx) {
    if (idx < 0 || idx >= MAX_PTYS || !g_ptys[idx].in_use) return;
    /* LOCK_ORDER: acquire per-pair lock to increment slave reference count. */
    uint64_t flags = spin_lock_irqsave(&g_ptys[idx].lock);
    g_ptys[idx].slave_refs++;
    spin_unlock_irqrestore(&g_ptys[idx].lock, flags);
}

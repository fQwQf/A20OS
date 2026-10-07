#include "drivers/char/uart.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "platform.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"

#ifndef UART_POLL_INTERVAL_TICKS
#define UART_POLL_INTERVAL_TICKS (TICKS_PER_SEC / 20)
#endif

/*
 * RX buffer size.  256 was enough for a paced console but not for a burst:
 * tools/serial_fidelity.py --blast 8 writes 8 command lines (~512 bytes) per
 * host write, and with a 256-byte ring plus a 16-byte 16550 FIFO the reader --
 * which is bound to ~11.5 kB/s by having to echo every byte back out the same
 * 115200-baud UART -- could not keep up, so uart_rx_push() silently discarded
 * 15130 of the 18909 bytes it was handed in one measured run (see
 * docs/measured/serial-fidelity.md).  A silent drop turns into a merged
 * command line on the console: the shell keeps the partial line, the next
 * command's bytes land in it, and mksh executes garbage
 * (`echecho FID000009...: inaccessible or not found`).
 *
 * 8192 covers the largest burst the harness produces with margin, and the
 * ring is still bounded: an unbounded host can still overrun it, but then
 * rx_dropped says so instead of the run quietly lying.
 */
#define RX_BUF_SIZE 8192

// receive buffer (ring buffer)
static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;
/* LOCK_ORDER: rx_lock protects the RX ring and tty_foreground_pgid only.  The
 * Ctrl-C path takes it just long enough to collect a wake; signalling runs
 * outside it, so no other lock is ever acquired while rx_lock is held. */
static spinlock_t rx_lock = SPINLOCK_INIT;
static wait_queue_t rx_waiters;
static int tty_foreground_pgid;

/*
 * RX_FIDELITY: runtime evidence for the guest console dropping input
 * characters.  The counters are diagnostics with a defined meaning, not
 * free-running statistics:
 *
 *   rx_dropped         characters discarded because the ring buffer was full.
 *                      The drop is silent by construction (see
 *                      uart_rx_push_locked), so without this counter a guest
 *                      that overruns its console ring gives no sign of it at
 *                      all.
 *   rx_polls_rx_bytes  bytes the task-context poll path took out of the 16550
 *                      receive register.  This is the coverage counter: it
 *                      must be non-zero in a fidelity run for the run to have
 *                      exercised the path at all (see uart_getc's poll
 *                      section), and it was 0 in every paced run before the
 *                      poll path was folded into the ring critical section.
 *   rx_irq_bytes       bytes the RX top half took out of the receive register.
 *
 * The ordering claim -- a byte read from the device cannot be pushed into the
 * ring after a byte that arrived behind it -- is structural rather than
 * counted: every read of the receive register in this file happens with
 * rx_lock held and every push happens before that lock is released, so the
 * ring order *is* the receive-register order.  What can still go wrong is a
 * drop (ring full), and that is what rx_dropped + the probe line make visible.
 */
static volatile uint32_t rx_dropped;
/* See the RX_FIDELITY note above: the count of bytes consumed by task-context
 * polls of the 16550 receive register. */
static volatile uint32_t rx_polls_rx_bytes;
static volatile uint32_t rx_irq_bytes;

static uint32_t rx_fidelity_last_irq;
static unsigned rx_fidelity_reported;
/* Bytes between periodic probe lines; see uart_rx_fidelity_report(). */
#define RX_FIDELITY_STRIDE 4096
/* Set by the Ctrl-C top half, consumed by the console reader in task context. */
static int g_ctrlc_pending;
static int g_ctrlc_signalled;

static int uart_task_should_spare(task_t *t)
{
    if (!t || t->pid <= 1)
        return 1;
    return strcmp(t->name, "sh") == 0 || strcmp(t->name, "busybox") == 0 ||
           strstr(t->exec_path, "/busybox") != NULL;
}

static int uart_signal_user_pgid(int pgid, int signum)
{
    int count = 0;
    if (pgid <= 0)
        return 0;
    int max_pid = proc_pid_max();
    for (int pid = 1; pid <= max_pid; pid++) {
        task_t *t = proc_find_get(pid);
        if (!t)
            continue;
        if (t->pid > 1 && t->pgdir && t->pgid == pgid) {
            proc_kill(t->pid, signum);
            count++;
        }
        proc_put(t);
    }
    return count;
}

static int uart_signal_all_user(int signum, int spare_shells)
{
    int count = 0;
    int max_pid = proc_pid_max();
    for (int pid = 1; pid <= max_pid; pid++) {
        task_t *t = proc_find_get(pid);
        if (t && t->pid > 1 && t->pgdir &&
            (!spare_shells || !uart_task_should_spare(t))) {
            proc_kill(t->pid, signum);
            count++;
        }
        proc_put(t);
    }
    return count;
}

/*
 * Snapshot-then-print, deliberately (design E3).  Holding a global lock across
 * one kdebug line per task is what makes this dump unsafe from the top half:
 * see the CTRL_C_CONTEXT_SPLIT note below.  So the lock is taken only to copy
 * the fields out, and every kdebug happens after the release.
 *
 * The snapshot is a file-scope array rather than a stack array because this
 * runs on the uart reader task, whose kernel stack is 512-2048 bytes on the
 * MCU profiles and the entries are far larger than that.  Two CPUs can
 * interleave into it if they dump at the same time; that is acceptable for a
 * hang diagnostic and is not worth a second global lock.
 */
enum { UART_DUMP_SNAP_MAX = 32 };
struct uart_dump_snap {
    int pid, ppid, pgid, sid;
    int state;
    unsigned long wake_time;
    int task_on_rq;
    char name[16];
};
static struct uart_dump_snap g_uart_dump_snap[UART_DUMP_SNAP_MAX];

static void uart_dump_tasks(void)
{
    /* LOCK_ORDER: tasklist_lock acquired while not holding rx_lock (task dump
     * helper); it is released before any kdebug. */
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        uint64_t tf = spin_lock_irqsave(&t->park_lock);
        int state = t->state;
        if (state == PROC_UNUSED) {
            spin_unlock_irqrestore(&t->park_lock, tf);
            continue;
        }
        if (n < UART_DUMP_SNAP_MAX) {
            /* Fill the array element in place rather than through a
             * 'struct uart_dump_snap *e'.  Same stores, but the left-hand side
             * is then a '.' member of the snapshot array instead of '->': the
             * gate check-task-state-boundary forbids '->(on_rq|...) =' outside
             * the park-lock owning files because that shape is what a lockless
             * task-field write looks like, and a copy out of a task into a
             * non-task struct must not be able to be read as one.  Keeping the
             * array as the destination also keeps the zero-stack discipline
             * this dump was written for (see the file-scope note above). */
            g_uart_dump_snap[n].pid = t->pid;
            g_uart_dump_snap[n].ppid = t->ppid;
            g_uart_dump_snap[n].pgid = t->pgid;
            g_uart_dump_snap[n].sid = t->sid;
            g_uart_dump_snap[n].state = state;
            g_uart_dump_snap[n].wake_time = (unsigned long)t->wake_time;
            g_uart_dump_snap[n].task_on_rq = __atomic_load_n(
                &t->on_rq, __ATOMIC_RELAXED);
            strncpy(g_uart_dump_snap[n].name, t->name,
                    sizeof(g_uart_dump_snap[n].name) - 1);
            g_uart_dump_snap[n].name[sizeof(g_uart_dump_snap[n].name) - 1] =
                '\0';
            n++;
        }
        spin_unlock_irqrestore(&t->park_lock, tf);
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);

    kdebug("[TTYDBG] task dump begin\n");
    for (int i = 0; i < n; i++) {
        const struct uart_dump_snap *e = &g_uart_dump_snap[i];
        const char *state = "?";
        switch (e->state) {
        case PROC_READY:   state = "READY"; break;
        case PROC_RUNNING: state = "RUN"; break;
        case PROC_BLOCKED: state = "BLOCK"; break;
        case PROC_ZOMBIE:  state = "ZOMB"; break;
        case PROC_STOPPED: state = "STOP"; break;
        default: break;
        }
        kdebug("[TTYDBG] pid=%d ppid=%d pgid=%d sid=%d state=%s wake=%lu onrq=%d name=%s\n",
               e->pid, e->ppid, e->pgid, e->sid, state,
               e->wake_time, e->task_on_rq, e->name);
    }
    if (n == UART_DUMP_SNAP_MAX)
        kdebug("[TTYDBG] task dump truncated at %d entries\n",
               UART_DUMP_SNAP_MAX);
    kdebug("[TTYDBG] task dump end\n");
}

/*
 * CTRL_C_CONTEXT_SPLIT: the two signal walks stay in the top half, because the
 * foreground process group has to learn about the key while the shell is busy
 * running a command and therefore nowhere near a console read -- deferring them
 * would stop Ctrl-C from killing anything.  proc_find_get() takes only pid_lock
 * and signal delivery from an interrupt is what that path is for.
 *
 * The task-table dump is the part that cannot happen here.  uart_dump_tasks()
 * holds tasklist_lock with interrupts disabled while it copies the task table
 * out, then writes one kdebug line per entry out of the same UART whose
 * interrupt is running, so a dump started from the top half re-enters the
 * console under its own IRQ.  The snapshot-then-print shape is what keeps the
 * console writes off the lock: no kdebug happens while tasklist_lock is held.  The top half only raises the
 * flag and wakes the reader; uart_getc() is this driver's only task-context
 * entry point and does the dump there.
 */
static void uart_signal_ctrlc(void)
{
    __atomic_store_n(&g_ctrlc_pending, 1, __ATOMIC_RELEASE);

    /* Waking before signalling, not after: where no foreground process group
     * has been established the walks below SIGINT every user task, init
     * included, so a reader woken only afterwards has already been killed and
     * the deferred dump would never run.  Waking first gives the reader a
     * chance to observe the flag while it is still alive to do so. */
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    (void)wait_queue_collect_one(&rx_waiters, 0, PROC_WAKE_EVENT, &wake_q);
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);

    int pgid = uart_get_foreground_pgid();
    int hit = uart_signal_user_pgid(pgid, SIGINT);
    int rest = uart_signal_all_user(SIGINT, hit > 0);
    __atomic_store_n(&g_ctrlc_signalled, hit + rest, __ATOMIC_RELAXED);
}

static void uart_service_ctrlc(void)
{
    if (!__atomic_exchange_n(&g_ctrlc_pending, 0, __ATOMIC_ACQ_REL))
        return;
    int signalled = __atomic_exchange_n(&g_ctrlc_signalled, 0, __ATOMIC_RELAXED);
    uart_dump_tasks();
    kdebug("[TTYDBG] Ctrl-C signalled %d task(s)\n", signalled);
}

/*
 * RX_LOCK_INVARIANT: every byte reaches the ring through a path that holds
 * rx_lock over *both* the receive-register read and the ring store, so ring
 * order is receive-register order and cannot be reordered by a top half
 * draining the FIFO between the two.
 *
 * uart_rx_push_locked() is the half that must run under the lock: it appends
 * and collects the wake, it never releases anything.  The wrappers around it
 * drop the lock first and only then flush the wake list or run the Ctrl-C
 * side effects -- LOCK_ORDER above forbids taking another lock while rx_lock
 * is held, and uart_signal_ctrlc() takes rx_lock itself, so a control byte is
 * handed back to the caller as 0 instead of being processed in place.
 *
 * Returns 1 for "the byte is consumed (enqueued or dropped)", 0 for "this was
 * a control byte, process it after you drop the lock".
 */
static int uart_rx_push_locked(char c, proc_wake_q_t *wake_q) {
    if (c == 0x03) {  // Ctrl-C
        return 0;
    }
    uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buffer[rx_head] = c;
        rx_head = next;
        (void)wait_queue_collect_one(&rx_waiters, 0, PROC_WAKE_EVENT, wake_q);
    } else {
        rx_dropped++;
    }
    return 1;
}

/* One byte, in and out of the lock: the shape used by paths that are handed a
 * byte by someone else (uart_receive_char) rather than reading the device
 * themselves. */
static void uart_rx_push(char c) {
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int consumed = uart_rx_push_locked(c, &wake_q);
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
    if (!consumed)
        uart_signal_ctrlc();
}

/* RX_FIDELITY probe: publish the counters into the console log as the run
 * progresses.  Called from task context (uart_getc) so the print cannot
 * re-enter the console from the top half -- see CTRL_C_CONTEXT_SPLIT.
 *
 * The cadence is "every 4096 received bytes, plus immediately on the first
 * sign of a defect".  Printing per character would interleave with the very
 * echo lines this gate compares; printing only near the start would report
 * the counters as they were during early boot, which says nothing about the
 * rest of the run.  kinfo rather than kdebug because KLOG_DEBUG sits below
 * the default klog_level and would never be emitted at all. */
static void uart_rx_fidelity_report(void) {
    uint32_t dropped = __atomic_load_n(&rx_dropped, __ATOMIC_RELAXED);
    uint32_t polled = __atomic_load_n(&rx_polls_rx_bytes, __ATOMIC_RELAXED);
    uint32_t irq = __atomic_load_n(&rx_irq_bytes, __ATOMIC_RELAXED);
    bool defect = dropped != 0;
    if (!defect && (irq - rx_fidelity_last_irq < RX_FIDELITY_STRIDE ||
                    rx_fidelity_reported >= 64))
        return;
    rx_fidelity_last_irq = irq;
    rx_fidelity_reported++;
    kinfo("[UART] rx_fidelity dropped=%u poll_bytes=%u irq_bytes=%u\n",
          dropped, polled, irq);
}

void uart_receive_char(char c) {
    uart_rx_push(c);
}

static int uart_irq_wrapper(int irq, void *priv);

void uart_init(void) {
    rx_head = 0;
    rx_tail = 0;
    tty_foreground_pgid = 0;
    spin_init(&rx_lock);
    wait_queue_init(&rx_waiters);
    arch_uart_init();
    uart_flush();
    request_irq(UART0_IRQ, uart_irq_wrapper, 0, NULL);
}

void uart_putc(char c) {
    arch_uart_putc(c);
}

// blocking read of one character (yields the CPU if there is no data)
int uart_getc(void) {
    for (;;) {
        /* The Ctrl-C dump the top half could not do; see CTRL_C_CONTEXT_SPLIT. */
        uart_service_ctrlc();
        /* RX_FIDELITY: emit the console-integrity counters from task context. */
        uart_rx_fidelity_report();
        uint64_t flags = spin_lock_irqsave(&rx_lock);
        if (rx_head != rx_tail) {
            char c = rx_buffer[rx_tail];
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
            spin_unlock_irqrestore(&rx_lock, flags);
            return (int)(unsigned char)c;
        }
        /*
         * RX_FIDELITY: the task-context read of the receive register happens
         * with rx_lock held and the byte is pushed before the lock drops.
         * Taking the byte with the lock released opened the window named in
         * RX_LOCK_INVARIANT: the top half could drain the rest of the FIFO
         * in between and append the later bytes first, so the reader saw
         * adjacent characters in the wrong order.
         */
        int c = arch_uart_poll_getc();
        if (c >= 0) {
            proc_wake_q_t wake_q;
            proc_wake_q_init(&wake_q);
            rx_polls_rx_bytes++;
            int consumed = uart_rx_push_locked((char)c, &wake_q);
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (!consumed)
                uart_signal_ctrlc();
            continue;
        }
        spin_unlock_irqrestore(&rx_lock, flags);

        if (current_board && current_board->uart_rx_is_polled) {
            /* This board's UART IRQ route and timer trap are not available.
             * Keep the foreground shell runnable and poll the 16550 directly. */
            cpu_relax();
            continue;
        }

        task_t *cur = proc_current();
        if (!cur) {
            arch_local_irq_enable();
            cpu_relax();
            arch_local_irq_disable();
            continue;
        }

        flags = spin_lock_irqsave(&rx_lock);
        c = arch_uart_poll_getc();
        if (c >= 0) {
            proc_wake_q_t wake_q;
            proc_wake_q_init(&wake_q);
            rx_polls_rx_bytes++;
            int consumed = uart_rx_push_locked((char)c, &wake_q);
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (!consumed)
                uart_signal_ctrlc();
            continue;
        }
        uint64_t deadline = timer_get_ticks() + UART_POLL_INTERVAL_TICKS;
        spin_unlock_irqrestore(&rx_lock, flags);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            continue;

        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&rx_lock);
        c = arch_uart_poll_getc();
        if (c >= 0) {
            proc_wake_q_t wake_q;
            proc_wake_q_init(&wake_q);
            rx_polls_rx_bytes++;
            int consumed = uart_rx_push_locked((char)c, &wake_q);
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (!consumed)
                uart_signal_ctrlc();
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        bool linked = wait_queue_link(&rx_waiters, &entry, token, 0);
        spin_unlock_irqrestore(&rx_lock, flags);

        arch_local_irq_enable();
        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        arch_local_irq_disable();
        wait_queue_unlink(&rx_waiters, &entry);
        proc_park_finish(token);
    }
}

// non-blocking attempt to read one character
int uart_try_getc(void) {
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    if (rx_head == rx_tail) {
        spin_unlock_irqrestore(&rx_lock, flags);
        if (current_board && current_board->uart_rx_is_polled)
            return arch_uart_poll_getc();
        return -1;
    }
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    spin_unlock_irqrestore(&rx_lock, flags);
    return (int)(unsigned char)c;
}

int uart_has_input(void) {
    /* Poll() runs in task context too, so it is the second chance to service a
     * Ctrl-C the reader was not parked for -- a foreground process that never
     * reads stdin still polls. */
    uart_service_ctrlc();
    int ctrlc = 0;
    if (current_board && current_board->uart_rx_is_polled) {
        /* Polled boards have no top half, but the read and the store still go
         * in one rx_lock section so a concurrent reader cannot push a later
         * byte first (RX_LOCK_INVARIANT). */
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t pflags = spin_lock_irqsave(&rx_lock);
        int polled = arch_uart_poll_getc();
        if (polled >= 0)
            ctrlc = !uart_rx_push_locked((char)polled, &wake_q);
        spin_unlock_irqrestore(&rx_lock, pflags);
        (void)proc_wake_q_flush(&wake_q);
    }
    if (ctrlc)
        uart_signal_ctrlc();
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int has = rx_head != rx_tail;
    spin_unlock_irqrestore(&rx_lock, flags);
    return has;
}

wait_queue_t *uart_read_wait_queue(void) {
    return &rx_waiters;
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

void uart_flush(void) {
    arch_uart_flush();
}

int uart_get_foreground_pgid(void) {
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int pgid = tty_foreground_pgid;
    spin_unlock_irqrestore(&rx_lock, flags);
    return pgid;
}

void uart_set_foreground_pgid(int pgid) {
    if (pgid <= 0)
        return;
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    tty_foreground_pgid = pgid;
    spin_unlock_irqrestore(&rx_lock, flags);
}

void uart_handle_irq(void) {
    /* LOCK_ORDER: IRQ handler pushes characters under rx_lock; no other locks
     * are acquired while it is held.  The receive register is drained *inside*
     * that same section (RX_LOCK_INVARIANT): reading a byte here and storing
     * it after dropping the lock would let a task-context poll of the same
     * register slot in between and publish its byte first.  The wake flush and
     * the Ctrl-C side effects run after the release, where taking further
     * locks is legal. */
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    int ctrlc = 0;
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int c;
    while ((c = arch_uart_poll_getc()) >= 0) {
        rx_irq_bytes++;
        if (!uart_rx_push_locked((char)c, &wake_q))
            ctrlc = 1;
    }
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
    arch_uart_ack_irq();
    if (ctrlc)
        uart_signal_ctrlc();
}

static int uart_irq_wrapper(int irq, void *priv) {
    (void)irq;
    (void)priv;
    uart_handle_irq();
    return 0;
}

static int uart_char_read(struct device *dev, void *buf, size_t count) {
    (void)dev;
    char *p = buf;
    size_t n = 0;
    while (n < count) {
        int c = uart_getc();
        if (c < 0)
            break;
        p[n++] = (char)c;
    }
    return (int)n;
}

static int uart_char_write(struct device *dev, const void *buf, size_t count) {
    (void)dev;
    const char *p = buf;
    for (size_t i = 0; i < count; i++)
        uart_putc(p[i]);
    return (int)count;
}

static char_dev_ops_t uart_char_ops = {
    .read  = uart_char_read,
    .write = uart_char_write,
};

static int uart_driver_probe(device_t *dev) {
    (void)dev;
    kinfo("[UART] UART driver probed (early-init already done)\n");
    return 0;
}

/* The UART is a board console service initialized by uart_init(); the
 * driver_t only publishes a devfs char device when a board actually
 * registers an UART device.  The match callback makes busless binding
 * deterministic instead of grabbing arbitrary board devices. */
static int uart_driver_match(device_t *dev) {
    if (!dev || !dev->name)
        return 0;
    return strncmp(dev->name, "uart", 4) == 0 ||
           strncmp(dev->name, "serial", 6) == 0;
}

static int uart_driver_remove(device_t *dev) {
    (void)dev;
    return 0;
}

static driver_t uart_driver = {
    .name       = "ns16550a-uart",
    .id_table   = NULL,
    .bus        = NULL,
    .match      = uart_driver_match,
    .probe      = uart_driver_probe,
    .remove     = uart_driver_remove,
    .class_ops  = &uart_char_ops,
    .class_type = DEV_CLASS_CHAR,
};

DRIVER_REGISTER(uart_driver);

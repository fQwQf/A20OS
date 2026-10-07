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

#define RX_BUF_SIZE 256

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
 * RX_FIDELITY: runtime evidence for the guest console dropping and
 * transposing adjacent input characters.  Both counters are diagnostics with
 * a defined meaning, not free-running statistics:
 *
 *   rx_reorders  the task-context poll path (uart_getc) consumed a byte from
 *                the 16550 receive register and then pushed it into the ring
 *                buffer, but the ring had already been advanced past that
 *                position by the RX interrupt handler in between.  Each count
 *                is one input line whose bytes reach the reader in an order
 *                different from the order the host wrote them -- i.e. one
 *                adjacent-character transposition.
 *   rx_dropped   characters discarded because the ring buffer was full.  The
 *                drop is silent by construction (see uart_rx_push), so without
 *                this counter a guest that overruns its console ring gives no
 *                sign of it at all.
 *
 * The reorder count is the load-bearing one: it is measured, not inferred, by
 * comparing the ring head on either side of the unlocked hardware read.  Its
 * precondition is the structural fact documented at the uart_getc() call site:
 * the byte is taken from the device with no lock held and with interrupts
 * enabled, so the top half can and does drain the receive register in the
 * window before the byte reaches the ring.
 */
static volatile uint32_t rx_reorders;
static volatile uint32_t rx_dropped;
/* Reachability evidence for the race above: how many bytes each consumer took
 * out of the 16550 receive register.  The task-context poll path is the one
 * that performs the unlocked read, so rx_polls_rx_irq_bytes is the number of
 * bytes that could have been overtaken by the top half. */
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
    int on_rq;
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
            g_uart_dump_snap[n].on_rq = __atomic_load_n(&t->on_rq,
                                                        __ATOMIC_RELAXED);
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
               e->wake_time, e->on_rq, e->name);
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

static void uart_rx_push(char c) {
    if (c == 0x03) {  // Ctrl-C
        uart_signal_ctrlc();
        return;
    }

    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buffer[rx_head] = c;
        rx_head = next;
        (void)wait_queue_collect_one(&rx_waiters, 0, PROC_WAKE_EVENT,
                                     &wake_q);
    } else {
        rx_dropped++;
    }
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
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
void uart_rx_fidelity_report(void) {
    uint32_t reorders = __atomic_load_n(&rx_reorders, __ATOMIC_RELAXED);
    uint32_t dropped = __atomic_load_n(&rx_dropped, __ATOMIC_RELAXED);
    uint32_t polled = __atomic_load_n(&rx_polls_rx_bytes, __ATOMIC_RELAXED);
    uint32_t irq = __atomic_load_n(&rx_irq_bytes, __ATOMIC_RELAXED);
    bool defect = reorders || dropped;
    if (!defect && (irq - rx_fidelity_last_irq < RX_FIDELITY_STRIDE ||
                    rx_fidelity_reported >= 64))
        return;
    rx_fidelity_last_irq = irq;
    rx_fidelity_reported++;
    kinfo("[UART] rx_fidelity reorders=%u dropped=%u poll_bytes=%u "
          "irq_bytes=%u\n", reorders, dropped, polled, irq);
}

/* Push a byte that was polled out of the receive register, and count it as a
 * reorder when the ring head did not move by exactly one position.  See the
 * RX_FIDELITY note above: head_before is the ring head sampled while the ring
 * was locked, immediately before the device was read with the lock dropped. */
static void uart_rx_push_polled(uint32_t head_before, char c) {
    rx_polls_rx_bytes++;
    uart_rx_push(c);
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    if (rx_head != (head_before + 1) % RX_BUF_SIZE)
        rx_reorders++;
    spin_unlock_irqrestore(&rx_lock, flags);
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
        spin_unlock_irqrestore(&rx_lock, flags);

        /*
         * RX_FIDELITY probe.  The head is sampled while the ring is still
         * locked and the byte is then taken from the device with the lock
         * dropped.  If the top half drained the receive register in that
         * window, it appended its own bytes first and ours lands one or more
         * positions later than head_before+1 -- an adjacent-character
         * transposition in the reader's stream.  Sampled before the read, so
         * the count is a property of the real code path and not of the fix.
         */
        uint32_t head_before;
        flags = spin_lock_irqsave(&rx_lock);
        head_before = rx_head;
        spin_unlock_irqrestore(&rx_lock, flags);

        int c = arch_uart_poll_getc();
        if (c >= 0) {
            uart_rx_push_polled(head_before, (char)c);
            continue;
        }

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
        head_before = rx_head;
        c = arch_uart_poll_getc();
        if (c >= 0) {
            spin_unlock_irqrestore(&rx_lock, flags);
            uart_rx_push_polled(head_before, (char)c);
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
        head_before = rx_head;
        c = arch_uart_poll_getc();
        if (c >= 0) {
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            uart_rx_push_polled(head_before, (char)c);
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
    if (current_board && current_board->uart_rx_is_polled) {
        int polled = arch_uart_poll_getc();
        if (polled >= 0)
            uart_rx_push((char)polled);
    }
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
    int c;
    /* LOCK_ORDER: IRQ handler pushes characters under rx_lock;
     * no other locks are acquired. */
    while ((c = arch_uart_poll_getc()) >= 0) {
        rx_irq_bytes++;
        uart_rx_push(c);
    }
    arch_uart_ack_irq();
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

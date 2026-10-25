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
#include "proc/signal.h"
/* The console line discipline: the driver asks it what a received byte means
 * (ISIG) rather than deciding for itself. */
#include "fs/tty.h"

#ifndef UART_POLL_INTERVAL_TICKS
#define UART_POLL_INTERVAL_TICKS (TICKS_PER_SEC / 20)
#endif

/*
 * RX buffer size.  256 was enough for a paced console but not for a burst:
 * tools/serial_fidelity.py --blast 8 writes 8 command lines (~512 bytes) per
 * host write, and with a 256-byte ring plus a 16-byte 16550 FIFO the reader --
 * which is bound to ~11.5 kB/s by having to echo every byte back out the same
 * 115200-baud UART -- could not keep up, so uart_rx_push() silently discarded
 * 15130 of the 18909 bytes it was handed in one measured run.  A silent drop
 * turns into a merged
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
/*
 * This driver deliberately does not look at control bytes.  Ctrl-C used to be
 * swallowed here (0x03 never reached the RX ring) and turned into SIGINT from
 * the RX path, with a task dump as a side effect.  That bypass kept the byte
 * from ever reaching the tty line discipline, so ISIG could not be implemented
 * where termios says it belongs, `stty -isig` had nothing to turn off, and a
 * guest of the hypervisor could not be interrupted at all (see the note in
 * kernel/hyp/hyp_dev.c).  Control characters are now ordinary data all the way
 * to the discipline, which raises the signals: kernel/fs/devfs/tty.c for the
 * console and kernel/drivers/char/pty.c for pty pairs.
 */

/*
 * RX_LOCK_INVARIANT: every byte reaches the ring through a path that holds
 * rx_lock over *both* the receive-register read and the ring store, so ring
 * order is receive-register order and cannot be reordered by a top half
 * draining the FIFO between the two.
 *
 * uart_rx_push_locked() is the half that must run under the lock: it appends
 * and collects the wake, it never releases anything.  The wrappers around it
 * drop the lock first and only then flush the wake list.
 *
 * Returns the signal the console line discipline wants raised for this byte, or
 * 0 when the byte was queued as data.  The caller must raise it after dropping
 * rx_lock: tty_console_isig_raise() takes rx_lock itself (through
 * uart_get_foreground_pgid), and LOCK_ORDER forbids taking another lock while
 * rx_lock is held.  Control bytes are consumed here rather than queued, which
 * is what a line discipline does with them.
 */
static int uart_rx_push_locked(char c, proc_wake_q_t *wake_q) {
    int sig = tty_console_isig_signal(c);
    if (sig)
        return sig;
    uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buffer[rx_head] = c;
        rx_head = next;
        (void)wait_queue_collect_one(&rx_waiters, 0, PROC_WAKE_EVENT, wake_q);
    } else {
        rx_dropped++;
    }
    return 0;
}

/* One byte, in and out of the lock: the shape used by paths that are handed a
 * byte by someone else (uart_receive_char) rather than reading the device
 * themselves. */
static void uart_rx_push(char c) {
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int sig = uart_rx_push_locked(c, &wake_q);
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
    if (sig)
        tty_console_isig_raise(sig);
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

void uart_write(const char *data, size_t length) {
#ifdef ARCH_UART_HAS_WRITE
    if (length) arch_uart_write(data, length);
#else
    for (size_t i = 0; i < length; i++)
        arch_uart_putc(data[i]);
#endif
}

// blocking read of one character (yields the CPU if there is no data)
int uart_getc(void) {
    for (;;) {
        /* Standard blocking-read semantics: a pending unblocked signal ends the
         * wait instead of parking forever.  Without this a reader blocked on
         * the console ignored SIGTERM/SIGINT outright -- measured before the
         * fix: `timeout 1 dd if=/dev/tty` took 11.98s to die and only did so
         * when the signal happened to be noticed at a later wakeup.  That is
         * also what made any program waiting on console input unkillable, and
         * what the ISIG path in the tty layer needs in order to end a read. */
        task_t *reader = proc_current();
        if (reader && signal_task_has_unblocked(reader))
            return -ERESTARTSYS;
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
            int sig = uart_rx_push_locked((char)c, &wake_q);
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (sig)
                tty_console_isig_raise(sig);
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
            int sig = uart_rx_push_locked((char)c, &wake_q);
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (sig)
                tty_console_isig_raise(sig);
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
            int sig = uart_rx_push_locked((char)c, &wake_q);
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (sig)
                tty_console_isig_raise(sig);
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
    if (current_board && current_board->uart_rx_is_polled) {
        /* Polled boards have no top half, but the read and the store still go
         * in one rx_lock section so a concurrent reader cannot push a later
         * byte first (RX_LOCK_INVARIANT). */
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t pflags = spin_lock_irqsave(&rx_lock);
        int polled = arch_uart_poll_getc();
        int sig = 0;
        if (polled >= 0)
            sig = uart_rx_push_locked((char)polled, &wake_q);
        spin_unlock_irqrestore(&rx_lock, pflags);
        (void)proc_wake_q_flush(&wake_q);
        if (sig)
            tty_console_isig_raise(sig);
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
    /* LOCK_ORDER: IRQ handler pushes characters under rx_lock; no other locks
     * are acquired while it is held.  The receive register is drained *inside*
     * that same section (RX_LOCK_INVARIANT): reading a byte here and storing
     * it after dropping the lock would let a task-context poll of the same
     * register slot in between and publish its byte first.  The wake flush and
     * the Ctrl-C side effects run after the release, where taking further
     * locks is legal. */
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    int sig = 0;
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int c;
    while ((c = arch_uart_poll_getc()) >= 0) {
        rx_irq_bytes++;
        int s = uart_rx_push_locked((char)c, &wake_q);
        if (s)
            sig = s;
    }
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
    arch_uart_ack_irq();
    if (sig)
        tty_console_isig_raise(sig);
}

static int uart_irq_wrapper(int irq, void *priv) {
    (void)irq;
    (void)priv;
    static int wrapper_once;
    if (!wrapper_once) { wrapper_once = 1;
        kinfo("[IRQDBG] uart top-half first run\n"); }
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

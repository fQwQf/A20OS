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

static void uart_dump_tasks(void)
{
    /* LOCK_ORDER: proc_lock acquired while not holding rx_lock (task dump helper). */
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    kdebug("[TTYDBG] task dump begin\n");
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED)
            continue;
        const char *state = "?";
        switch (t->state) {
        case PROC_READY:   state = "READY"; break;
        case PROC_RUNNING: state = "RUN"; break;
        case PROC_BLOCKED: state = "BLOCK"; break;
        case PROC_ZOMBIE:  state = "ZOMB"; break;
        default: break;
        }
        kdebug("[TTYDBG] pid=%d ppid=%d pgid=%d sid=%d state=%s wake=%lu onrq=%d name=%s\n",
               t->pid, t->ppid, t->pgid, t->sid, state,
               (unsigned long)t->wake_time, t->on_rq, t->name);
    }
    kdebug("[TTYDBG] task dump end\n");
    spin_unlock_irqrestore(&proc_lock, flags);
}

/*
 * CTRL_C_CONTEXT_SPLIT: the two signal walks stay in the top half, because the
 * foreground process group has to learn about the key while the shell is busy
 * running a command and therefore nowhere near a console read -- deferring them
 * would stop Ctrl-C from killing anything.  proc_find_get() takes only pid_lock
 * and signal delivery from an interrupt is what that path is for.
 *
 * The task-table dump is the part that cannot happen here.  uart_dump_tasks()
 * takes proc_lock with interrupts disabled and then writes one kdebug line per
 * task out of the same UART whose interrupt is running, so a dump started from
 * the top half re-enters the console under its own IRQ and can livelock the
 * console behind the very lock it is printing.  The top half only raises the
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
    }
    spin_unlock_irqrestore(&rx_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
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
        uint64_t flags = spin_lock_irqsave(&rx_lock);
        if (rx_head != rx_tail) {
            char c = rx_buffer[rx_tail];
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
            spin_unlock_irqrestore(&rx_lock, flags);
            return (int)(unsigned char)c;
        }
        spin_unlock_irqrestore(&rx_lock, flags);

        int c = arch_uart_poll_getc();
        if (c >= 0) {
            uart_rx_push((char)c);
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
        c = arch_uart_poll_getc();
        if (c >= 0) {
            spin_unlock_irqrestore(&rx_lock, flags);
            uart_rx_push((char)c);
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
            spin_unlock_irqrestore(&rx_lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            uart_rx_push((char)c);
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

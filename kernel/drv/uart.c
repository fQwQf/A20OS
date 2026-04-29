#include "drv/uart.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/timer.h"
#include "proc/proc.h"

// 接收缓冲区大小
#define RX_BUF_SIZE 256

// 接收缓冲区（环形缓冲区）
static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint32_t rx_head;  // 缓冲区头指针
static volatile uint32_t rx_tail;  // 缓冲区尾指针
static spinlock_t rx_lock = SPINLOCK_INIT;
static task_t *rx_waiter;

static void uart_wake_waiter_locked(void) {
    if (rx_waiter && rx_waiter->state == PROC_BLOCKED) {
        task_t *t = rx_waiter;
        rx_waiter = NULL;
        proc_make_ready(t);
    }
}

static void uart_rx_push(char c) {
    if (c == 0x03) {  // Ctrl-C
        /* This kernel has pgid/sid bookkeeping but no real tty foreground
         * process-group tracking yet. Signal the task currently attached to
         * the console path instead of broadcasting to the whole pgid. */
        task_t *cur = proc_current();
        if (cur && cur->pid > 0) {
            proc_kill(cur->pid, SIGINT);
        }
        return;
    }

    uint64_t flags = spin_lock_irqsave(&rx_lock);
    uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buffer[rx_head] = c;
        rx_head = next;
        uart_wake_waiter_locked();
    }
    spin_unlock_irqrestore(&rx_lock, flags);
}

// 初始化 UART 设备
void uart_init(void) {
    rx_head = 0;
    rx_tail = 0;
    rx_waiter = NULL;
    spin_init(&rx_lock);
    arch_uart_init();
    uart_flush();  // 等待发送完成
}

// 发送一个字符
void uart_putc(char c) {
    arch_uart_putc(c);
}

// 阻塞式读取一个字符（如果没有数据则让出 CPU）
int uart_getc(void) {
    for (;;) {
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

        task_t *cur = proc_current();
        if (!cur) {
            arch_local_irq_enable();
            __asm__ volatile("nop");
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
        rx_waiter = cur;
        proc_set_wake_time(cur, timer_get_ticks() + (TICKS_PER_SEC / 20));
        cur->state = PROC_BLOCKED;
        spin_unlock_irqrestore(&rx_lock, flags);

        arch_local_irq_enable();
        sched();
        arch_local_irq_disable();

        flags = spin_lock_irqsave(&rx_lock);
        if (rx_waiter == cur)
            rx_waiter = NULL;
        proc_set_wake_time(cur, 0);
        spin_unlock_irqrestore(&rx_lock, flags);
    }
}

// 非阻塞式尝试读取一个字符
int uart_try_getc(void) {
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    if (rx_head == rx_tail) {
        spin_unlock_irqrestore(&rx_lock, flags);
        return -1;
    }
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    spin_unlock_irqrestore(&rx_lock, flags);
    return (int)(unsigned char)c;
}

// 检查是否有输入数据
int uart_has_input(void) {
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    int has = rx_head != rx_tail;
    spin_unlock_irqrestore(&rx_lock, flags);
    return has;
}

// 发送字符串
void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

// 等待发送完成
void uart_flush(void) {
    arch_uart_flush();
}

// UART 中断处理函数
void uart_handle_irq(void) {
    int c;
    while ((c = arch_uart_poll_getc()) >= 0) {
        uart_rx_push(c);
    }
    arch_uart_ack_irq();
}

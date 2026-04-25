#include "drv/uart.h"
#include "core/consts.h"
#include "core/defs.h"
#include "proc/proc.h"

// 接收缓冲区大小
#define RX_BUF_SIZE 256

// 接收缓冲区（环形缓冲区）
static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint32_t rx_head;  // 缓冲区头指针
static volatile uint32_t rx_tail;  // 缓冲区尾指针

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

    uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buffer[rx_head] = c;
        rx_head = next;
    }
}

// 初始化 UART 设备
void uart_init(void) {
    rx_head = 0;
    rx_tail = 0;
    arch_uart_init();
    uart_flush();  // 等待发送完成
}

// 发送一个字符
void uart_putc(char c) {
    arch_uart_putc(c);
}

// 阻塞式读取一个字符（如果没有数据则让出 CPU）
int uart_getc(void) {
    while (rx_head == rx_tail) {
        int c = arch_uart_poll_getc();
        if (c >= 0) {
            uart_rx_push((char)c);
            continue;
        }
        arch_local_irq_enable();
        proc_yield();
        arch_local_irq_disable();
    }
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return (int)(unsigned char)c;
}

// 非阻塞式尝试读取一个字符
int uart_try_getc(void) {
    if (rx_head == rx_tail)
        return -1;
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return (int)(unsigned char)c;
}

// 检查是否有输入数据
int uart_has_input(void) {
    return rx_head != rx_tail;
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

#include "uart.h"
#include "consts.h"
#include "defs.h"
#include "proc.h"

#define UART0 ((volatile uint8_t *)UART0_BASE)

// UART 寄存器偏移
#define RHR 0  // 接收保持寄存器
#define THR 0  // 发送保持寄存器
#define IER 1  // 中断使能寄存器
#define FCR 2  // FIFO 控制寄存器
#define LCR 3  // 线路控制寄存器
#define MCR 4  // 调制解调器控制寄存器
#define LSR 5  // 线路状态寄存器

// 中断使能位
#define IER_RX_ENABLE (1 << 0)  // 接收中断使能
#define IER_TX_ENABLE (1 << 1)  // 发送中断使能

// 线路状态位
#define LSR_RX_READY  (1 << 0)  // 接收数据就绪
#define LSR_TX_IDLE   (1 << 5)  // 发送空闲
#define LSR_TX_EMPTY  (1 << 6)  // 发送空

// 接收缓冲区大小
#define RX_BUF_SIZE 256

// 接收缓冲区（环形缓冲区）
static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint32_t rx_head;  // 缓冲区头指针
static volatile uint32_t rx_tail;  // 缓冲区尾指针

// 初始化 UART 设备
void uart_init(void) {
    rx_head = 0;
    rx_tail = 0;

    UART0[IER] = 0x00;  // 禁用中断
    UART0[LCR] = 0x80;  // 设置 DLAB 位，配置波特率
    UART0[0] = 0x03;  // 低字节：波特率除数低 8 位
    UART0[1] = 0x00;  // 高字节：波特率除数高 8 位
    UART0[LCR] = 0x03;  // 清除 DLAB，设置 8 位数据，无校验，1 停止位
    UART0[FCR] = 0x07;  // 启用 FIFO，清空接收/发送 FIFO
    UART0[MCR] = 0x0B;  // 设置 RTS/DTR，启用中断
    UART0[IER] = IER_RX_ENABLE;  // 启用接收中断
    uart_flush();  // 等待发送完成
}

// 发送一个字符
void uart_putc(char c) {
    while (!(UART0[LSR] & LSR_TX_IDLE));
    UART0[THR] = (uint8_t)c;
}

// 阻塞式读取一个字符（如果没有数据则让出 CPU）
int uart_getc(void) {
    while (rx_head == rx_tail) {
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
    while (!(UART0[LSR] & LSR_TX_EMPTY));
}

// UART 中断处理函数
void uart_handle_irq(void) {
    while (UART0[LSR] & LSR_RX_READY) {
        char c = UART0[RHR];
        if (c == 0x03) {  // Ctrl-C
            /* proc_current() in IRQ context = the task that was preempted,
             * which is the foreground process. Signal its whole group. */
            task_t *cur = proc_current();
            if (cur && cur->pid > 0 && cur->pgid > 0) {
                proc_kill_pgid(cur->pgid, SIGINT, 0);
            }
            continue;
        }
        uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buffer[rx_head] = c;
            rx_head = next;
        }
    }
}

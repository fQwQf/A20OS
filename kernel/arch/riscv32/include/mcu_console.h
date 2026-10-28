#ifndef A20_RISCV_MCU_CONSOLE_H
#define A20_RISCV_MCU_CONSOLE_H
void arch_uart_init(void);
void arch_uart_putc(char c);
int arch_uart_poll_getc(void);
void arch_uart_flush(void);
void arch_uart_ack_irq(void);
int arch_uart_irq(void);
#endif

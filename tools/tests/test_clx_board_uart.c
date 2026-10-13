#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#define _ARCH_H
#define CONFIG_STM32_CLUSTER_PORT 2
static unsigned irq_held;
static uint32_t arch_irq_save(void) {
    assert(!irq_held);
    irq_held = 1;
    return 0;
}
static void arch_irq_restore(uint32_t flags) {
    (void)flags;
    assert(irq_held);
    irq_held = 0;
}
#include "../../kernel/platform/stm32f103/cluster_uart.c"

static int (*send_byte)(void *, uint8_t);
static int (*receive_byte)(void *);
static uint32_t port_bytes, irq_disabled;
static int init_failure, bind_failure, tx_failure;
static uint8_t last_byte;
static int device_input[700];
static unsigned input_head, input_tail;

int a20_mcu_leaf_set_io(int (*send)(void *, uint8_t), int (*receive)(void *),
                        void *ctx) {
    assert(!ctx);
    send_byte = send;
    receive_byte = receive;
    return bind_failure;
}
int stm32_uart_init(stm32_uart_port_t port, uint32_t baud, int irq) {
    assert(port == STM32_UART_USART2 && baud == 115200 && irq == 1);
    return init_failure;
}
void stm32_uart_set_rx_irq(stm32_uart_port_t port, int enable) {
    assert(port == STM32_UART_USART2 && !enable);
    irq_disabled++;
}
int stm32_uart_send_byte(stm32_uart_port_t port, uint8_t byte,
                         uint32_t timeout) {
    assert(port == STM32_UART_USART2 && timeout == 100000 && !irq_held);
    last_byte = byte;
    port_bytes++;
    return tx_failure;
}
int stm32_uart_poll_byte(stm32_uart_port_t port, uint8_t *byte) {
    assert(port == STM32_UART_USART2);
    if (input_head == input_tail)
        return 0;
    int value = device_input[input_head++];
    if (value < 0)
        return -1;
    *byte = (uint8_t)value;
    return 1;
}
int main(void) {
    init_failure = -1;
    assert(stm32_cluster_uart_init() < 0 && !send_byte);
    init_failure = 0;
    bind_failure = -1;
    assert(stm32_cluster_uart_init() < 0 && irq_disabled == 1);
    bind_failure = 0;
    assert(stm32_cluster_uart_init() == 0);
    for (unsigned i = 0; i <= 255; i++) {
        assert(send_byte(NULL, (uint8_t)i) == 0);
        assert(last_byte == i);
    }
    assert(port_bytes ==
           256); /* Includes LF, END and ESC, without rewriting. */
    tx_failure = -1;
    assert(send_byte(NULL, 10) < 0);
    assert(receive_byte(NULL) < 0);
    for (unsigned i = 0; i < 600; i++)
        device_input[input_tail++] = i & 255;
    device_input[input_tail++] = -1;
    stm32_cluster_uart_irq();
    assert(stm32_cluster_uart_rx_drops() == 90); /* 89 full + one HW error. */
    for (unsigned i = 0; i < 511; i++)
        assert(receive_byte(NULL) == (int)(i & 255));
    assert(receive_byte(NULL) < 0 && !irq_held);
    for (unsigned i = 0; i < 90; i++)
        device_input[input_tail++] = i;
    stm32_cluster_uart_irq();
    for (unsigned i = 0; i < 90; i++)
        assert(receive_byte(NULL) == (int)i);
    assert(receive_byte(NULL) < 0);
    puts("STM32 raw UART: all bytes, TX failure, RX overflow/error/wrap PASS");
    return 0;
}

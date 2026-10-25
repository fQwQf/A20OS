#include "board.h"

#ifdef CONFIG_STM32_CLUSTER_PORT
#include "core/arch.h"
#include "drivers/stm32f1/stm32_uart.h"
#include "mcu/leaf.h"

/* One ISR producer, one leaf-pump consumer. Overflow drops bytes and is
 * counted; SLIP/CRC and transaction retry recover a damaged packet. */
#define CLUSTER_RX_BYTES 512u
static uint8_t cluster_rx[CLUSTER_RX_BYTES];
static volatile uint32_t cluster_head, cluster_tail, cluster_drops;
static const stm32_uart_port_t cluster_port =
    CONFIG_STM32_CLUSTER_PORT == 2 ? STM32_UART_USART2 : STM32_UART_USART3;

static int cluster_send(void *ctx, uint8_t byte) {
    (void)ctx;
    return stm32_uart_send_byte(cluster_port, byte, 100000u);
}

static int cluster_receive(void *ctx) {
    (void)ctx;
    uint32_t flags = arch_irq_save();
    int value = -1;
    if (cluster_tail != cluster_head) {
        value = cluster_rx[cluster_tail];
        cluster_tail = (cluster_tail + 1u) % CLUSTER_RX_BYTES;
    }
    arch_irq_restore(flags);
    return value;
}

int stm32_cluster_uart_init(void) {
    cluster_head = cluster_tail = cluster_drops = 0;
    if (stm32_uart_init(cluster_port, 115200u, 1) != 0)
        return -1;
    if (a20_mcu_leaf_set_io(cluster_send, cluster_receive, NULL) != 0) {
        stm32_uart_set_rx_irq(cluster_port, 0);
        return -1;
    }
    return 0;
}

void stm32_cluster_uart_irq(void) {
    uint8_t byte;
    int result;
    while ((result = stm32_uart_poll_byte(cluster_port, &byte)) != 0) {
        if (result < 0) {
            cluster_drops++;
            continue;
        }
        uint32_t next = (cluster_head + 1u) % CLUSTER_RX_BYTES;
        if (next == cluster_tail)
            cluster_drops++;
        else {
            cluster_rx[cluster_head] = byte;
            cluster_head = next;
        }
    }
}

uint32_t stm32_cluster_uart_rx_drops(void) {
    return cluster_drops;
}
#endif

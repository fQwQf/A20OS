#ifdef CONFIG_BOARD_STM32F103

#include "arch/platform_hooks.h"
#include "board.h"
#include "console.h"
#include "drivers/char/uart.h"
#include "drivers/stm32f1/backlight.h"
#include "drivers/stm32f1/bluetooth.h"
#include "drivers/stm32f1/ir.h"
#include "drivers/stm32f1/stm32_uart.h"
#include "drivers/stm32f1/wifi.h"
#include "live2d.h"

#define STM32_IRQ_EXTI9_5 23U
#define STM32_IRQ_USART1  37U
#define STM32_IRQ_USART2  38U
#define STM32_IRQ_USART3  39U

static volatile uint32_t live2d_clock;
static uint32_t live2d_countdown = LIVE2D_FRAME_MS;

uint32_t armv7m_platform_core_clock_hz(void) {
    return stm32_hclk_hz();
}

void armv7m_platform_systick(uint64_t ticks) {
    if (--live2d_countdown == 0U) {
        live2d_countdown = LIVE2D_FRAME_MS;
        live2d_clock++;
    }
    stm32_backlight_systick();
    if ((ticks % 500U) == 0U)
        stm32_status_led_toggle();
}

uint32_t stm32_live2d_frame_clock(void) {
    return live2d_clock;
}

void armv7m_platform_irq_dispatch(uint32_t irq) {
    int c;

    switch (irq) {
    case STM32_IRQ_EXTI9_5:
        stm32_ir_isr();
        break;
    case STM32_IRQ_USART1:
        while ((c = arch_uart_poll_getc()) >= 0)
            uart_receive_char((char)c);
        arch_uart_ack_irq();
        break;
    case STM32_IRQ_USART2:
        stm32_wifi_irq();
        break;
    case STM32_IRQ_USART3:
        stm32_bluetooth_irq();
        break;
    default:
        break;
    }
}

void armv7m_platform_fault_notify(void) {
    stm32_status_led_set(1);
}

#endif

#ifndef _STM32F103_BOARD_CONFIG_H
#define _STM32F103_BOARD_CONFIG_H

/* Build-time board geometry. The ARMv7-M architecture consumes only the
 * generic aliases below; STM32 addresses stay owned by this platform. */
#ifndef STM32_FLASH_KB
#define STM32_FLASH_KB 64
#endif

#ifndef STM32_RAM_KB
#define STM32_RAM_KB 20
#endif

#define ARMV7M_FLASH_BASE        0x08000000UL
#define ARMV7M_RAM_BASE          0x20000000UL
#define ARMV7M_CONSOLE_UART_BASE 0x40013800UL
#define ARMV7M_CONSOLE_UART_IRQ  37U

#endif

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

#define STM32_FLASH_CONTROL_BASE 0x40022000UL

/* Register windows used by the shared-bus/fabric setup.  Keeping the physical
 * addresses here, next to the rest of this board's memory map, is what lets
 * arch/armv7m/boards/stm32f103 C sources stay free of hardcoded MMIO constants. */
#define STM32_RCC_APB2ENR   0x40021018UL
#define STM32_RCC_AHBENR    0x40021014UL
#define STM32_GPIOD_CRL     0x40011400UL
#define STM32_GPIOE_CRL     0x40011800UL
#define STM32_GPIOF_CRL     0x40011C00UL
#define STM32_GPIOG_CRL     0x40012000UL
#define STM32_FSMC_BCR1     0xA0000000UL
#define STM32_FSMC_BTR1     0xA0000004UL
#define STM32_FSMC_BCR2     0xA0000008UL
#define STM32_FSMC_BTR2     0xA000000CUL

#define ARMV7M_FLASH_BASE        0x08000000UL
#define ARMV7M_RAM_BASE          0x20000000UL
#define ARMV7M_CONSOLE_UART_BASE 0x40013800UL
#define ARMV7M_CONSOLE_UART_IRQ  37U

#endif

#ifndef _STM32F103_EXTSRAM_H
#define _STM32F103_EXTSRAM_H

#include "core/types.h"

#define STM32_EXTSRAM_BASE 0x68000000UL
#define STM32_EXTSRAM_SIZE (1024U * 1024U)

int stm32_extsram_init(void);
void stm32_extsram_shutdown(void);
int stm32_extsram_ready(void);
size_t stm32_extsram_available(void);
void *stm32_extsram_alloc(size_t size);
void stm32_extsram_free(void *ptr);
int stm32_extsram_owns(const void *ptr);
size_t stm32_extsram_allocation_size(const void *ptr);

#endif

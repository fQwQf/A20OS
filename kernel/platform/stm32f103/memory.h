#ifndef _STM32F103_MEMORY_H
#define _STM32F103_MEMORY_H

#include "core/types.h"

typedef struct stm32_memory_info {
    uint16_t device_id;
    uint16_t revision_id;
    int silicon_capacity_valid;
    int flash_capacity_from_silicon;
    int ram_capacity_from_silicon;
    size_t internal_ram_total;
    size_t internal_ram_used;
    size_t internal_ram_free;
    size_t internal_heap_total;
    size_t internal_heap_used;
    size_t internal_heap_free;
    size_t stack_peak_bytes;
    size_t external_ram_total;
    size_t external_ram_used;
    size_t external_ram_free;
    size_t flash_total;
    size_t flash_used;
} stm32_memory_info_t;

void stm32_memory_init(void);
void stm32_memory_refresh(void);
const stm32_memory_info_t *stm32_memory_info(void);

#endif

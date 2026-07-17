#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/memory.h"
#include "mcu/heap.h"
#include "drivers/stm32f1/extsram.h"

#define STM32_DBGMCU_IDCODE (*(volatile uint32_t *)0xE0042000UL)
#define STM32_FLASH_SIZE_KB (*(volatile uint16_t *)0x1FFFF7E0UL)
#define STM32_STACK_PATTERN 0xA55AA55AU

extern char _heap_start[];
extern char _ram_origin[];
extern char _ram_linked_end[];
extern char _stack_limit[];
extern char _stack_top[];
extern char _flash_origin[];
extern char _flash_linked_end[];
extern char _flash_image_end[];

static stm32_memory_info_t memory_info;

static size_t linked_ram_bytes(void) {
    return (uintptr_t)_ram_linked_end - (uintptr_t)_ram_origin;
}

static size_t linked_flash_bytes(void) {
    return (uintptr_t)_flash_linked_end - (uintptr_t)_flash_origin;
}

#ifdef CONFIG_STM32_XUANWU
static size_t detect_internal_ram(uint16_t device_id, uint16_t flash_kb) {
    switch (device_id) {
    case 0x412U: /* STM32F1 low-density performance line */
        return 10U * 1024U;
    case 0x410U: /* STM32F1 medium-density performance line */
        return 20U * 1024U;
    case 0x414U: /* STM32F1 high-density performance line */
        return (flash_kb <= 256U ? 48U : 64U) * 1024U;
    case 0x418U: /* STM32F1 connectivity line */
        return 64U * 1024U;
    case 0x430U: /* STM32F1 XL-density performance line */
        return 96U * 1024U;
    default:
        return 0;
    }
}
#endif

static size_t stack_peak_bytes(void) {
    const volatile uint32_t *p =
        (const volatile uint32_t *)(uintptr_t)_stack_limit;
    const volatile uint32_t *top =
        (const volatile uint32_t *)(uintptr_t)_stack_top;

    while (p < top && *p == STM32_STACK_PATTERN)
        p++;
    return (uintptr_t)top - (uintptr_t)p;
}

void stm32_memory_init(void) {
    size_t ram_total = linked_ram_bytes();
    size_t flash_total = linked_flash_bytes();

    memory_info.device_id = 0;
    memory_info.revision_id = 0;
    memory_info.silicon_capacity_valid = 0;
    memory_info.flash_capacity_from_silicon = 0;
    memory_info.ram_capacity_from_silicon = 0;

#ifdef CONFIG_STM32_XUANWU
    uint32_t idcode = STM32_DBGMCU_IDCODE;
    uint16_t flash_kb = STM32_FLASH_SIZE_KB;
    size_t detected_ram =
        detect_internal_ram((uint16_t)(idcode & 0x0FFFU), flash_kb);

    memory_info.device_id = (uint16_t)(idcode & 0x0FFFU);
    memory_info.revision_id = (uint16_t)(idcode >> 16);
    if (flash_kb >= 16U && flash_kb <= 1024U &&
        (size_t)flash_kb * 1024U >= linked_flash_bytes()) {
        flash_total = (size_t)flash_kb * 1024U;
        memory_info.flash_capacity_from_silicon = 1;
    }
    if (detected_ram != 0 && detected_ram >= linked_ram_bytes()) {
        ram_total = detected_ram;
        memory_info.ram_capacity_from_silicon = 1;
    }
    if (memory_info.flash_capacity_from_silicon &&
        memory_info.ram_capacity_from_silicon)
        memory_info.silicon_capacity_valid = 1;
#endif

    memory_info.internal_ram_total = ram_total;
    memory_info.flash_total = flash_total;
    memory_info.flash_used =
        (uintptr_t)_flash_image_end - (uintptr_t)_flash_origin;
    stm32_memory_refresh();
}

void stm32_memory_refresh(void) {
    mcu_heap_stats_t heap;
    mcu_heap_get_stats(&heap);

    memory_info.internal_heap_total = heap.arena_bytes;
    memory_info.internal_heap_used = heap.used_bytes;
    memory_info.internal_heap_free = heap.free_bytes;
    memory_info.stack_peak_bytes = stack_peak_bytes();

    size_t static_bytes =
        (uintptr_t)_heap_start - (uintptr_t)_ram_origin;
    memory_info.internal_ram_used =
        static_bytes + heap.used_bytes + memory_info.stack_peak_bytes;
    if (memory_info.internal_ram_used > memory_info.internal_ram_total)
        memory_info.internal_ram_used = memory_info.internal_ram_total;
    memory_info.internal_ram_free =
        memory_info.internal_ram_total - memory_info.internal_ram_used;

    if (stm32_extsram_ready()) {
        memory_info.external_ram_total = stm32_extsram_capacity();
        memory_info.external_ram_free = stm32_extsram_available();
        memory_info.external_ram_used =
            memory_info.external_ram_total - memory_info.external_ram_free;
    } else {
        memory_info.external_ram_total = 0;
        memory_info.external_ram_used = 0;
        memory_info.external_ram_free = 0;
    }
}

const stm32_memory_info_t *stm32_memory_info(void) {
    return &memory_info;
}

#endif

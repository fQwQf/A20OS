#include "mcu/heap_backend.h"
#include "drivers/stm32f1/extsram.h"

size_t arch_mcu_heap_available(void) {
    return stm32_extsram_available();
}
void *arch_mcu_heap_alloc(size_t size) {
    return stm32_extsram_alloc(size);
}
void arch_mcu_heap_free(void *pointer) {
    stm32_extsram_free(pointer);
}
int arch_mcu_heap_owns(const void *pointer) {
    return stm32_extsram_owns(pointer);
}
size_t arch_mcu_heap_allocation_size(const void *pointer) {
    return stm32_extsram_allocation_size(pointer);
}

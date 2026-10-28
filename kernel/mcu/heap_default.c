#include "mcu/heap_backend.h"
/* Boards with an additional arena override these individually. */
__attribute__((weak)) size_t arch_mcu_heap_available(void) { return 0; }
__attribute__((weak)) void *arch_mcu_heap_alloc(size_t n) { (void)n; return NULL; }
__attribute__((weak)) void arch_mcu_heap_free(void *p) { (void)p; }
__attribute__((weak)) int arch_mcu_heap_owns(const void *p) { (void)p; return 0; }
__attribute__((weak)) size_t arch_mcu_heap_allocation_size(const void *p) { (void)p; return 0; }

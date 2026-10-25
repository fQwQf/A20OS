#ifndef A20_MCU_HEAP_BACKEND_H
#define A20_MCU_HEAP_BACKEND_H
#include "core/types.h"

/* Optional board-owned arena outside the common MCU heap. An unavailable
 * arena returns zero/NULL; owns() gates every free and allocation-size query.
 * The backend provides its own synchronization and hardware initialization. */
size_t arch_mcu_heap_available(void);
void *arch_mcu_heap_alloc(size_t size);
void arch_mcu_heap_free(void *pointer);
int arch_mcu_heap_owns(const void *pointer);
size_t arch_mcu_heap_allocation_size(const void *pointer);
#endif

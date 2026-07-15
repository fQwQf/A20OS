#ifndef _MCU_HEAP_H
#define _MCU_HEAP_H

#include "core/types.h"

typedef struct mcu_heap_stats {
    size_t arena_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t largest_free_bytes;
} mcu_heap_stats_t;

void mcu_heap_init(void);
size_t mcu_heap_available(void);
void mcu_heap_get_stats(mcu_heap_stats_t *stats);

#endif

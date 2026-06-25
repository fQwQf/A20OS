#include "core/cpu.h"
#include "core/random.h"
#include "core/timer.h"
#include "mm/mm.h"
#include "proc/proc.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

uint64_t a20_random_entropy_sample(void) {
    uint64_t v = timer_get_ticks();
    v ^= (uint64_t)(uintptr_t)__builtin_return_address(0);
    v ^= (uint64_t)(uintptr_t)&v;
    v ^= arch_read_addr_space_token();
    v ^= frame_free_count() << 32;
    v ^= (uint64_t)(uintptr_t)proc_current();
    return v;
}

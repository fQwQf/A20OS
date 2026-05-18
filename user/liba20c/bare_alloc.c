/*
 * A20OS liba20c — bare memory allocator for pre-malloc init.
 */
#include <stdint.h>
#include "../liba20rt/a20_syscall.h"

static uint8_t *bare_base;
static uint64_t bare_pos;
static uint64_t bare_cap;

void *__bare_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (bare_pos + n > bare_cap) {
        uint64_t chunk = n > 65536 ? n : 65536;
        uint64_t args[3] = {chunk, 0, 0};
        int64_t r = a20_vm_alloc(args);
        if (r < 0) return NULL;
        bare_base = (uint8_t *)args[2];
        bare_pos = 0;
        bare_cap = chunk;
    }
    void *p = bare_base + bare_pos;
    bare_pos += n;
    return p;
}

/*
 * A20OS liba20c — bare memory allocator for pre-malloc init.
 */
#include <stdint.h>
#include <stddef.h>
#include "../liba20rt/a20_syscall.h"

static uint8_t *bare_base;
static uint64_t bare_pos;
static uint64_t bare_cap;

void *__bare_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (bare_pos + n > bare_cap) {
        uint64_t chunk = n > 65536 ? n : 65536;
        a20_vm_alloc_args_t args;
        args.size      = sizeof(args);
        args.version   = 1;
        args.addr_hint = 0;
        args.length    = chunk;
        args.prot      = A20_PROT_READ | A20_PROT_WRITE;
        args.flags     = 0;
        args.out_addr  = 0;
        int64_t r = a20_syscall6(A20_SYS_vm_alloc, (uint64_t)&args, 0, 0, 0, 0, 0);
        if (r < 0) return NULL;
        bare_base = (uint8_t *)args.out_addr;
        bare_pos = 0;
        bare_cap = chunk;
    }
    void *p = bare_base + bare_pos;
    bare_pos += n;
    return p;
}

#ifndef A20_NATIVE_MEMORY_CONTRACT_H
#define A20_NATIVE_MEMORY_CONTRACT_H
/* Same allocator algorithm/test on a flat MCU and private-VM servers.
 * Calls are deliberately serialized: the SDK allocator has no mutex yet. */
#include "a20_mem.h"
static int native_memory_contract(void) {
    if (a20_malloc(0) || a20_malloc(UINT64_MAX) || a20_malloc(UINTPTR_MAX) ||
        a20_calloc(UINT64_MAX / 16 + 1, 16))
        return 1;
    unsigned char *p = a20_malloc(17);
    unsigned char *q = a20_calloc(4, 8);
    if (!p || !q || ((uintptr_t)p & 15) || ((uintptr_t)q & 15))
        return 2;
    for (unsigned i = 0; i < 32; i++)
        if (q[i])
            return 3;
    for (unsigned i = 0; i < 17; i++)
        p[i] = (unsigned char)(i + 31);
    unsigned char *grown = a20_realloc(p, 8200);
    if (!grown || ((uintptr_t)grown & 15))
        return 4;
    for (unsigned i = 0; i < 17; i++)
        if (grown[i] != i + 31)
            return 5;
    if (a20_realloc(grown, UINT64_MAX))
        return 6;
    for (unsigned i = 0; i < 17; i++)
        if (grown[i] != i + 31)
            return 7;
    if (a20_realloc(grown, 9) != grown)
        return 8;
    a20_free((void *)0);
    a20_free(grown);
    a20_free(q);
    unsigned char *reused = a20_malloc(8);
    if (reused != grown)
        return 9;
    a20_free(reused);
    void *fresh = a20_realloc((void *)0, 23);
    if (!fresh || ((uintptr_t)fresh & 15))
        return 10;
    if (a20_realloc(fresh, 0))
        return 11;
    return 0;
}
#endif

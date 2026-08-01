/*
 * A20OS x86_64 — hardware entropy source (RDRAND / RDSEED).
 *
 * Mixed into the core software PRNG by random.c so getrandom() is seeded
 * with genuine CPU entropy when the instruction is available.
 */
#include "core/types.h"

int arch_hw_entropy_sample(uint64_t *out)
{
    if (!out)
        return 0;

    unsigned char ok = 0;
    uint64_t v = 0;

    /* RDRAND is documented to occasionally fail transiently; retry. */
    for (int i = 0; i < 10; i++) {
        __asm__ __volatile__("rdrand %0; setc %1"
                             : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return 1;
        }
    }

    /* Fall back to RDSEED, which draws directly from the entropy source. */
    for (int i = 0; i < 10; i++) {
        __asm__ __volatile__("rdseed %0; setc %1"
                             : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return 1;
        }
    }

    return 0;
}

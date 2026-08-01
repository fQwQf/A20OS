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

    /* Check CPUID before executing RDRAND/RDSEED: a CPU or hypervisor without
     * the feature faults with #UD on the instruction. */
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1), "c"(0));
    int has_rdrand = (ecx >> 30) & 1;
    int has_rdseed = 0;
    if ((ecx >> 27) & 1) {      /* OSXSAVE leaf 7 available */
        __asm__ __volatile__("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(7), "c"(0));
        has_rdseed = (ebx >> 18) & 1;
    }
    if (!has_rdrand && !has_rdseed)
        return 0;

    unsigned char ok = 0;
    uint64_t v = 0;

    /* RDRAND is documented to occasionally fail transiently; retry. */
    if (has_rdrand) {
        for (int i = 0; i < 10; i++) {
            __asm__ __volatile__("rdrand %0; setc %1"
                                 : "=r"(v), "=qm"(ok));
            if (ok) {
                *out = v;
                return 1;
            }
        }
    }

    /* Fall back to RDSEED, which draws directly from the entropy source. */
    if (has_rdseed) {
        for (int i = 0; i < 10; i++) {
            __asm__ __volatile__("rdseed %0; setc %1"
                                 : "=r"(v), "=qm"(ok));
            if (ok) {
                *out = v;
                return 1;
            }
        }
    }

    return 0;
}

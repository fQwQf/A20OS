#ifndef A20_X86_TSC_BAYTRAIL_H
#define A20_X86_TSC_BAYTRAIL_H

#include "core/types.h"

/* Hardware facts: Intel SDM Vol. 4, MSR_PLATFORM_INFO (0xce),
 * MSR_FSB_FREQ (0xcd), and Linux v6.17 arch/x86/kernel/tsc_msr.c's
 * measured Bay Trail reference-clock ratios. This is deliberately limited to
 * GenuineIntel family 6/model 0x37. Other Atom models use different tables.
 * A VM may advertise that model without implementing its MSRs, so it must use
 * the ordinary CPUID/PIT discovery path instead. */
struct x86_tsc_probe_ops {
    void *opaque;
    void (*cpuid)(void *, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
    uint64_t (*read_msr)(void *, uint32_t);
};

static inline uint64_t x86_baytrail_tsc_hz(const struct x86_tsc_probe_ops *ops) {
    uint32_t a, b, c, d;
    ops->cpuid(ops->opaque, 0, &a, &b, &c, &d);
    if (a < 1 || b != 0x756e6547U || d != 0x49656e69U || c != 0x6c65746eU)
        return 0;
    ops->cpuid(ops->opaque, 1, &a, &b, &c, &d);
    unsigned family = (a >> 8) & 15;
    unsigned model = (a >> 4) & 15;
    if (family == 15)
        family += (a >> 20) & 255;
    if (family == 6 || family == 15)
        model |= ((a >> 16) & 15) << 4;
    if (family != 6 || model != 0x37 || (c & (1U << 31)))
        return 0;

    unsigned ratio = (unsigned)(ops->read_msr(ops->opaque, 0xce) >> 8) & 255;
    unsigned index = (unsigned)ops->read_msr(ops->opaque, 0xcd) & 7;
    uint64_t numerator, denominator;
    switch (index) {
    case 0: numerator = 5; denominator = 6; break;
    case 1: numerator = 1; denominator = 1; break;
    case 2: numerator = 4; denominator = 3; break;
    case 3: numerator = 7; denominator = 6; break;
    case 4: numerator = 4; denominator = 5; break;
    default: return 0; /* Reserved frequency encodings are not guesses. */
    }
    /* Multiply before dividing to preserve the fractional reference clock. */
    uint64_t hz = (100000000ULL * numerator * ratio + denominator / 2) / denominator;
    return hz >= 100000000ULL && hz <= 10000000000ULL ? hz : 0;
}

#endif

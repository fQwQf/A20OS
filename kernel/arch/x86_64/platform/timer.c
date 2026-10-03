#ifdef CONFIG_X86_64

#include "core/defs.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "cpu.h"
#include "platform.h"
#include "firmware.h"

#define TSC_FREQ_MIN       100000000ULL
#define TSC_FREQ_MAX       10000000000ULL
#define TSC_CALIBRATED_MAX 10000000000000ULL
#define LAPIC_FREQ_MIN     10000ULL
#define LAPIC_FREQ_FALLBACK 10000000ULL
#define LAPIC_CALIBRATION_TICKS (ARCH_TIMER_FREQ / 100)
#define PIT_FREQ           1193182ULL
#define PIT_CALIBRATION_COUNT 59659U
#define HPET_CAPABILITIES   0x000
#define HPET_CONFIGURATION  0x010
#define HPET_COUNTER        0x0f0

static uint64_t tsc_freq = ARCH_TIMER_FREQ;
static uint64_t lapic_freq[CONFIG_NR_CPUS];
static volatile unsigned tsc_freq_state;
static uint64_t hpet_freq;
static uintptr_t hpet_base;
static unsigned use_hpet;
/*
 * Per-CPU high-water mark.  The mark only exists to absorb a counter read that
 * lands behind an earlier one on this CPU, so keeping one per CPU lets each core
 * take it uncontended instead of serialising every reader in the system on a
 * single cache line.
 */
static volatile uint64_t last_ticks[CONFIG_NR_CPUS];

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                  uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__("cpuid"
                         : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                         : "a"(leaf), "c"(subleaf));
}

static uint64_t scale_ticks(uint64_t value, uint64_t from, uint64_t to) {
    uint64_t a = from;
    uint64_t b = to;
    while (b) {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    from /= a;
    to /= a;

    uint64_t whole = value / from;
    uint64_t fraction = (value % from) * to / from;
    uint64_t max = ~0ULL;

    if (whole > max / to)
        return max;
    whole *= to;
    if (fraction > max - whole)
        return max;
    return whole + fraction;
}

/*
 * Reciprocal of the reduced (source_freq, ARCH_TIMER_FREQ) ratio, so the hot
 * path multiplies by it rather than dividing by the source frequency.  The
 * magic is floor(2^64 * ARCH_TIMER_FREQ / source_freq), split into two words
 * because ARCH_TIMER_FREQ may exceed the source rate and push it past 64 bits.
 *
 * Dropping the remainder shortens each conversion by at most one tick and never
 * moves it backwards, so the result stays monotonic and the error stays bounded
 * by the tick period instead of accumulating with uptime.
 */
static uint64_t clk_scale_magic_lo = 0;
static uint64_t clk_scale_magic_hi = 1;

/* Scale using the precomputed reciprocal, with no per-call division. */
static inline uint64_t scale_ticks_reduced(uint64_t value) {
    uint64_t product_lo, product_hi;

    /* floor(value * magic / 2^64) needs only the high half of the product with
     * the low magic word plus a low multiply by the high word: everything below
     * bit 64 of the full product is shifted out.  Two multiplies replace two
     * 64-bit divisions, which cannot overlap with the surrounding loads. */
    __asm__ __volatile__("mulq %3"
                         : "=a"(product_lo), "=d"(product_hi)
                         : "a"(value), "r"(clk_scale_magic_lo));
    return product_hi + value * clk_scale_magic_hi;
}

/* floor(numerator << 64 / denominator), for numerator < denominator. */
static uint64_t recip_shift64(uint64_t numerator, uint64_t denominator) {
    __uint128_t remainder = 0;
    uint64_t quotient = 0;

    /* Long division over the 128-bit dividend numerator:64.  The kernel links
     * no 128-bit divide helper, and calibration runs once per clock source. */
    for (int bit = 127; bit >= 0; bit--) {
        uint64_t input = (bit >= 64) ? ((numerator >> (bit - 64)) & 1U) : 0U;

        remainder = (remainder << 1) | input;
        quotient <<= 1;
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= 1U;
        }
    }
    return quotient;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a ? a : 1;
}

static void timer_update_clk_scale(void) {
    uint64_t src = use_hpet ? hpet_freq : tsc_freq;
    if (!src)
        src = ARCH_TIMER_FREQ;
    uint64_t g = gcd_u64(src, ARCH_TIMER_FREQ);
    uint64_t from = src / g;
    uint64_t to = ARCH_TIMER_FREQ / g;

    /* 2^64 * to / from splits at the word boundary into the whole ratio and the
     * fraction shifted up by a word. */
    clk_scale_magic_hi = to / from;
    clk_scale_magic_lo = recip_shift64(to % from, from);
}

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t hpet_read(uint32_t reg) {
    return *(volatile uint64_t *)(hpet_base + reg);
}

static void hpet_write(uint32_t reg, uint64_t value) {
    *(volatile uint64_t *)(hpet_base + reg) = value;
}

static int enable_hpet(void) {
    hpet_base = firmware_acpi_hpet_address();
    if (!hpet_base)
        return 0;
    uint64_t capabilities = hpet_read(HPET_CAPABILITIES);
    uint64_t period_fs = capabilities >> 32;
    if (!period_fs || period_fs > 1000000000ULL)
        return 0;
    hpet_freq = 1000000000000000ULL / period_fs;
    if (!hpet_freq)
        return 0;
    hpet_write(HPET_CONFIGURATION,
               hpet_read(HPET_CONFIGURATION) | 1ULL);
    return 1;
}

static int has_invariant_tsc(void) {
    uint32_t max_ext, ebx, ecx, edx;
    cpuid(0x80000000U, 0, &max_ext, &ebx, &ecx, &edx);
    if (max_ext < 0x80000007U)
        return 0;
    cpuid(0x80000007U, 0, &max_ext, &ebx, &ecx, &edx);
    return !!(edx & (1U << 8));
}

/*
 * KVM keeps the guest TSC stable even when it does not advertise the
 * invariant-TSC bit (CPUID 0x80000007:EDX[8]) for the virtual CPU model.
 * Skipping the HPET there avoids the slow MMIO read and the frequency
 * rounding error that made the x86_64 clock run measurably behind real time.
 */
static int running_under_kvm(void) {
    uint32_t max, ebx, ecx, edx;
    cpuid(0x40000000U, 0, &max, &ebx, &ecx, &edx);
    return max >= 0x40000000U && ebx == 0x4b4d564bU;
}

static uint64_t calibrate_tsc_with_pit(void) {
    uint8_t speaker = inb(0x61);
    outb(0x61, speaker & ~3U);
    outb(0x43, 0xb0); /* PIT channel 2, one-shot, low byte then high byte. */
    outb(0x42, PIT_CALIBRATION_COUNT & 0xff);
    outb(0x42, PIT_CALIBRATION_COUNT >> 8);

    uint64_t start = read_tsc();
    outb(0x61, (speaker & ~2U) | 1U);
    unsigned timeout = 100000000U;
    while (!(inb(0x61) & 0x20) && --timeout)
        cpu_relax();
    uint64_t elapsed = read_tsc() - start;
    outb(0x61, speaker);
    if (!timeout || !elapsed)
        return 0;

    uint64_t freq = scale_ticks(elapsed, PIT_CALIBRATION_COUNT, PIT_FREQ);
    freq = (freq + 500000ULL) / 1000000ULL * 1000000ULL;
    return freq >= TSC_FREQ_MIN && freq <= TSC_CALIBRATED_MAX ? freq : 0;
}

static uint64_t discover_tsc_freq(void) {
    uint32_t max_leaf, ebx, ecx, edx;
    uint32_t denominator, numerator, crystal;
    uint64_t product;
    uint64_t cpuid_freq = 0;

    cpuid(0, 0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf >= 0x15) {
        cpuid(0x15, 0, &denominator, &numerator, &crystal, &edx);
        if (denominator && numerator && crystal &&
            !__builtin_mul_overflow((uint64_t)crystal,
                                    (uint64_t)numerator, &product)) {
            uint64_t freq = product / denominator;
            if (freq >= TSC_FREQ_MIN && freq <= TSC_FREQ_MAX)
                return freq;
        }
    }

    if (max_leaf >= 0x16) {
        uint32_t base_mhz;
        cpuid(0x16, 0, &base_mhz, &ebx, &ecx, &edx);
        uint64_t freq = (uint64_t)base_mhz * 1000000ULL;
        if (freq >= TSC_FREQ_MIN && freq <= TSC_FREQ_MAX)
            cpuid_freq = freq;
    }
    uint64_t calibrated = calibrate_tsc_with_pit();
    if (calibrated) {
        uint64_t difference = calibrated > cpuid_freq
            ? calibrated - cpuid_freq : cpuid_freq - calibrated;
        if (!cpuid_freq || difference > cpuid_freq / 20) {
            return calibrated;
        }
    }
    return cpuid_freq ? cpuid_freq : ARCH_TIMER_FREQ;
}

static void ensure_tsc_freq(void) {
    if (__atomic_load_n(&tsc_freq_state, __ATOMIC_ACQUIRE) == 2)
        return;

    unsigned expected = 0;
    if (__atomic_compare_exchange_n(&tsc_freq_state, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        tsc_freq = discover_tsc_freq();
        if (!has_invariant_tsc() && !running_under_kvm() && enable_hpet())
            use_hpet = 1;
        timer_update_clk_scale();
        __atomic_store_n(&tsc_freq_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&tsc_freq_state, __ATOMIC_ACQUIRE) != 2)
        cpu_relax();
}

static void calibrate_lapic(void) {
    unsigned cpu = arch_current_cpu_id();
    lapic_freq[cpu] = LAPIC_FREQ_FALLBACK;
    lapic_write(LAPIC_TIMER_DIV, 0x3); /* Divide the LAPIC bus clock by 16. */
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER | LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xffffffffU);

    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < LAPIC_CALIBRATION_TICKS)
        cpu_relax();
    uint64_t elapsed = timer_get_ticks() - start;
    uint32_t count = 0xffffffffU - lapic_read(LAPIC_TIMER_CUR);
    uint64_t measured = count && elapsed
        ? scale_ticks(count, elapsed, ARCH_TIMER_FREQ) : 0;
    if (measured >= LAPIC_FREQ_MIN && measured <= TSC_FREQ_MAX)
        lapic_freq[cpu] = measured;
}

void timer_init(void) {
    ensure_tsc_freq();
    calibrate_lapic();
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER | LAPIC_LVT_MASKED);
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    uint64_t count = scale_ticks(ticks, ARCH_TIMER_FREQ,
                                 lapic_freq[arch_current_cpu_id()]);
    if (count == 0) count = 1;
    if (count > 0xffffffffU) count = 0xffffffffU;
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)count);
}

uint64_t timer_get_ticks(void) {
    ensure_tsc_freq();
    uint64_t ticks = scale_ticks_reduced(use_hpet ? hpet_read(HPET_COUNTER)
                                                  : read_tsc());
    volatile uint64_t *last = &last_ticks[cpu_current_id()];

    uint64_t previous = __atomic_load_n(last, __ATOMIC_RELAXED);
    while (ticks > previous &&
           !__atomic_compare_exchange_n(last, &previous, ticks, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    return ticks > previous ? ticks : previous;
}

void timer_irq_tick(void) {}

void timer_enable(void) {
    lapic_write(LAPIC_LVT_TIMER,
        (lapic_read(LAPIC_LVT_TIMER) & ~LAPIC_LVT_MASKED) | IRQ_VECTOR_TIMER);
}

void timer_disable(void) {
    lapic_write(LAPIC_LVT_TIMER, lapic_read(LAPIC_LVT_TIMER) | LAPIC_LVT_MASKED);
}

uint64_t arch_vdso_counter(void)
{
    return read_tsc();
}

uint64_t arch_vdso_counter_freq(void)
{
    ensure_tsc_freq();
    /* When HPET is the time source user space cannot read it; report no
     * usable counter so the vDSO stays off rather than disagreeing with the
     * syscall path. */
    if (use_hpet)
        return 0;
    return tsc_freq ? tsc_freq : ARCH_TIMER_FREQ;
}

#endif

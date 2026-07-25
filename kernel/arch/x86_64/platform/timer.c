#ifdef CONFIG_X86_64

#include "core/defs.h"
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
static volatile uint64_t last_ticks;

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
        if (!has_invariant_tsc() && enable_hpet())
            use_hpet = 1;
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
    uint64_t ticks;
    if (use_hpet)
        ticks = scale_ticks(hpet_read(HPET_COUNTER), hpet_freq, ARCH_TIMER_FREQ);
    else
        ticks = scale_ticks(read_tsc(), tsc_freq, ARCH_TIMER_FREQ);

    uint64_t previous = __atomic_load_n(&last_ticks, __ATOMIC_RELAXED);
    while (ticks > previous &&
           !__atomic_compare_exchange_n(&last_ticks, &previous, ticks, 1,
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

#endif

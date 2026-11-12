#ifdef CONFIG_X86_64

#include "core/defs.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/klog.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "cpu.h"
#include "platform.h"
#include "firmware.h"
#include "tsc_baytrail.h"

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
static unsigned hpet_counter_64;
static uint64_t hpet_extended_counter;
struct cpu_clock_anchor {
    uint64_t cycle, ticks, rate;
    unsigned ready, verified;
} __attribute__((aligned(64)));
static struct cpu_clock_anchor cpu_clock[CONFIG_NR_CPUS];
static uint64_t hpet_clock_highwater;
static unsigned baytrail_tsc_known;
/*
 * Per-CPU high-water mark. Each CPU owns a distinct cache line so its
 * monotonicity CAS does not invalidate another CPU's slot. With two CPUs this
 * storage occupies 128 bytes. The shared-SMP domain clock below is unchanged.
 */
#ifdef CONFIG_QEMU_SHARED_SMP
/* Each KVM VM has its own TSC epoch. CPU 0 owns the domain clock; its timer
 * IRQ publishes at least once per programmed 1 ms interval. Remote CPUs
 * consume this clock without adding a VM-local counter. A stalled owner
 * stalls domain time; independent owner failover is not supported. */
static volatile uint64_t shared_clock_ticks;
#else
struct timer_tick_slot {
    volatile uint64_t ticks;
    uint8_t padding[64 - sizeof(uint64_t)];
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct timer_tick_slot) == 64,
               "per-CPU timer high-water slot must occupy one cache line");
static struct timer_tick_slot last_ticks[CONFIG_NR_CPUS];
#endif

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                  uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__("cpuid"
                         : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                         : "a"(leaf), "c"(subleaf));
}

static void tsc_probe_cpuid(void *opaque, uint32_t leaf, uint32_t *eax,
                            uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    (void)opaque;
    cpuid(leaf, 0, eax, ebx, ecx, edx);
}

static uint64_t tsc_probe_msr(void *opaque, uint32_t msr) {
    (void)opaque;
    return rdmsr(msr);
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

static uint64_t read_hpet_clock(void) {
    if (hpet_counter_64)
        return hpet_read(HPET_COUNTER);
    /* Never serialize slow MMIO behind an IRQ-disabled global spinlock:
     * simultaneous timer IRQs otherwise spend their time waiting on it.
     * Publish one sample optimistically. A competing publisher wins; its
     * newer value is a valid sample for this call too, so no retry is needed.
     * Maintenance deadlines below guarantee a sample each half-wrap. */
    uint64_t previous = __atomic_load_n(&hpet_extended_counter, __ATOMIC_ACQUIRE);
    uint32_t low = (uint32_t)hpet_read(HPET_COUNTER);
    uint32_t delta = low - (uint32_t)previous;
    if (delta > 0x7fffffffU)
        return __atomic_load_n(&hpet_extended_counter, __ATOMIC_ACQUIRE);
    uint64_t next = previous + delta;
    if (__atomic_compare_exchange_n(&hpet_extended_counter, &previous, next, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return next;
    return previous;
}

static uint64_t clock_maintenance_interval(uint64_t ticks) {
    if (use_hpet && !hpet_counter_64 && hpet_freq) {
        /* Sample at least twice per wrap even in tickless idle. */
        uint64_t budget = ((uint64_t)ARCH_TIMER_FREQ << 31) / hpet_freq;
        if (ticks > budget)
            ticks = budget;
    }
    return ticks;
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
    hpet_counter_64 = !!(capabilities & (1ULL << 13));
    hpet_extended_counter = (uint32_t)hpet_read(HPET_COUNTER);
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

/* TSC epochs may differ even with invariant frequency. Each CPU correlates
 * its own counter with the common HPET domain, never another CPU's TSC. The
 * local IRQ mask prevents migration or an anchor update between CPU lookup,
 * RDTSC and the anchor snapshot. No global lock or MMIO is on the fast path. */
static uint64_t read_reference_clock(void) {
    int restore_irqs = arch_irqs_enabled();
    arch_local_irq_disable();
    unsigned cpu = cpu_current_id();
    uint64_t ticks;
    struct cpu_clock_anchor *anchor = &cpu_clock[cpu];
    uint64_t cycle = read_tsc();
    if (anchor->ready && anchor->verified && cycle >= anchor->cycle &&
        cycle - anchor->cycle <= anchor->rate * 2) {
        ticks = anchor->ticks + scale_ticks(cycle - anchor->cycle,
                                           anchor->rate, ARCH_TIMER_FREQ);
    } else {
        ticks = scale_ticks_reduced(read_hpet_clock());
    }
    if (restore_irqs)
        arch_local_irq_enable();
    return ticks;
}

static void refresh_cpu_clock(int initialize) {
    unsigned cpu = cpu_current_id();
    struct cpu_clock_anchor *anchor = &cpu_clock[cpu];
    if (!initialize && !anchor->ready)
        return;
    uint64_t cycle = read_tsc();
    if (!initialize && cycle >= anchor->cycle &&
        cycle - anchor->cycle < anchor->rate)
        return;

    uint64_t best_span = ~0ULL, best_cycle = 0, best_ticks = 0;
    for (unsigned i = 0; i < 5; i++) {
        uint64_t before = read_tsc();
        uint64_t reference = scale_ticks_reduced(read_hpet_clock());
        uint64_t after = read_tsc();
        if (after >= before && after - before < best_span) {
            best_span = after - before;
            best_cycle = before + best_span / 2;
            best_ticks = reference;
        }
    }
    /* A preempted vCPU or an SMI is not a useful calibration sample. */
    if (best_span > tsc_freq / 1000)
        return;
    uint64_t rate = tsc_freq;
    if (!initialize) {
        uint64_t predicted = best_cycle >= anchor->cycle
            ? anchor->ticks + scale_ticks(best_cycle - anchor->cycle,
                                          anchor->rate, ARCH_TIMER_FREQ) : 0;
        uint64_t error = predicted > best_ticks ? predicted - best_ticks
                                               : best_ticks - predicted;
        uint64_t elapsed = best_ticks > anchor->ticks ? best_ticks - anchor->ticks : 0;
        uint64_t tolerance = MS_TO_TICKS(5) + elapsed / (anchor->verified ? 2000 : 200) +
            scale_ticks(best_span, tsc_freq, ARCH_TIMER_FREQ);
        if (error > tolerance) {
            anchor->ready = 0;
            klog_record_only(KLOG_WARN,
                "[TIME] cpu=%u invariant TSC validation failed error_ticks=%llu; HPET fallback\n",
                cpu, (unsigned long long)error);
            return;
        }
        /* Refine the PIT/CPUID frequency estimate against the reference over
         * a full second. This corrects small source-rate calibration errors,
         * not epoch jumps; those were rejected above. */
        rate = anchor->rate;
        if (best_ticks > anchor->ticks && best_cycle >= anchor->cycle) {
            uint64_t measured = scale_ticks(best_cycle - anchor->cycle,
                                            best_ticks - anchor->ticks,
                                            ARCH_TIMER_FREQ);
            uint64_t difference = measured > rate ? measured - rate : rate - measured;
            if (difference <= rate / 200)
                rate = measured;
        }
    }
    anchor->cycle = best_cycle;
    anchor->ticks = best_ticks;
    anchor->rate = rate;
    anchor->ready = 1;
    if (initialize) {
        anchor->verified = 0;
    } else if (!anchor->verified) {
        anchor->verified = 1;
        klog_record_only(KLOG_INFO,
            "[TIME] cpu=%u normalized TSC=1 rate=%llu; HPET reference verified\n",
            cpu, (unsigned long long)rate);
    }
}

/* Invariant rate does not imply a common epoch on different CPUs. Until
 * synchronization or a real pvclock implementation validates that epoch,
 * SMP uses the shared HPET where available, including under KVM. */
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

    const struct x86_tsc_probe_ops atom_ops = {
        .cpuid = tsc_probe_cpuid, .read_msr = tsc_probe_msr,
    };
    uint64_t atom_hz = x86_baytrail_tsc_hz(&atom_ops);
    if (atom_hz) {
        /* Bay Trail's PIT/HPET may be absent or nonfunctional. Its TSC and
         * non-turbo ratio are fixed by the SoC even when CPUID omits the
         * invariant-TSC flag; do not replace it with an unusable HPET. */
        baytrail_tsc_known = 1;
        return atom_hz;
    }

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
        if (!baytrail_tsc_known &&
            (CONFIG_NR_CPUS > 1 || !has_invariant_tsc()) && enable_hpet())
            use_hpet = 1;
        timer_update_clk_scale();
        __atomic_store_n(&tsc_freq_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&tsc_freq_state, __ATOMIC_ACQUIRE) != 2)
        cpu_relax();
}

static uint64_t calibration_ticks(void) {
#ifdef CONFIG_QEMU_SHARED_SMP
    /* AP calibration precedes online publication, while the boot CPU may
     * wait with IRQs disabled. Measure this local device with its own TSC;
     * this counter must never escape into shared scheduler deadlines. */
    return scale_ticks(read_tsc(), tsc_freq, ARCH_TIMER_FREQ);
#else
    return timer_get_ticks();
#endif
}

static void calibrate_lapic(void) {
    unsigned cpu = arch_current_cpu_id();
    lapic_freq[cpu] = LAPIC_FREQ_FALLBACK;
    lapic_write(LAPIC_TIMER_DIV, 0x3); /* Divide the LAPIC bus clock by 16. */
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER | LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xffffffffU);

    uint64_t start = calibration_ticks();
    while (calibration_ticks() - start < LAPIC_CALIBRATION_TICKS)
        cpu_relax();
    uint64_t elapsed = calibration_ticks() - start;
    uint32_t count = 0xffffffffU - lapic_read(LAPIC_TIMER_CUR);
    uint64_t measured = count && elapsed
        ? scale_ticks(count, elapsed, ARCH_TIMER_FREQ) : 0;
    if (measured >= LAPIC_FREQ_MIN && measured <= TSC_FREQ_MAX)
        lapic_freq[cpu] = measured;
}

void timer_init(void) {
    ensure_tsc_freq();
    if (arch_current_cpu_id() == 0 && use_hpet && hpet_counter_64) {
        /* Exercise real wrap handling on 64-bit emulators too. Preserve the
         * hardware epoch so this diagnostic option never rewinds early time. */
        const char *args = bootargs_get();
        const char *force = args ? strstr(args, "a20.hpet32=1") : NULL;
        if (force && (force == args || force[-1] == ' ') &&
            (!force[12] || force[12] == ' ')) {
            hpet_extended_counter = hpet_read(HPET_COUNTER);
            hpet_counter_64 = 0;
        }
    }
    if (arch_current_cpu_id() == 0)
        kinfo("[TIME] clocksource=%s frequency=%llu counter_bits=%u TSC-invariant=%d; %s\n",
              use_hpet ? "HPET" : "TSC",
              (unsigned long long)(use_hpet ? hpet_freq : tsc_freq),
              use_hpet && !hpet_counter_64 ? 32U : 64U,
              has_invariant_tsc(), use_hpet ? "user TSC vDSO disabled" : "TSC counter selected");
    calibrate_lapic();
    if (use_hpet && has_invariant_tsc()) {
        refresh_cpu_clock(1);
        kinfo("[TIME] cpu=%u TSC anchor initialized=%u; verification pending; user TSC vDSO disabled\n",
              arch_current_cpu_id(), cpu_clock[arch_current_cpu_id()].ready);
    }
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER | LAPIC_LVT_MASKED);
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    ticks = clock_maintenance_interval(ticks);
#ifdef CONFIG_QEMU_SHARED_SMP
    /* Each unmodified QEMU owns its LAPIC. Poll shared IPI intents even when
     * this kernel CPU is idle; default platform keeps its tickless path. */
    if (ticks > ARCH_TIMER_FREQ / 1000)
        ticks = ARCH_TIMER_FREQ / 1000;
#endif
    uint64_t count = scale_ticks(ticks, ARCH_TIMER_FREQ,
                                 lapic_freq[arch_current_cpu_id()]);
    if (count == 0) count = 1;
    if (count > 0xffffffffU) count = 0xffffffffU;
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)count);
}

/* Bound a hypervisor activation even when tickless idle left a long or
 * exhausted one-shot. Preserve an earlier deadline across frequent exits. */
void x86_64_timer_ensure_deadline(uint64_t ticks) {
    ticks = clock_maintenance_interval(ticks);
    uint64_t count = scale_ticks(ticks, ARCH_TIMER_FREQ,
                                lapic_freq[arch_current_cpu_id()]);
    if (!count) count = 1;
    if (count > 0xffffffffU) count = 0xffffffffU;
    uint32_t current = lapic_read(LAPIC_TIMER_CUR);
    if (!current || current > count)
        lapic_write(LAPIC_TIMER_INIT, (uint32_t)count);
    timer_enable();
}

uint64_t timer_get_ticks(void) {
#ifdef CONFIG_QEMU_SHARED_SMP
    if (cpu_current_id() != 0)
        return __atomic_load_n(&shared_clock_ticks, __ATOMIC_ACQUIRE);
#endif
    ensure_tsc_freq();
    uint64_t ticks = use_hpet ? read_reference_clock()
                             : scale_ticks_reduced(read_tsc());
#ifdef CONFIG_QEMU_SHARED_SMP
    volatile uint64_t *last = &shared_clock_ticks;
#else
    volatile uint64_t *last = use_hpet ? &hpet_clock_highwater
                                     : &last_ticks[cpu_current_id()].ticks;
#endif

    uint64_t previous = __atomic_load_n(last, __ATOMIC_RELAXED);
    while (ticks > previous &&
           !__atomic_compare_exchange_n(last, &previous, ticks, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    return ticks > previous ? ticks : previous;
}

void timer_irq_tick(void) {
    if (use_hpet)
        refresh_cpu_clock(0);
}

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
#ifdef CONFIG_QEMU_SHARED_SMP
    /* User RDTSC would reintroduce the unrelated per-VM clock epochs. */
    return 0;
#else
    ensure_tsc_freq();
    /* When HPET is the time source user space cannot read it; report no
     * usable counter so the vDSO stays off rather than disagreeing with the
     * syscall path. */
    if (use_hpet)
        return 0;
    return tsc_freq ? tsc_freq : ARCH_TIMER_FREQ;
#endif
}

#endif

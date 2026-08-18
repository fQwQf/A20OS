#ifdef CONFIG_RISCV64

#include "core/defs.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "firmware.h"
#include "platform.h"

static uint64_t timer_freq;
static int timer_has_sstc;
static int timer_backend_ready;

/* Runtime timebase frequency: the firmware DTB (/cpus timebase-frequency)
 * is authoritative (QEMU virt publishes 10 MHz, JH7110 24 MHz); fall back to
 * the arch constant when no usable DTB was handed over.  The result is
 * cached so hot paths (TICKS_PER_SEC conversions) stay a single load. */
uint64_t riscv64_timer_freq(void) {
    uint64_t freq = __atomic_load_n(&timer_freq, __ATOMIC_ACQUIRE);
    if (freq)
        return freq;
    freq = riscv64_fdt_timebase_freq();
    if (!freq)
        freq = ARCH_TIMER_FREQ;
    __atomic_store_n(&timer_freq, freq, __ATOMIC_RELEASE);
    return freq;
}

void timer_init(void) {
    if (!__atomic_load_n(&timer_backend_ready, __ATOMIC_ACQUIRE)) {
        timer_has_sstc = riscv64_fdt_has_isa_extension("sstc");
        __atomic_store_n(&timer_backend_ready, 1, __ATOMIC_RELEASE);
        printf("[TIMER] backend=%s freq=%llu Hz\n",
               timer_has_sstc ? "sstc" : "sbi",
               (unsigned long long)riscv64_timer_freq());
    }
    timer_set_interval(riscv64_timer_freq() / 100);
    /* Allow U-mode to read the time CSR directly; the vDSO time path
     * (kernel/mm/vdso.c) depends on this on every hart. */
    uint64_t scounteren;
    __asm__ volatile("csrr %0, scounteren" : "=r"(scounteren));
    scounteren |= 0x2; /* TM */
    __asm__ volatile("csrw scounteren, %0" :: "r"(scounteren));
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    uint64_t now = timer_get_ticks();
    uint64_t deadline = now + ticks;
    if (__atomic_load_n(&timer_has_sstc, __ATOMIC_RELAXED)) {
        /* Sstc exposes the supervisor timer comparator directly.  Numeric CSR
         * syntax keeps the build independent of assembler extension names. */
        __asm__ volatile("csrw 0x14d, %0" :: "r"(deadline) : "memory");
    } else {
        firmware_set_timer(deadline);
    }
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

void timer_irq_tick(void) {
    /* RISC-V already uses the monotonic hardware time CSR. */
}

void timer_enable(void) {
    uint64_t sie = arch_read_sie();
    arch_write_sie(sie | SIE_STIE);
}

void timer_disable(void) {
    uint64_t sie = arch_read_sie();
    arch_write_sie(sie & ~SIE_STIE);
}

#endif /* CONFIG_RISCV64 */

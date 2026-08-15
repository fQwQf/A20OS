#ifdef CONFIG_LOONGARCH32

#include "core/defs.h"
#include "core/timer.h"
#include "firmware.h"

/*
 * The NaiLoong Core exposes the stable 64-bit cycle counter through the
 * rdcntvl.w / rdcntvh.w instructions (CNTVL/CNTVH CSRs).  Read high first,
 * then low, so a carry between the two halves cannot be mis-observed.
 */
static uint64_t stable_counter_read(void) {
    uint32_t hi, lo;
    __asm__ __volatile__(
        "rdtimeh.w %0, $zero\n"
        "rdtimel.w %1, $zero\n"
        : "=r"(hi), "=r"(lo));
    return ((uint64_t)hi << 32) | lo;
}

void timer_init(void) {
    timer_set_interval(TICKS_PER_SEC / 100);
}

void timer_set_interval(uint64_t ticks) {
    /* Clear pending timer interrupt (TICLR, CSR 0x44, bit 0) */
    uint32_t clr = 1;
    __asm__ __volatile__("csrwr %0, 0x44" :: "r"(clr));
    /* Set next timer: TCFG = (initval << 2) | En=1 | Periodic=0 */
    uint32_t cfg = ((uint32_t)ticks << 2) | 0x1;
    __asm__ __volatile__("csrwr %0, 0x41" :: "r"(cfg));
}

uint64_t timer_get_ticks(void) {
    return stable_counter_read();
}

void timer_irq_tick(void) {
    /* Uses the monotonic rdcntvl/rdcntvh counter directly. */
}

void timer_enable(void) {
    uint32_t crmd;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(crmd));
    crmd |= (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x0" :: "r"(crmd));
}

void timer_disable(void) {
    uint32_t crmd;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(crmd));
    crmd &= ~(1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x0" :: "r"(crmd));
}

#endif /* CONFIG_LOONGARCH32 */

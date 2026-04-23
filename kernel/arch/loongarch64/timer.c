#ifdef CONFIG_LOONGARCH64

#include "defs.h"
#include "timer.h"
#include "firmware.h"

void timer_init(void) {
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    /* Clear pending timer interrupt (TICLR, CSR 0x44, bit 0) */
    uint64_t clr = 1;
    __asm__ __volatile("csrwr %0, 0x44" :: "r"(clr));
    /* Set next timer: TCFG = (initval << 2) | En=1 | Periodic=0 */
    uint64_t cfg = (ticks << 2) | 0x1;
    __asm__ __volatile("csrwr %0, 0x41" :: "r"(cfg));
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ __volatile("csrrd %0, 0x42" : "=r"(val));
    return val;
}

void timer_enable(void) {
    uint64_t crmd;
    __asm__ __volatile("csrrd %0, 0x0" : "=r"(crmd));
    crmd |= (1UL << 2);
    __asm__ __volatile("csrwr %0, 0x0" :: "r"(crmd));
}

void timer_disable(void) {
    uint64_t crmd;
    __asm__ __volatile("csrrd %0, 0x0" : "=r"(crmd));
    crmd &= ~(1UL << 2);
    __asm__ __volatile("csrwr %0, 0x0" :: "r"(crmd));
}

#endif /* CONFIG_LOONGARCH64 */

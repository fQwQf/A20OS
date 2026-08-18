#ifdef CONFIG_LOONGARCH64

#include "core/defs.h"
#include "core/timer.h"
#include "firmware.h"

#define LA64_CSR_CRMD_IE       (1UL << 2)
#define LA64_CSR_ECFG_TI       (1UL << 11)
#define LA64_TCFG_EN           (1UL << 0)
#define LA64_TCFG_INITVAL_MAX  ((1UL << 46) - 1)

static inline uint64_t timer_read_crmd(void) {
    uint64_t value;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(value));
    return value;
}

static inline void timer_write_crmd(uint64_t value) {
    __asm__ __volatile__("csrwr %0, 0x0"
                         : "+r"(value)
                         :
                         : "memory");
}

static inline uint64_t timer_read_ecfg(void) {
    uint64_t value;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(value));
    return value;
}

static inline void timer_write_ecfg(uint64_t value) {
    __asm__ __volatile__("csrwr %0, 0x4"
                         : "+r"(value)
                         :
                         : "memory");
}

static inline void timer_write_tcfg(uint64_t value) {
    __asm__ __volatile__("csrwr %0, 0x41"
                         : "+r"(value)
                         :
                         : "memory");
}

static inline void timer_clear_irq(void) {
    uint64_t value = 1;
    __asm__ __volatile__("csrwr %0, 0x44"
                         : "+r"(value)
                         :
                         : "memory");
}

void timer_init(void) {
    timer_set_interval(TICKS_PER_SEC / 100);
}

void timer_set_interval(uint64_t ticks) {
    if (ticks == 0)
        ticks = 1;
    if (ticks > LA64_TCFG_INITVAL_MAX)
        ticks = LA64_TCFG_INITVAL_MAX;

    /* TCFG.InitVal occupies bits 47:2. Keep Periodic clear so each handler
     * chooses the next scheduler/wakeup deadline before rearming. */
    timer_clear_irq();
    timer_write_tcfg((ticks << 2) | LA64_TCFG_EN);
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ __volatile__("rdtime.d %0, $zero" : "=r"(val));
    return val;
}

void timer_irq_tick(void) {
    /* LoongArch uses the monotonic rdtime.d counter directly. */
}

void timer_enable(void) {
    /* The one-shot programmed by timer_init() can expire while early boot
     * keeps CRMD.IE clear.  Clear that stale pending state and arm a fresh
     * first tick before interrupts become observable. Mask TI while changing
     * TCFG so this ordering also remains safe if a later caller has IE set. */
    uint64_t ecfg = timer_read_ecfg();
    timer_write_ecfg(ecfg & ~LA64_CSR_ECFG_TI);
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_write_ecfg(ecfg | LA64_CSR_ECFG_TI);

    timer_write_crmd(timer_read_crmd() | LA64_CSR_CRMD_IE);
}

void timer_disable(void) {
    timer_write_ecfg(timer_read_ecfg() & ~LA64_CSR_ECFG_TI);
    timer_write_tcfg(0);
    timer_clear_irq();
}

#endif /* CONFIG_LOONGARCH64 */

#ifdef CONFIG_LOONGARCH64

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"

extern void trap_entry_la64(void);
extern void loongarch64_smp_handle_ipi(int from_user);

static void handle_timer_irq(int from_user) {
    __asm__ __volatile__("csrwr %0, 0x44" :: "r"(1UL) : "memory");
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
    if (!from_user) return;

    task_t *cur = proc_current();
    if (cur) cur->total_time++;
    proc_yield();
}

void trap_init(void) {
    arch_write_tvec((uint64_t)trap_entry_la64);

    /*
     * ECFG (CSR 0x4):
     *   IS[11] = timer interrupt
     *   IS[12] = IOCSR inter-processor interrupt
     *   IS[2]  = HWI0 used by QEMU virt PCIe / virtio
     */
    uint64_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 12) | (1UL << 11) | (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
}

void arch_handle_irq(uint64_t irq, int from_user) {
    if (irq == IRQ_S_IPI) {
        loongarch64_smp_handle_ipi(from_user);
        return;
    }

    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    if (irq == IRQ_S_EXT)
        driver_irq_dispatch((uint32_t)irq);
}

#endif

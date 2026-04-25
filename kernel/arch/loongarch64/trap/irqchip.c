#ifdef CONFIG_LOONGARCH64

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drv/uart.h"

extern void trap_entry_la64(void);

static void handle_timer_irq(int from_user) {
    __asm__ __volatile__("csrwr %0, 0x44" :: "r"(1UL) : "memory");
    timer_irq_tick();
    timer_set_interval(TICKS_PER_SEC / 100);
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
     *   IS[2]  = HWI0 used by QEMU virt PCIe / virtio
     */
    uint64_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 11) | (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
}

void arch_handle_irq(uint64_t irq, int from_user) {
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    if (irq == IRQ_S_EXT)
        uart_handle_irq();
}

#endif

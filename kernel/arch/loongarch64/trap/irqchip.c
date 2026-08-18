#ifdef CONFIG_LOONGARCH64

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"

extern void trap_entry_la64(void);
extern void loongarch64_smp_handle_ipi(int from_user);
extern void la64_handle_device_irq(void);
extern void la64_eiointc_pic_init(void);
#ifdef CONFIG_BOARD_LS2K1000
extern void ls2k1000_handle_device_irq(void);
#endif

static void handle_timer_irq(int from_user) {
    uint64_t ticlr = 1;
    __asm__ __volatile__("csrwr %0, 0x44"
                         : "+r"(ticlr)
                         :
                         : "memory");
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
    proc_sched_tick(from_user);
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
    /* EENTRY points at one common entry stub, so interrupts must use the
     * non-vectored layout regardless of the firmware's ECFG.VS setting. */
    ecfg &= ~(0x7UL << 16);
#ifdef CONFIG_BOARD_LS2K1000
    /* Discard U-Boot's local-interrupt mask.  The cooperative fallback keeps
     * only timer/IPI; the Phase 2 image adds the LS2K HWI1 cascade below. */
    ecfg &= ~0x1FFFUL;
#endif
    ecfg |= (1UL << 12) | (1UL << 11);
#ifdef CONFIG_BOARD_LS2K1000
#ifndef CONFIG_COOPERATIVE_BOOT
    ecfg |= (1UL << LS2K_LIOINTC_PARENT_IRQ);
#endif
#else
    ecfg |= (1UL << 2);
#endif
    __asm__ __volatile__("csrwr %0, 0x4"
                         : "+r"(ecfg)
                         :
                         : "memory");

    /* Unmask the PCH-PIC PCI INTx lines and enable the EIOINTC inputs so the
     * QEMU virt I/O interrupt controllers can deliver device IRQs on HWI0. */
#ifndef CONFIG_BOARD_LS2K1000
    la64_eiointc_pic_init();
#endif
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

#ifdef CONFIG_BOARD_LS2K1000
    if (irq == LS2K_LIOINTC_PARENT_IRQ) {
        ls2k1000_handle_device_irq();
        return;
    }
#else
    if (irq == IRQ_S_EXT) {
        la64_handle_device_irq();
        return;
    }
#endif
}

#endif

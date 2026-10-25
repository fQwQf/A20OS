#include "core/arch.h"
#include "drivers/core/driver_core.h"
#include "core/panic.h"
#include "core/smp.h"

/* Select by GICR_TYPER affinity, never assume firmware frame ordering. */
uint64_t quartz64_zero_gicr_base(void) {
    static uint64_t selected;
    if (selected)
        return selected;
    uint64_t mpidr = arch_smp_boot_hw_id();
    uint32_t affinity = (uint32_t)(mpidr & 0xffffffU) |
                        (uint32_t)((mpidr >> 8) & 0xff000000U);
    for (uint64_t off = 0; off < 0x80000; ) {
        uint64_t base = PAGE_OFFSET + 0xfd460000UL + off;
        uint64_t typer = *(volatile uint64_t *)(uintptr_t)(base + 8);
        if ((uint32_t)(typer >> 32) == affinity) {
            selected = base;
            return base;
        }
        if (typer & (1U << 4))
            break;
        off += typer & 2U ? 0x40000 : 0x20000;
    }
    panic("Quartz64-Zero: boot CPU has no GIC redistributor");
}

/* Trap initialization owns the GIC distributor and CPU interface. */
static void init(void) {}
static void set_irq(uint32_t irq, unsigned offset) {
    uintptr_t base = irq < 32U ? GICR_BASE + 0x10000 : GICD_BASE;
    *(volatile uint32_t *)(base + offset + irq / 32U * 4) = 1U << (irq % 32U);
    arch_mb();
}
static void enable(uint32_t irq) { set_irq(irq, 0x100); }
static void disable(uint32_t irq) { set_irq(irq, 0x180); }
static uint32_t ack(void) { return 0; } /* Exception entry already read IAR. */
static void eoi(uint32_t irq) {
    __asm__ __volatile__("msr icc_eoir1_el1, %0" :: "r"((uint64_t)irq) : "memory");
}
const irqchip_ops_t quartz64_zero_gic_ops = {
    .init = init, .enable_irq = enable, .disable_irq = disable, .ack = ack, .eoi = eoi,
};

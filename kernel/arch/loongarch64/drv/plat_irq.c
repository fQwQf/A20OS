#include "plat_irq.h"
#include "loongarch64_consts.h"
#include "arch_ops.h"
#include "types.h"




/* -------------------- MMIO helpers -------------------- */

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uint64_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline uint8_t mmio_read8(uint64_t addr) {
    return *(volatile uint8_t *)addr;
}

static inline void mmio_write8(uint64_t addr, uint8_t val) {
    *(volatile uint8_t *)addr = val;
}

/* -------------------- IOCSR helpers -------------------- */
/*
 * If you already have arch_iocsr_* helpers, replace these with your own.
 */
static inline uint32_t iocsr_read32(uint32_t reg)
{
    uint32_t val;
    asm volatile("iocsrrd.w %0, %1" : "=r"(val) : "r"(reg));
    return val;
}

static inline void iocsr_write32(uint32_t reg, uint32_t val)
{
    asm volatile("iocsrwr.w %0, %1" :: "r"(val), "r"(reg));
}

static inline uint8_t iocsr_read8(uint32_t reg)
{
    uint32_t val;
    asm volatile("iocsrrd.b %0, %1" : "=r"(val) : "r"(reg));
    return (uint8_t)val;
}

static inline void iocsr_write8(uint32_t reg, uint8_t val)
{
    asm volatile("iocsrwr.b %0, %1" :: "r"((uint32_t)val), "r"(reg));
}

/* -------------------- Local helpers -------------------- */

static inline int irq_hart_id(void)
{
    return 0; /* single core for now */
}

static inline void extioi_enable_irq(unsigned int irq)
{
    uint32_t reg = IOCSR_EXTIOI_EN_BASE + ((irq / 32U) * 4U);
    uint32_t bit = 1U << (irq % 32U);
    uint32_t val = iocsr_read32(reg);
    iocsr_write32(reg, val | bit);
}

static inline void extioi_disable_irq(unsigned int irq)
{
    uint32_t reg = IOCSR_EXTIOI_EN_BASE + ((irq / 32U) * 4U);
    uint32_t bit = 1U << (irq % 32U);
    uint32_t val = iocsr_read32(reg);
    iocsr_write32(reg, val & ~bit);
}

static inline void extioi_ack_irq(unsigned int irq)
{
    uint32_t reg = IOCSR_EXTIOI_ISR_BASE + ((irq / 32U) * 4U);
    uint32_t bit = 1U << (irq % 32U);
    iocsr_write32(reg, bit);
}

static inline void extioi_route_group_to_hwi0(unsigned int irq)
{
    uint32_t group = irq / 32U; /* 0..7 */
    iocsr_write8(IOCSR_EXTIOI_ROUTE_BASE + group, EXTIOI_ROUTE_TO_HWI0);
}

static inline void extioi_route_irq_to_core0(unsigned int irq)
{
    iocsr_write8(IOCSR_EXTIOI_COREMAP_BASE + irq, EXTIOI_ROUTE_CORE0);
}

static inline void ls7a_unmask_irq(unsigned int src_irq)
{
    uint32_t v = mmio_read32(LS7A_PCH_PIC_MASK);
    v |= (1U << src_irq);
    mmio_write32(LS7A_PCH_PIC_MASK, v);
}

static inline void ls7a_mask_irq(unsigned int src_irq)
{
    uint32_t v = mmio_read32(LS7A_PCH_PIC_MASK);
    v &= ~(1U << src_irq);
    mmio_write32(LS7A_PCH_PIC_MASK, v);
}

static inline void ls7a_set_irq_level_trigger(unsigned int src_irq)
{
    uint32_t v = mmio_read32(LS7A_PCH_PIC_TRIG);
    v &= ~(1U << src_irq);   /* assume 0 = level, 1 = edge */
    mmio_write32(LS7A_PCH_PIC_TRIG, v);
}

static inline void ls7a_route_src_to_extioi(unsigned int src_irq, unsigned int ext_irq)
{
    mmio_write8(LS7A_PCH_PIC_HTMSI_VEC + src_irq, (uint8_t)ext_irq);
}

/* -------------------- Public platform IRQ API -------------------- */

void plat_irq_init(void)
{
    /*
     * 1) LS7A PCH-PIC:
     *    - route UART source irq2 -> EXTIOI irq2
     *    - configure level trigger
     *    - unmask source
     *
     * 2) EXTIOI:
     *    - route irq group to HWI0 (default is already HWI0 on QEMU)
     *    - route irq2 to core0 (default is already node0/core0 on QEMU)
     *    - enable irq2
     *
     * 3) CPU local external interrupt source:
     *    done by arch_irq_enable_external()
     */
    ls7a_route_src_to_extioi(LS7A_UART_SRC_IRQ, EXTIOI_UART_IRQ);
    ls7a_set_irq_level_trigger(LS7A_UART_SRC_IRQ);
    ls7a_unmask_irq(LS7A_UART_SRC_IRQ);

    extioi_route_group_to_hwi0(EXTIOI_UART_IRQ);
    extioi_route_irq_to_core0(EXTIOI_UART_IRQ);
    extioi_enable_irq(EXTIOI_UART_IRQ);

    arch_irq_enable_external();
}

void plat_irq_init_cpu(void)
{
    /* single-core: nothing extra beyond plat_irq_init() */
}

void plat_irq_enable(unsigned int irq)
{
    if (irq == LS7A_UART_SRC_IRQ) {
        ls7a_unmask_irq(LS7A_UART_SRC_IRQ);
        extioi_enable_irq(EXTIOI_UART_IRQ);
    }
}

void plat_irq_disable(unsigned int irq)
{
    if (irq == LS7A_UART_SRC_IRQ) {
        extioi_disable_irq(EXTIOI_UART_IRQ);
        ls7a_mask_irq(LS7A_UART_SRC_IRQ);
    }
}

int plat_irq_claim(void)
{
    /*
     * There is no PLIC-style claim register here.
     * For the current minimal kernel, we only support UART external irq.
     *
     * If LS7A source irq2 is pending, report UART irq.
     */
    uint32_t pending = mmio_read32(LS7A_PCH_PIC_ISR);

    if (pending & (1U << LS7A_UART_SRC_IRQ))
        return (int)LS7A_UART_SRC_IRQ;

    return 0;
}

void plat_irq_complete(unsigned int irq)
{
    if (irq != LS7A_UART_SRC_IRQ)
        return;

    /*
     * In level-trigger mode, device handling should make the source deassert.
     * Here we only ack EXTIOI.
     *
     * If you later switch LS7A to edge-trigger mode, you also need:
     *   mmio_write32(LS7A_PCH_PIC_INTCLR, 1U << LS7A_UART_SRC_IRQ);
     */
    extioi_ack_irq(EXTIOI_UART_IRQ);
}
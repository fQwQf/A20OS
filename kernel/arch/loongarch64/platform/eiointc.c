#ifdef CONFIG_LOONGARCH64

#include "platform.h"
#include "core/types.h"
#include "core/defs.h"
#include "drivers/core/driver_hwapi.h"

/*
 * LoongArch64 QEMU virt I/O interrupt controllers.
 *
 * The virt machine routes:
 *   PCI INTx / UART / RTC ... -> PCH-PIC (platic @ 0x10000000)
 *   PCH-PIC output line i     -> EIOINTC input i
 *   EIOINTC (IOCSR @ 0x1400)  -> CPU HWI0 (ESTAT IS[2])
 *
 * QEMU reset state already routes every EIOINTC input to HWI0 on CPU 0
 * (IP map / core map reset to 0) and every PCH-PIC line through PCH-PIC
 * output 0 (HTMSI vector reset to 0).  To receive PCI INTx we therefore only
 * have to:
 *   - unmask the PCH-PIC lines used by PCI INTx (16..19),
 *   - enable the corresponding EIOINTC input set,
 *   - demux the PCH-PIC status register in the HWI0 external IRQ handler.
 *
 * Keep the legacy arch_virtio_*_probe() path (which runs no IRQ) untouched;
 * platforms that do not wire these controllers keep completion polling.
 */

#define LA_PCH_PIC_BASE        0x10000000UL
#define PCH_PIC_INT_MASK_LO    (LA_PCH_PIC_BASE + 0x20)
#define PCH_PIC_INT_MASK_HI    (LA_PCH_PIC_BASE + 0x24)
#define PCH_PIC_INT_STATUS_LO  (LA_PCH_PIC_BASE + 0x3a0)

#define EIOINTC_IOCSR_BASE     0x1400UL
#define EIOINTC_ENABLE         (EIOINTC_IOCSR_BASE + 0x200)
#define EIOINTC_COREISR        (EIOINTC_IOCSR_BASE + 0x400)

/* PCI INTx A..D map to PCH-PIC lines 16..19 with the standard swizzle. */
#define LA_PCI_INTX_IRQ_BASE   16

static inline void loongarch64_iocsr_write32(uint32_t value, uint32_t reg)
{
    __asm__ __volatile__("iocsrwr.w %0, %1" :: "r"(value), "r"(reg) : "memory");
}

void la64_eiointc_pic_init(void)
{
    /* Unmask the PCI INTx lines (16..19); keep every other PIC line masked
     * until a driver requests it.  Reset state masks all 64 lines. */
    *(volatile uint32_t *)PCH_PIC_INT_MASK_LO = 0xFFF0FFFFU;
    *(volatile uint32_t *)PCH_PIC_INT_MASK_HI = 0xFFFFFFFFU;
    mb();

    /* With the default HTMSI vector all PCH-PIC outputs collapse onto
     * EIOINTC input 0, so enabling word 0 covers every PCI INTx line. */
    loongarch64_iocsr_write32(0xFFFFFFFFU, EIOINTC_ENABLE);
    mb();
}

static int la64_dispatch_pic_irq(uint32_t status)
{
    int handled = 0;
    while (status) {
        int line = __builtin_ctz(status);
        status &= status - 1;
        driver_irq_dispatch((uint32_t)line);
        handled = 1;
    }
    return handled;
}

/*
 * External IRQ (HWI0) handler for the QEMU virt machine.
 * Runs in trap context with the device interrupt still asserted in the
 * PCH-PIC; dispatches each active line, then writes 1-to-clear to the
 * EIOINTC core ISR so the CPU HWI0 line drops.
 */
void la64_handle_device_irq(void)
{
    uint32_t status = *(volatile uint32_t *)PCH_PIC_INT_STATUS_LO;
    if (status) {
        (void)la64_dispatch_pic_irq(status);
        loongarch64_iocsr_write32(1U, EIOINTC_COREISR);
        mb();
    }
}

/*
 * Map a PCI INTx pin to the platform interrupt line.  Returns -1 when the
 * platform has no wired INTx routing (the transport then keeps polling).
 */
int arch_pci_intx_irq(int bus, int dev, int func, int pin)
{
    (void)bus;
    (void)func;
    if (pin < 1 || pin > 4)
        return -1;
    return LA_PCI_INTX_IRQ_BASE + ((dev + pin - 1) & 3);
}

#endif /* CONFIG_LOONGARCH64 */

#ifdef CONFIG_X86_64

#include "cpu.h"
#include "drivers/bus/pci_hal.h"
#include "platform.h"
#include "core/lock.h"
#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
static spinlock_t pci_config_lock = SPINLOCK_INIT;

#define PCI_CFG_ADDRESS_PORT 0x0CF8
#define PCI_CFG_DATA_PORT    0x0CFC

static uintptr_t pci_ecam_base = PCI_ECAM_BASE;
static int pci_legacy_only;

void arch_pci_host_init(uintptr_t ecam_base) {
    pci_ecam_base = ecam_base;
    pci_legacy_only = !ecam_base || ecam_base < PAGE_OFFSET ||
        !arch_device_map_mmio(ecam_base - PAGE_OFFSET, 4096, 0);
}

/* Machines predating PCIe have no ECAM window at all, and the 0xCF8/0xCFC
 * indirection pair is the only way in.  Deciding which one this is means asking
 * the window for something only a real ECAM can answer.
 *
 * The test is the host bridge at 00:00.0, not the base address itself.  A
 * machine with no ECAM does not read back as one fixed wrong value: QEMU's
 * i440fx leaves the address unmapped, so the first dword reads 0x00000000,
 * while a machine that decodes the range but has no host bridge there reads
 * 0xffffffff.  Either way it is not a vendor id, and both have to be rejected
 * -- checking only for all-ones is what let an i440fx boot enumerate 129
 * phantom devices at bus fe with id=0000:0000 and then fail much later with
 * "no init program found", pointing at nothing.
 *
 * A real host bridge always has a vendor id, and no machine has one at 00:00.0,
 * so a nonzero, non-all-ones vendor id means the window is live. */
static int ecam_absent(void) {
    if (pci_legacy_only)
        return 1;
    uint32_t id = *(volatile uint32_t *)pci_ecam_base;
    if (id == 0x00000000U || id == 0xffffffffU) {
        pci_legacy_only = 1;
        return 1;
    }
    return 0;
}

/* Serialize CF8 selection with the exact-width CFC transaction. Width must
 * not be emulated using read/modify/write (PCI status bits may be W1C). */
int x86_pci_legacy_access(int bus, int dev, int func, unsigned reg,
                          unsigned width, int write, uint32_t *value) {
    if (!value || bus < 0 || bus > 255 || dev < 0 || dev > 31 || func < 0 || func > 7 ||
        (width != 1 && width != 2 && width != 4) || reg >= 256 ||
        width > 256 - reg || (reg & (width - 1))) return -EINVAL;
    uint64_t saved = spin_lock_irqsave(&pci_config_lock);
    outl(PCI_CFG_ADDRESS_PORT, 0x80000000U | ((uint32_t)bus << 16) |
         ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xfc));
    uint16_t port = PCI_CFG_DATA_PORT + (reg & 3);
    if (write) {
        if (width == 1) outb(port, (uint8_t)*value);
        else if (width == 2) outw(port, (uint16_t)*value);
        else outl(port, *value);
    } else {
        if (width == 1) *value = inb(port);
        else if (width == 2) *value = inw(port);
        else *value = inl(port);
    }
    spin_unlock_irqrestore(&pci_config_lock, saved);
    return 0;
}

static uint32_t pci_cfg_read32(int bus, int dev, int func, uint32_t reg) {
    if (!ecam_absent()) {
        uintptr_t addr = pci_ecam_base
            | ((uint32_t)bus << 20)
            | ((uint32_t)dev << 15)
            | ((uint32_t)func << 12)
            | (reg & 0xFFC);
        return *(volatile uint32_t *)addr;
    }
    uint32_t value = 0xffffffffU;
    (void)x86_pci_legacy_access(bus, dev, func, reg, 4, 0, &value);
    return value;
}

static void pci_cfg_write32(int bus, int dev, int func, uint32_t reg,
                            uint32_t val) {
    if (!ecam_absent()) {
        uintptr_t addr = pci_ecam_base
            | ((uint32_t)bus << 20)
            | ((uint32_t)dev << 15)
            | ((uint32_t)func << 12)
            | (reg & 0xFFC);
        *(volatile uint32_t *)addr = val;
        return;
    }
    (void)x86_pci_legacy_access(bus, dev, func, reg, 4, 1, &val);
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    return pci_cfg_read32(bus, dev, func, reg);
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    pci_cfg_write32(bus, dev, func, reg, val);
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    return (uintptr_t)arch_device_map_mmio(bar_addr, 1, 0);
}

#endif

#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
int arch_pci_mmio_window(uint64_t *base, uint64_t *size) {
    if (!base || !size) return -EINVAL;
#ifdef CONFIG_BOARD_QEMU_VIRT_X86_64
    *base = 0xc0000000ULL; *size = 0x30000000ULL;
    return 0;
#else
    *base = 0; *size = 0;
    return -EOPNOTSUPP;
#endif
}
int arch_pci_bar_reachable(uint64_t base, uint64_t size) {
    if (!size || size > (uint64_t)(~(size_t)0)) return 0;
    return arch_device_map_mmio(base, (size_t)size, 0) != NULL;
}

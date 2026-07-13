#include "drivers/bus/pci_hal.h"
#include "platform.h"

#define PPC64_H_RTAS                    0xF000UL
#define PPC64_RTAS_IBM_READ_PCI_CONFIG  0x2016U
#define PPC64_RTAS_IBM_WRITE_PCI_CONFIG 0x2017U
#define PPC64_PCI_BUID                  0x0800000020000000UL

typedef struct {
    uint32_t token;
    uint32_t nargs;
    uint32_t nret;
    uint32_t data[8];
} ppc64_rtas_args_t;

static ppc64_rtas_args_t rtas_args __attribute__((aligned(64)));

static uint32_t cpu_to_be32(uint32_t v) {
    return __builtin_bswap32(v);
}

static uint32_t be32_to_cpu(uint32_t v) {
    return __builtin_bswap32(v);
}

static uint32_t rtas_config_addr(int bus, int dev, int func, uint32_t reg) {
    uint32_t devfn = ((uint32_t)dev << 3) | (uint32_t)func;
    return ((reg & 0xF00U) << 20) |
           (((uint32_t)bus & 0xFFU) << 16) |
           (devfn << 8) | (reg & 0xFFU);
}

static long ppc64_rtas_call(uint32_t token, uint32_t nargs, uint32_t nret) {
    register uint64_t r3 __asm__("r3") = PPC64_H_RTAS;
    register uint64_t r4 __asm__("r4") =
        (uint64_t)((uintptr_t)&rtas_args - PAGE_OFFSET);

    rtas_args.token = cpu_to_be32(token);
    rtas_args.nargs = cpu_to_be32(nargs);
    rtas_args.nret = cpu_to_be32(nret);

    __asm__ __volatile__(
        "sc 1"
        : "+r"(r3), "+r"(r4)
        :
        : "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
          "ctr", "lr", "cr0", "memory");
    if (r3 != 0)
        return (long)r3;
    return (int32_t)be32_to_cpu(rtas_args.data[nargs]);
}

void arch_pci_host_init(uintptr_t ecam_base) {
    (void)ecam_base;
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    rtas_args.data[0] = cpu_to_be32(rtas_config_addr(bus, dev, func, reg));
    rtas_args.data[1] = cpu_to_be32((uint32_t)(PPC64_PCI_BUID >> 32));
    rtas_args.data[2] = cpu_to_be32((uint32_t)PPC64_PCI_BUID);
    rtas_args.data[3] = cpu_to_be32(4);
    if (ppc64_rtas_call(PPC64_RTAS_IBM_READ_PCI_CONFIG, 4, 2) != 0)
        return 0xFFFFFFFFU;
    return be32_to_cpu(rtas_args.data[5]);
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    rtas_args.data[0] = cpu_to_be32(rtas_config_addr(bus, dev, func, reg));
    rtas_args.data[1] = cpu_to_be32((uint32_t)(PPC64_PCI_BUID >> 32));
    rtas_args.data[2] = cpu_to_be32((uint32_t)PPC64_PCI_BUID);
    rtas_args.data[3] = cpu_to_be32(4);
    rtas_args.data[4] = cpu_to_be32(val);
    (void)ppc64_rtas_call(PPC64_RTAS_IBM_WRITE_PCI_CONFIG, 5, 1);
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    if (bar_addr >= 0x80000000UL && bar_addr < 0x100000000UL)
        return (uintptr_t)(bar_addr + 0x200000000000UL);
    return (uintptr_t)bar_addr;
}

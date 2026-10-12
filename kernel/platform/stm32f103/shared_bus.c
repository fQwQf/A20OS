#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"

#define MMIO32(addr) (*(volatile uint32_t *)(addr))
/* Custom STM32F103Z(E) fabric wiring: NE1 shared SRAM, NE2 control registers.
 * Asynchronous 16-bit timing at the reset HSI clock; PD6 is mandatory NWAIT.
 * The fabric must implement the wait/arbitration contract in docs/cluster/25. */
void mcu_fabric_bus_init(void) {
    MMIO32(0x40021018UL) |= 0x1e0U;  /* GPIOD/E/F/G */
    MMIO32(0x40021014UL) |= 1U << 8; /* FSMC */
    static const unsigned pins[][2] = {
        {0x40011400UL, 0},  {0x40011400UL, 1},  {0x40011400UL, 4},
        {0x40011400UL, 5},  {0x40011400UL, 7},  {0x40011400UL, 8},
        {0x40011400UL, 9},  {0x40011400UL, 10}, {0x40011400UL, 11},
        {0x40011400UL, 12}, {0x40011400UL, 13}, {0x40011400UL, 14},
        {0x40011400UL, 15}, {0x40011800UL, 0},  {0x40011800UL, 1},
        {0x40011800UL, 7},  {0x40011800UL, 8},  {0x40011800UL, 9},
        {0x40011800UL, 10}, {0x40011800UL, 11}, {0x40011800UL, 12},
        {0x40011800UL, 13}, {0x40011800UL, 14}, {0x40011800UL, 15},
        {0x40011c00UL, 0},  {0x40011c00UL, 1},  {0x40011c00UL, 2},
        {0x40011c00UL, 3},  {0x40011c00UL, 4},  {0x40011c00UL, 5},
        {0x40011c00UL, 12}, {0x40011c00UL, 13}, {0x40011c00UL, 14},
        {0x40011c00UL, 15}, {0x40012000UL, 0},  {0x40012000UL, 1},
        {0x40012000UL, 2},  {0x40012000UL, 3},  {0x40012000UL, 4},
        {0x40012000UL, 5},  {0x40012000UL, 9},
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        uintptr_t reg = pins[i][0] + (pins[i][1] >= 8 ? 4U : 0U);
        unsigned shift = (pins[i][1] & 7U) * 4U;
        MMIO32(reg) = (MMIO32(reg) & ~(15U << shift)) | (11U << shift);
    }
    /* PD6/FSMC_NWAIT is an input (floating), not AF push-pull like DQ/CS.
     * The board must provide its defined inactive level during power-up. */
    MMIO32(0x40011400UL) = (MMIO32(0x40011400UL) & ~(15U << 24)) | (4U << 24);
    /* DATAST=15, ADDSET=15, turnaround=15. ASYNCWAIT (bit15) enables active-low
     * NWAIT for asynchronous SRAM; WAITEN (bit13) is for synchronous accesses.
     * See RM0008 sections 21.5.4/21.5.6. Preserve conservative setup time for
     * wait assertion; read/write may then stretch until fabric completion. */
    MMIO32(0xa0000004UL) = 0x000f0f0fU;
    MMIO32(0xa000000cUL) = 0x000f0f0fU;
    MMIO32(0xa0000000UL) = 0x00009011U;
    MMIO32(0xa0000008UL) = 0x00009011U;
    __asm__ __volatile__("dsb" ::: "memory");
}
#endif

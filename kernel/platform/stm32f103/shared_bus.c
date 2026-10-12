#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"

#define MMIO32(addr) (*(volatile uint32_t *)(addr))
/* Custom STM32F103Z(E) fabric wiring: NE1 shared SRAM, NE2 control registers.
 * Conservative asynchronous 16-bit timing at the reset HSI clock. The fabric
 * must implement the wait/arbitration contract in docs/cluster/25. */
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
    /* DATAST=15, ADDSET=15, turnaround=15; no burst or write buffering.
     * The controller must complete a bus access in this bounded cycle and
     * arbitrate at least one access per waiting port; otherwise adapt timing
     * and NWAIT explicitly before physical deployment. */
    MMIO32(0xa0000004UL) = 0x000f0f0fU;
    MMIO32(0xa000000cUL) = 0x000f0f0fU;
    MMIO32(0xa0000000UL) = 0x00001011U;
    MMIO32(0xa0000008UL) = 0x00001011U;
    __asm__ __volatile__("dsb" ::: "memory");
}
#endif

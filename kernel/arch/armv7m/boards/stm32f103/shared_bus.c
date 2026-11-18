#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"
#include "board_config.h"

#define MMIO32(addr) (*(volatile uint32_t *)(addr))
/* Custom STM32F103Z(E) fabric wiring: NE1 shared SRAM, NE2 control registers.
 * Asynchronous 16-bit timing at the reset HSI clock; PD6 is mandatory NWAIT.
 * The fabric must implement the wait/arbitration contract in docs/zh/cluster/mcu-shared-kernel.md. */
void mcu_fabric_bus_init(void) {
    MMIO32(STM32_RCC_APB2ENR) |= 0x1e0U;  /* GPIOD/E/F/G */
    MMIO32(STM32_RCC_AHBENR) |= 1U << 8; /* FSMC */
    static const unsigned pins[][2] = {
        {STM32_GPIOD_CRL, 0},  {STM32_GPIOD_CRL, 1},  {STM32_GPIOD_CRL, 4},
        {STM32_GPIOD_CRL, 5},  {STM32_GPIOD_CRL, 7},  {STM32_GPIOD_CRL, 8},
        {STM32_GPIOD_CRL, 9},  {STM32_GPIOD_CRL, 10}, {STM32_GPIOD_CRL, 11},
        {STM32_GPIOD_CRL, 12}, {STM32_GPIOD_CRL, 13}, {STM32_GPIOD_CRL, 14},
        {STM32_GPIOD_CRL, 15}, {STM32_GPIOE_CRL, 0},  {STM32_GPIOE_CRL, 1},
        {STM32_GPIOE_CRL, 7},  {STM32_GPIOE_CRL, 8},  {STM32_GPIOE_CRL, 9},
        {STM32_GPIOE_CRL, 10}, {STM32_GPIOE_CRL, 11}, {STM32_GPIOE_CRL, 12},
        {STM32_GPIOE_CRL, 13}, {STM32_GPIOE_CRL, 14}, {STM32_GPIOE_CRL, 15},
        {STM32_GPIOF_CRL, 0},  {STM32_GPIOF_CRL, 1},  {STM32_GPIOF_CRL, 2},
        {STM32_GPIOF_CRL, 3},  {STM32_GPIOF_CRL, 4},  {STM32_GPIOF_CRL, 5},
        {STM32_GPIOF_CRL, 12}, {STM32_GPIOF_CRL, 13}, {STM32_GPIOF_CRL, 14},
        {STM32_GPIOF_CRL, 15}, {STM32_GPIOG_CRL, 0},  {STM32_GPIOG_CRL, 1},
        {STM32_GPIOG_CRL, 2},  {STM32_GPIOG_CRL, 3},  {STM32_GPIOG_CRL, 4},
        {STM32_GPIOG_CRL, 5},  {STM32_GPIOG_CRL, 9},
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        uintptr_t reg = pins[i][0] + (pins[i][1] >= 8 ? 4U : 0U);
        unsigned shift = (pins[i][1] & 7U) * 4U;
        MMIO32(reg) = (MMIO32(reg) & ~(15U << shift)) | (11U << shift);
    }
    /* PD6/FSMC_NWAIT is an input (floating), not AF push-pull like DQ/CS.
     * The board must provide its defined inactive level during power-up. */
    MMIO32(STM32_GPIOD_CRL) = (MMIO32(STM32_GPIOD_CRL) & ~(15U << 24)) | (4U << 24);
    /* DATAST=15, ADDSET=15, turnaround=15. ASYNCWAIT (bit15) enables active-low
     * NWAIT for asynchronous SRAM; WAITEN (bit13) is for synchronous accesses.
     * See RM0008 sections 21.5.4/21.5.6. Preserve conservative setup time for
     * wait assertion; read/write may then stretch until fabric completion. */
    MMIO32(STM32_FSMC_BTR1) = 0x000f0f0fU;
    MMIO32(STM32_FSMC_BTR2) = 0x000f0f0fU;
    MMIO32(STM32_FSMC_BCR1) = 0x00009011U;
    MMIO32(STM32_FSMC_BCR2) = 0x00009011U;
    __asm__ __volatile__("dsb" ::: "memory");
}
#endif

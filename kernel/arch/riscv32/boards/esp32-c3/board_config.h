#ifndef A20_ESP32C3_BOARD_CONFIG_H
#define A20_ESP32C3_BOARD_CONFIG_H
/* IRAM/DRAM are aliases of SRAM. These disjoint physical windows never overlap.
 * Upper SRAM remains reserved for ROM loader data/stack. See ldscript.ld. */
#define MCU_CODE_BASE 0x40380000UL
#define MCU_CODE_END  0x403a0000UL
#define MCU_RAM_BASE 0x3fca0000UL
#define MCU_RAM_END  0x3fcdc000UL
#endif

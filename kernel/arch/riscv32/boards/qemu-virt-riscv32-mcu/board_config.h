#ifndef A20_RV32_MCU_QEMU_CONFIG_H
#define A20_RV32_MCU_QEMU_CONFIG_H
/*
 * Keep the emulated machine inside Hi3861's resource envelope.  Hi3861 has
 * 352 KiB of SRAM and runs its text from flash; here text shares RAM, so the
 * window is trimmed to 320 KiB to leave a comparable ~236 KiB heap.
 */
#define MCU_RAM_BASE 0x80000000UL
#define MCU_RAM_END  0x80050000UL
#define MCU_CODE_BASE 0x80000000UL
#define MCU_CODE_END 0x80050000UL
#endif

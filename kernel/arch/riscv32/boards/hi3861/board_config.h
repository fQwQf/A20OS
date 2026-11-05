#ifndef A20_HI3861_BOARD_CONFIG_H
#define A20_HI3861_BOARD_CONFIG_H

/*
 * Hi3861 kernel A starts at the payload of the standard 0x3c0-byte firmware
 * header.  The FlashBoot on the SoC jumps there and executes the image in
 * place; .data is copied down to SRAM by the RV32 MCU entry code.
 */
#define HI3861_FLASH_BASE              0x00400000UL
#define HI3861_KERNEL_PARTITION        0x0000D000UL
#define HI3861_KERNEL_PARTITION_SIZE   0x000E4000UL
#define HI3861_FIRMWARE_HEADER_SIZE    0x000003C0UL
#define HI3861_KERNEL_BASE             (HI3861_FLASH_BASE + HI3861_KERNEL_PARTITION + \
                                        HI3861_FIRMWARE_HEADER_SIZE)

/* Keep the ROM/flashboot data and check-info area at 0x11a9c0..0x11e000 free. */
#define MCU_CODE_BASE   HI3861_KERNEL_BASE
#define MCU_CODE_END    (HI3861_FLASH_BASE + HI3861_KERNEL_PARTITION + \
                         HI3861_KERNEL_PARTITION_SIZE)
#define MCU_RAM_BASE    0x000D8000UL
#define MCU_RAM_END     0x0011A000UL

/*
 * A 1 kHz tick makes every timer IRQ run the full scheduler tick path on a
 * 160 MHz part; the handler then costs about as much as the period and the
 * CPU can end up re-entering the interrupt at __switch's `ret` before a new
 * task ever executes.  100 Hz leaves a wide margin and is plenty for the
 * shell/sleep workload this port supports.
 */
#define ARCH_TIMER_FREQ 100UL
#define A20_CPU_HZ      160000000UL

#endif

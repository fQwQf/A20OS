/* The factory M0Sense UF2 bootloader enters raw applications at 0x23000000.
 * Relocate into SRAM before touching root clocks or flash clock dividers. */
    .section .text.xip_entry, "ax"
    .balign 4
    .global xip_start
xip_start:
    csrci mstatus, 8
    csrw mie, zero
    .option push
    .option norelax
    la t0, __text_load
    la t1, __text_run
    la t2, __text_end
1:
    bgeu t1, t2, 2f
    lw t3, 0(t0)
    sw t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 1b
2:
    la t0, __data_load
    la t1, __data_run
    la t2, __data_end
3:
    bgeu t1, t2, 4f
    lw t3, 0(t0)
    sw t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 3b
4:
    fence.i
    la t0, probe_start
    jr t0
    .option pop

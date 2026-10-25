/* SRAM-only M-mode probe; independent of the S-mode/SBI RV32 kernel. */
    .section .text.entry, "ax"
    .balign 4
    .global probe_start
probe_start:
    csrci mstatus, 8
    csrw mie, zero
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __stack_top
    la t0, probe_trap
    csrw mtvec, t0
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:
    fence.i
    call probe_main
3:
    j 3b

    .balign 64
probe_trap:
    csrci mstatus, 8
    csrr a0, mcause
    csrr a1, mepc
    csrr a2, mtval
    call probe_fatal
4:
    j 4b

    .section .text.entry,"ax",@progbits
    .global _start
_start:
    csrci mstatus, 8
    csrw mie, zero
    la sp, _stack_top
    mv tp, zero
    la t0, _bss_start
    la t1, _bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  la t0, riscv_mcu_vectors
    ori t0, t0, 1
    csrw mtvec, t0
    call kernel_main
3:  wfi
    j 3b

    /* ESP32-C3 requires vectored mtvec; also valid on standard RISC-V. */
    .section .text.vectors,"ax",@progbits
    .balign 256
    .global riscv_mcu_vectors
riscv_mcu_vectors:
    .option push
    .option norvc
    .rept 32
    j riscv_mcu_trap
    .endr
    .option pop

    .section .text,"ax",@progbits
    .balign 4
riscv_mcu_trap:
    /* Integer registers, mepc and mstatus. Keep the C ABI 16-byte alignment. */
    addi sp, sp, -144
    sw ra, 4(sp)
    sw gp, 12(sp)
    sw tp, 16(sp)
    sw t0, 20(sp)
    addi t0, sp, 144
    sw t0, 8(sp)
    sw t1, 24(sp)
    sw t2, 28(sp)
    sw s0, 32(sp)
    sw s1, 36(sp)
    sw a0, 40(sp)
    sw a1, 44(sp)
    sw a2, 48(sp)
    sw a3, 52(sp)
    sw a4, 56(sp)
    sw a5, 60(sp)
    sw a6, 64(sp)
    sw a7, 68(sp)
    sw s2, 72(sp)
    sw s3, 76(sp)
    sw s4, 80(sp)
    sw s5, 84(sp)
    sw s6, 88(sp)
    sw s7, 92(sp)
    sw s8, 96(sp)
    sw s9, 100(sp)
    sw s10, 104(sp)
    sw s11, 108(sp)
    sw t3, 112(sp)
    sw t4, 116(sp)
    sw t5, 120(sp)
    sw t6, 124(sp)
    csrr t0, mepc
    sw t0, 128(sp)
    csrr t0, mstatus
    sw t0, 132(sp)
    mv a0, sp
    call riscv_mcu_trap_handler
    lw t0, 128(sp)
    csrw mepc, t0
    lw t0, 132(sp)
    csrw mstatus, t0
    lw ra, 4(sp)
    lw gp, 12(sp)
    lw tp, 16(sp)
    lw t0, 20(sp)
    lw t1, 24(sp)
    lw t2, 28(sp)
    lw s0, 32(sp)
    lw s1, 36(sp)
    lw a0, 40(sp)
    lw a1, 44(sp)
    lw a2, 48(sp)
    lw a3, 52(sp)
    lw a4, 56(sp)
    lw a5, 60(sp)
    lw a6, 64(sp)
    lw a7, 68(sp)
    lw s2, 72(sp)
    lw s3, 76(sp)
    lw s4, 80(sp)
    lw s5, 84(sp)
    lw s6, 88(sp)
    lw s7, 92(sp)
    lw s8, 96(sp)
    lw s9, 100(sp)
    lw s10, 104(sp)
    lw s11, 108(sp)
    lw t3, 112(sp)
    lw t4, 116(sp)
    lw t5, 120(sp)
    lw t6, 124(sp)
    addi sp, sp, 144
    mret

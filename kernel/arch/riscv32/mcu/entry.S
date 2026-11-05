    .section .text.entry,"ax",@progbits
    .global _start
_start:
    csrci mstatus, 8
    csrw mie, zero
    la sp, _stack_top
    mv tp, zero
#ifdef CONFIG_MCU_COPY_DATA
    /* XIP boards link .data with a flash LMA and a RAM VMA. */
    la t0, _data_load
    la t1, _data_start
    la t2, _data_end
1:  bgeu t1, t2, 2f
    lw t3, 0(t0)
    sw t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 1b
2:
#endif
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
    /* Trap stack switch.  While U-mode runs, mscratch holds that task's
     * kernel stack top; a trap from M-mode keeps mscratch == 0 and stays on
     * the current stack.  csrrw + bnez touches no general register, so the
     * interrupted t0 is still intact when the frame is written below. */
    csrrw sp, mscratch, sp
    bnez sp, 9f
    csrrw sp, mscratch, sp        /* from M: restore sp (mscratch stays 0) */
    addi sp, sp, -144
    sw ra, 4(sp)
    sw gp, 12(sp)
    sw tp, 16(sp)
    sw t0, 20(sp)
    addi t0, sp, 144
    sw t0, 8(sp)
    j 8f
9:  addi sp, sp, -144              /* from U: sp is the kernel stack top */
    sw ra, 4(sp)
    sw gp, 12(sp)
    sw tp, 16(sp)
    sw t0, 20(sp)
    csrr t0, mscratch             /* user sp */
    sw t0, 8(sp)
    csrw mscratch, zero           /* handler never runs on the user stack */
8:  sw t1, 24(sp)
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
    csrr t0, mstatus
    sw t0, 128(sp)
    csrr t0, mepc
    sw t0, 132(sp)
    mv a0, sp
    call riscv_mcu_trap_handler
    lw t0, 132(sp)
    csrw mepc, t0
    lw t0, 128(sp)
    csrw mstatus, t0
    lw ra, 4(sp)
    lw gp, 12(sp)
    lw tp, 16(sp)
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

    /* Return path depends on the privilege the trap came from (MPP). */
    lw t0, 128(sp)
    srli t0, t0, 11
    andi t0, t0, 3
    bnez t0, 1f                   /* back to M */
    /* Back to U: keep the kernel stack top in mscratch for the next trap,
     * restore the interrupted t0 and the user stack pointer. */
    addi t0, sp, 144
    csrw mscratch, t0
    lw t0, 20(sp)
    lw sp, 8(sp)
    mret
1:  csrw mscratch, zero
    lw t0, 20(sp)
    addi sp, sp, 144
    mret

	/* Enter U-mode: a0 = entry, a1 = arg, a2 = user sp, a3 = mstatus.
	 * PMP is opened for the whole address space: this profile is a trusted
	 * flat-program model, not hardware isolation. */
	.section .text
	.balign 4
	.globl mcu_enter_user
mcu_enter_user:
	li	t0, -1
	csrw	pmpaddr0, t0
	csrw	pmpaddr1, t0
	/* entry 0/1 both NAPOT R/W/X until the first trap installs the stack
	 * guard via riscv_mcu_pmp_update(). */
	li	t0, 0x1f1f
	csrw	pmpcfg0, t0

	csrw	mepc, a0
	csrw	mstatus, a3
	mv	sp, a2
	mv	a0, a1
	mret

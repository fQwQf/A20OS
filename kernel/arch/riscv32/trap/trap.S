.altmacro
.macro SAVE_GP n
    sw x\n, \n*4(sp)
.endm
.macro LOAD_GP n
    lw x\n, \n*4(sp)
.endm
.macro SAVE_FP n
    fsd f\n, (144 + \n*8)(sp)
.endm
.macro LOAD_FP n
    fld f\n, (144 + \n*8)(sp)
.endm

.set RISCV32_FRAME_SIZE, 416

    .section .text
    .globl __trap_from_user
    .globl __return_to_user
    .globl __trap_from_kernel
    .globl user_trap_return
    .align 2

__trap_from_user:
    csrrw   sp, sscratch, sp
    addi    sp, sp, -RISCV32_FRAME_SIZE
    sw      x1, 1*4(sp)
    sw      x3, 3*4(sp)
    sw      x4, 4*4(sp)
    .set n, 5
    .rept 27
        SAVE_GP %n
        .set n, n+1
    .endr
    csrr    t0, sstatus
    csrr    t1, sepc
    sw      t0, 32*4(sp)
    sw      t1, 33*4(sp)
    csrr    t2, sscratch
    sw      t2, 2*4(sp)
    sw      a0, 34*4(sp)
    frcsr   t0
    sw      t0, 400(sp)
    .set n, 0
    .rept 32
        SAVE_FP %n
        .set n, n+1
    .endr
    mv      a0, sp
    lw      tp, 35*4(sp)
    la      t0, __trap_from_kernel
    csrw    stvec, t0
    li      t0, (1 << 8)
    csrs    sstatus, t0
    call    trap_handler
    j       __return_to_user

	.align 2
__return_to_user:
    la      t0, __trap_from_user
    csrw    stvec, t0
#ifndef CONFIG_NOMMU
    lw      t0, 0*4(sp)
    csrr    t1, satp
    beq     t0, t1, 1f
    csrw    satp, t0
    sfence.vma
1:
#endif
    lw      t0, 32*4(sp)
    lw      t1, 33*4(sp)
    lw      t2, 2*4(sp)
    csrw    sstatus, t0
    csrw    sepc, t1
    csrw    sscratch, t2
    lw      t0, 400(sp)
    fscsr   t0
    .set n, 0
    .rept 32
        LOAD_FP %n
        .set n, n+1
    .endr
    lw      x1, 1*4(sp)
    lw      x3, 3*4(sp)
    .set n, 5
    .rept 27
        LOAD_GP %n
        .set n, n+1
    .endr
    sw      x4, 35*4(sp)
    lw      x4, 4*4(sp)
    addi    sp, sp, RISCV32_FRAME_SIZE
    csrrw   sp, sscratch, sp
    csrc    sstatus, 8
    sret

	.align 2
__trap_from_kernel:
    addi    sp, sp, -36*4
    sw      x1, 1*4(sp)
    .set n, 3
    .rept 29
        SAVE_GP %n
        .set n, n+1
    .endr
    csrr    t0, sstatus
    csrr    t1, sepc
    sw      t0, 32*4(sp)
    sw      t1, 33*4(sp)
    sw      sp, 2*4(sp)
    mv      a0, sp
    call    kernel_trap_handler
    lw      t0, 32*4(sp)
    lw      t1, 33*4(sp)
    csrw    sstatus, t0
    csrw    sepc, t1
    lw      x1, 1*4(sp)
    .set n, 3
    .rept 29
        LOAD_GP %n
        .set n, n+1
    .endr
    addi    sp, sp, 36*4
    sret

user_trap_return:
    j __return_to_user

    .globl arch_return_to_user
arch_return_to_user:
    mv sp, a0
    j __return_to_user

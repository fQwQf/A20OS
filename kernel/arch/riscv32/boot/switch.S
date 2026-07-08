.altmacro
.macro SAVE_CALLEE n
    sw s\n, (\n+2)*4(sp)
.endm
.macro LOAD_CALLEE n
    lw s\n, (\n+2)*4(a0)
.endm

    .section .text
    .globl __switch
    .align 2

__switch:
    addi    sp, sp, -16*4
    sw      ra, 0(sp)
    sw      tp, 4(sp)
    .set n, 0
    .rept 12
        SAVE_CALLEE %n
        .set n, n+1
    .endr
    csrr    t0, satp
    sw      t0, 14*4(sp)
    csrr    t0, sstatus
    sw      t0, 15*4(sp)
    sw      sp, 0(tp)
    lw      ra, 0(a0)
    lw      tp, 4(a0)
    .set n, 0
    .rept 12
        LOAD_CALLEE %n
        .set n, n+1
    .endr
    lw      t0, 14*4(a0)
    csrr    t1, satp
    beq     t0, t1, 1f
    csrw    satp, t0
    sfence.vma
1:
    lw      t0, 15*4(a0)
    csrw    sstatus, t0
    addi    a0, a0, 16*4
    mv      sp, a0
    ret

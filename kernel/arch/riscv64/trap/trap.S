/*
 * A20OS trap handling for RISC-V 64-bit
 *
 * TrapContext layout (560 bytes, aligned 16):
 *   offset 0-248:   x[0..31]   (32 general-purpose registers, 8 bytes each)
 *   offset 256:     sstatus    (CSR, at 32*8)
 *   offset 264:     sepc       (CSR, at 33*8)
 *   offset 272:     last_a0    (original a0 for SA_RESTART, at 34*8)
 *   offset 280:     kernel_tp  (kernel task pointer, at 35*8)
 *   offset 288-536: f[0..31]   (floating-point registers, at 36*8)
 *   offset 544:     fcsr       (floating-point control/status, at 68*8)
 *   Total: 70*8 = 560 bytes
 *
 * Ported from RocketOS trap.S with physical-address-only adjustments.
 */

.altmacro
.macro SAVE_GP n
    sd x\n, \n*8(sp)
.endm
.macro LOAD_GP n
    ld x\n, \n*8(sp)
.endm
.macro SAVE_FP n
    fsd f\n, (36+\n)*8(sp)
.endm
.macro LOAD_FP n
    fld f\n, (36+\n)*8(sp)
.endm

    .section .text
    .globl __trap_from_user
    .globl __return_to_user
    .globl __trap_from_kernel
    .globl user_trap_return
    .align 2

/*
 * __trap_from_user
 * Called when a trap occurs while in user mode.
 * sscratch holds the kernel stack pointer.
 * We swap sp with sscratch, save user context on kernel stack,
 * then call trap_handler(trap_context_t *ctx).
 */
__trap_from_user:
    /* Swap sp and sscratch: sp now = kernel stack, sscratch now = user stack */
    csrrw   sp, sscratch, sp

    /* Allocate TrapContext on kernel stack */
    addi    sp, sp, -70*8

    /* Save general-purpose registers */
    sd      x1, 1*8(sp)
    /* Skip x2 (sp) - saved later from sscratch */
    sd      x3, 3*8(sp)
    /* Save user tp */
    sd      x4, 4*8(sp)
    /* Save x5~x31 */
    .set n, 5
    .rept 27
        SAVE_GP %n
        .set n, n+1
    .endr

    /* Now we can use t0/t1/t2 freely (already saved) */
    /* Save sstatus and sepc */
    csrr    t0, sstatus
    csrr    t1, sepc
    sd      t0, 32*8(sp)
    sd      t1, 33*8(sp)

    /* FP loads/stores require FS != Off. */
    li      t1, (3 << 13)
    csrs    sstatus, t1
    .set n, 0
    .rept 32
        SAVE_FP %n
        .set n, n+1
    .endr
    frcsr   t1
    sd      t1, 68*8(sp)

    /* Read user stack from sscratch and save it as x[2] */
    csrr    t2, sscratch
    sd      t2, 2*8(sp)

    /* Save original a0 to last_a0 (for signal SA_RESTART) */
    sd      a0, 34*8(sp)

    /* First argument: TrapContext address on kernel stack */
    mv      a0, sp

    /* Load kernel tp from kernel_tp field */
    ld      tp, 35*8(sp)

    /* Set stvec to kernel trap handler (we're now in kernel mode) */
    la      t0, __trap_from_kernel
    csrw    stvec, t0

    /* Set SPP=1 (entering supervisor mode), SUM=1, and keep FP usable. */
    li      t0, (1 << 8) | (1 << 18)
    csrs    sstatus, t0

    /* Call C trap handler */
    call    trap_handler

    /* After handler returns, restore user context */
    j       __return_to_user

/*
 * __return_to_user
 * Restore user context from kernel stack and return to user mode.
 * Expects sp to point to TrapContext on kernel stack.
 */
__return_to_user:
    /* Set stvec back to user trap handler (before restoring context) */
    la      t0, __trap_from_user
    csrw    stvec, t0

    ld      t0, 0*8(sp)
    csrr    t1, satp
    beq     t0, t1, .Lskip_satp
    csrw    satp, t0
    sfence.vma
.Lskip_satp:

    ld      t0, 32*8(sp)
    ld      t1, 33*8(sp)
    ld      t2, 2*8(sp)

    /* Keep FS enabled while restoring FP state, then restore user sstatus. */
    li      t3, (3 << 13)
    csrs    sstatus, t3
    .set n, 0
    .rept 32
        LOAD_FP %n
        .set n, n+1
    .endr
    ld      t3, 68*8(sp)
    fscsr   t3

    csrw    sstatus, t0
    csrw    sepc, t1
    csrw    sscratch, t2

    /* Restore general-purpose registers (except sp and tp) */
    ld      x1, 1*8(sp)
    ld      x3, 3*8(sp)
    /* Skip x4 (tp) for now */

    .set n, 5
    .rept 27
        LOAD_GP %n
        .set n, n+1
    .endr

    /* Save kernel tp into kernel_tp field */
    sd      x4, 35*8(sp)
    /* Restore user tp */
    ld      x4, 4*8(sp)

    /* Release TrapContext on kernel stack */
    addi    sp, sp, 70*8

    /* Swap sp and sscratch: sp = user stack, sscratch = kernel stack */
    csrrw   sp, sscratch, sp

    /* Clear SPP bit to return to user mode */
    csrc    sstatus, 8

    sret

/*
 * __trap_from_kernel
 * Called when a trap occurs while already in kernel mode.
 * No stack swap needed. We save context on current kernel stack.
 * Kernel traps use the full context too, so floating-point state is preserved
 * if a kernel path or compiler-generated code uses FP registers.
 */
__trap_from_kernel:
    /* No stack swap - already on kernel stack */
    addi    sp, sp, -70*8

    /* Save general-purpose registers */
    sd      x1, 1*8(sp)
    /* Skip x2 (sp), saved later */

    /* Save x3~x31 */
    .set n, 3
    .rept 29
        SAVE_GP %n
        .set n, n+1
    .endr

    /* Save sstatus and sepc */
    csrr    t0, sstatus
    csrr    t1, sepc
    sd      t0, 32*8(sp)
    sd      t1, 33*8(sp)

    li      t1, (3 << 13)
    csrs    sstatus, t1
    .set n, 0
    .rept 32
        SAVE_FP %n
        .set n, n+1
    .endr
    frcsr   t1
    sd      t1, 68*8(sp)

    /* Save current sp */
    sd      sp, 2*8(sp)

    /* First argument: TrapContext address */
    mv      a0, sp

    /* Call C kernel trap handler */
    call    kernel_trap_handler

    /* Restore context after handler returns */
    ld      t0, 32*8(sp)
    ld      t1, 33*8(sp)

    li      t2, (3 << 13)
    csrs    sstatus, t2
    .set n, 0
    .rept 32
        LOAD_FP %n
        .set n, n+1
    .endr
    ld      t2, 68*8(sp)
    fscsr   t2

    csrw    sstatus, t0
    csrw    sepc, t1

    /* Restore general-purpose registers */
    ld      x1, 1*8(sp)

    .set n, 3
    .rept 29
        LOAD_GP %n
        .set n, n+1
    .endr

    /* Release TrapContext */
    addi    sp, sp, 70*8

    sret

user_trap_return:
    j __return_to_user

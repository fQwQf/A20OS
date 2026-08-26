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

    /* FS=Off has no user FP state.  FS=Clean already has an identical trusted
     * copy in this task's persistent trap frame, so only Initial/Dirty state
     * needs to be written back on entry.  The return side remains conservative
     * and reloads any non-Off state in case the syscall scheduled another
     * task. */
    li      t1, (3 << 13)
    and     t1, t0, t1
    beqz    t1, .Luser_fp_save_done
    li      t2, (2 << 13)
    beq     t1, t2, .Luser_fp_save_done
    li      t1, (3 << 13)
    csrs    sstatus, t1
    .set n, 0
    .rept 32
        SAVE_FP %n
        .set n, n+1
    .endr
    frcsr   t1
    sd      t1, 68*8(sp)
    /* The register image is now synchronized with this task's persistent
     * trap frame.  Return with FS=Clean so a following syscall can retain the
     * live FPRs without saving and restoring all 32 registers again.  Any
     * later user FP instruction makes FS Dirty in hardware automatically. */
    ld      t0, 32*8(sp)
    li      t1, (3 << 13)
    not     t1, t1
    and     t0, t0, t1
    li      t1, (2 << 13)
    or      t0, t0, t1
    sd      t0, 32*8(sp)
.Luser_fp_save_done:
    /* The kernel is compiled for the integer lp64 ABI.  Keep FS disabled in C
     * so an accidental kernel FP instruction traps instead of corrupting live
     * user state retained by the fast-return path. */
    li      t1, (3 << 13)
    csrc    sstatus, t1

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

    /* C arms reserved=1 only for an ordinary user ecall.  Any context switch,
     * signal delivery, ptrace/context rewrite, or exec clears it. */
    ld      t0, 69*8(sp)
    li      t1, 1
    beq     t0, t1, __return_to_user_fast

    /* Complex paths retain the original full-context restore. */
    j       __return_to_user

/*
 * Fast return for a syscall which neither switched tasks nor rewrote the user
 * frame.  The C ABI preserved s0-s11, while caller-saved GPRs are reloaded
 * below.  Clean FP state remains live because FS stayed disabled in kernel C;
 * Initial/Dirty state was saved on entry and is conservatively restored.
 */
	.align 2
__return_to_user_fast:
    la      t0, __trap_from_user
    csrw    stvec, t0

    ld      t0, 32*8(sp)
    ld      t1, 33*8(sp)
    ld      t2, 2*8(sp)

    li      t3, (3 << 13)
    and     t3, t0, t3
    beqz    t3, .Lfast_fp_restore_done
    li      t4, (2 << 13)
    beq     t3, t4, .Lfast_fp_restore_done
    li      t3, (3 << 13)
    csrs    sstatus, t3
    .set n, 0
    .rept 32
        LOAD_FP %n
        .set n, n+1
    .endr
    ld      t3, 68*8(sp)
    fscsr   t3
.Lfast_fp_restore_done:

    csrw    sstatus, t0
    csrw    sepc, t1
    csrw    sscratch, t2

    /* C may clobber caller-saved integer registers. */
    ld      x1, 1*8(sp)
    ld      x3, 3*8(sp)
    ld      x5, 5*8(sp)
    ld      x6, 6*8(sp)
    ld      x7, 7*8(sp)
    ld      x10, 10*8(sp)
    ld      x11, 11*8(sp)
    ld      x12, 12*8(sp)
    ld      x13, 13*8(sp)
    ld      x14, 14*8(sp)
    ld      x15, 15*8(sp)
    ld      x16, 16*8(sp)
    ld      x17, 17*8(sp)
    ld      x28, 28*8(sp)
    ld      x29, 29*8(sp)
    ld      x30, 30*8(sp)
    ld      x31, 31*8(sp)

    /* Preserve the kernel task pointer for the next entry, then restore the
     * user tp and return through the same stack/sscratch exchange. */
    sd      x4, 35*8(sp)
    ld      x4, 4*8(sp)
    addi    sp, sp, 70*8
    csrrw   sp, sscratch, sp
    csrc    sstatus, 8
    sret

/*
 * __return_to_user
 * Restore user context from kernel stack and return to user mode.
 * Expects sp to point to TrapContext on kernel stack.
 */
	.align 2
__return_to_user:
    /* Set stvec back to user trap handler (before restoring context) */
    la      t0, __trap_from_user
    csrw    stvec, t0

#ifndef CONFIG_NOMMU
    ld      t0, 0*8(sp)
    csrr    t1, satp
    beq     t0, t1, .Lskip_satp
    csrw    satp, t0
    la      t2, riscv64_asid_mask
    ld      t2, 0(t2)
    bnez    t2, .Lskip_satp
    sfence.vma
.Lskip_satp:
#endif

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
	.align 2
__trap_from_kernel:
#ifndef CONFIG_NOMMU
    /* Stack-pointer guard FIRST -- before any store touches the possibly-
     * corrupted sp.  Kernel stacks live in the direct map, between
     * PAGE_OFFSET and the pfa-exported direct-map end; a trap arriving
     * outside that window would otherwise fault inside this save path and
     * spin forever with a drifting sp. */
    la      t5, .Lkf_sp_stash
    sd      sp, 0(t5)
    sd      t6, 8(t5)
    /* PAGE_OFFSET (platform.h) without the C 'UL' suffix: as cannot parse it */
    li      t6, 0xFFFFFFC000000000
    bgeu    sp, t6, .Lkf_sp_lo_ok
    j       .Lkf_sp_bad
.Lkf_sp_lo_ok:
    la      t6, g_pfa_direct_map_end
    ld      t6, 0(t6)
    bltu    sp, t6, .Lkf_sp_good
.Lkf_sp_bad:
    ld      a0, 0(t5)
    csrr    a1, sstatus
    csrr    a2, sepc
    ld      a3, 8(t5)
    la      sp, __trap_emerg_top
    call    trap_sp_corrupted_report
.Lkf_sp_good:
    ld      t6, 8(t5)
    ld      t5, 0(t5)
#endif

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

    .globl arch_return_to_user
arch_return_to_user:
    mv sp, a0
    j __return_to_user

    .section .bss
    .align 16
    .globl __trap_emerg_top
__trap_emerg_top:
    .space 512

/* Guard stash: bad sp + original t6 across the range check. */
    .align 8
.Lkf_sp_stash:
    .space 16

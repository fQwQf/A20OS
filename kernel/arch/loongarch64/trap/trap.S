/*
 * LoongArch64 — Trap / Exception / TLB Refill Handling
 *
 * Trap context layout (trap_context_t, 70*8 = 560 bytes):
 *   [0..31]  = regs[32]   (32 GPRs; $r0=$zero, $r1=$ra, $r2=$tp, $r3=$sp, ...)
 *   [32]     = era        (Exception Return Address)
 *   [33]     = prmd       (Previous CRMD — PPLV[1:0], PIE[2])
 *   [34]     = kernel_tp  (task pointer for kernel use)
 *   [35]     = kernel_sp  (kernel stack pointer for return-to-user)
 *   [36..67] = f[32]      (floating-point registers)
 *   [68]     = fcsr0      (floating-point control/status)
 *
 * CSR usage for user↔kernel transitions:
 *   SAVE0 (0x30) = kernel stack pointer
 *   SAVE1 (0x31) = task pointer (tp)
 *
 * General exception entry = trap_entry_la64 (CSR EENTRY = 0xC)
 * TLB refill entry        = tlb_refill_entry_la64 (CSR TLBRENTRY = 0x88)
 *
 * Task context layout (task_context_t, 15*8 = 120 bytes):
 *   [0]  ra     [1] tp    [2..10] s0-s8   [11] fp
 *   [12] sp     [13] pgdl  [14] prmd
 */

#include "loongarch64_asm.h"

#define CTX_SIZE    (70 * 8)
#define REG_OFF(n)  ((n) * 8)
#define ERA_OFF     (32 * 8)
#define PRMD_OFF    (33 * 8)
#define KTP_OFF     (34 * 8)
#define KSP_OFF     (35 * 8)
#define FP_OFF(n)   ((36 + (n)) * 8)
#define FCSR_OFF    (68 * 8)

    .section .text
    .align 4
    .global trap_entry_la64
    .global tlb_refill_entry_la64
    .global __trap_from_kernel
    .global __trap_from_user
    .global __return_to_user
    .global user_trap_return

/* ---- General exception entry ----
 * Dispatched to from CSR EENTRY (0xC).
 * On entry: CRMD.PPLV = previous PLV, ERA = fault PC.
 * If PPLV==3 (user mode): sp = user sp, SAVE0 = kernel sp, SAVE1 = task tp.
 * If PPLV==0 (kernel mode): sp = kernel sp.
 */
trap_entry_la64:
    /* Save original $t0 before we clobber it (TLBRSAVE is free here;
     * TLB refill handler only uses it during TLB miss, which has
     * returned by the time we reach the general exception path). */
    csrwr   $t0, 0x8B             /* TLBRSAVE = original $t0 */

    /* Read PRMD.PPLV to decide user vs kernel trap path */
    csrrd   $t0, 0x1              /* PRMD */
    andi    $t0, $t0, 0x3         /* PPLV bits [1:0] */
    bnez    $t0, __trap_from_user /* PPLV != 0 → came from user */
    b       __trap_from_kernel

/* ---- Trap entry from kernel mode (PLV0 → PLV0) ----
 * sp is already the kernel stack.
 * SAVE0/SAVE1 may contain stale user-mode values — don't touch them.
 */
    .align 4
__trap_from_kernel:
    addi.d  $sp, $sp, -CTX_SIZE

    st.d    $zero, $sp, REG_OFF(0)
    st.d    $ra,   $sp, REG_OFF(1)
    st.d    $tp,   $sp, REG_OFF(2)
    /* REG_OFF(3) = sp: save after context is set up */
    st.d    $a0,   $sp, REG_OFF(4)
    st.d    $a1,   $sp, REG_OFF(5)
    st.d    $a2,   $sp, REG_OFF(6)
    st.d    $a3,   $sp, REG_OFF(7)
    st.d    $a4,   $sp, REG_OFF(8)
    st.d    $a5,   $sp, REG_OFF(9)
    st.d    $a6,   $sp, REG_OFF(10)
    st.d    $a7,   $sp, REG_OFF(11)
    /* Retrieve original $t0 from TLBRSAVE (saved in trap_entry_la64) */
    csrrd   $t0, 0x8B
    st.d    $t0,   $sp, REG_OFF(12)
    st.d    $t1,   $sp, REG_OFF(13)
    st.d    $t2,   $sp, REG_OFF(14)
    st.d    $t3,   $sp, REG_OFF(15)
    st.d    $t4,   $sp, REG_OFF(16)
    st.d    $t5,   $sp, REG_OFF(17)
    st.d    $t6,   $sp, REG_OFF(18)
    st.d    $t7,   $sp, REG_OFF(19)
    st.d    $t8,   $sp, REG_OFF(20)
    st.d    $r21,  $sp, REG_OFF(21)
    st.d    $fp,   $sp, REG_OFF(22)
    st.d    $s0,   $sp, REG_OFF(23)
    st.d    $s1,   $sp, REG_OFF(24)
    st.d    $s2,   $sp, REG_OFF(25)
    st.d    $s3,   $sp, REG_OFF(26)
    st.d    $s4,   $sp, REG_OFF(27)
    st.d    $s5,   $sp, REG_OFF(28)
    st.d    $s6,   $sp, REG_OFF(29)
    st.d    $s7,   $sp, REG_OFF(30)
    st.d    $s8,   $sp, REG_OFF(31)

    fst.d   $f0,  $sp, FP_OFF(0)
    fst.d   $f1,  $sp, FP_OFF(1)
    fst.d   $f2,  $sp, FP_OFF(2)
    fst.d   $f3,  $sp, FP_OFF(3)
    fst.d   $f4,  $sp, FP_OFF(4)
    fst.d   $f5,  $sp, FP_OFF(5)
    fst.d   $f6,  $sp, FP_OFF(6)
    fst.d   $f7,  $sp, FP_OFF(7)
    fst.d   $f8,  $sp, FP_OFF(8)
    fst.d   $f9,  $sp, FP_OFF(9)
    fst.d   $f10, $sp, FP_OFF(10)
    fst.d   $f11, $sp, FP_OFF(11)
    fst.d   $f12, $sp, FP_OFF(12)
    fst.d   $f13, $sp, FP_OFF(13)
    fst.d   $f14, $sp, FP_OFF(14)
    fst.d   $f15, $sp, FP_OFF(15)
    fst.d   $f16, $sp, FP_OFF(16)
    fst.d   $f17, $sp, FP_OFF(17)
    fst.d   $f18, $sp, FP_OFF(18)
    fst.d   $f19, $sp, FP_OFF(19)
    fst.d   $f20, $sp, FP_OFF(20)
    fst.d   $f21, $sp, FP_OFF(21)
    fst.d   $f22, $sp, FP_OFF(22)
    fst.d   $f23, $sp, FP_OFF(23)
    fst.d   $f24, $sp, FP_OFF(24)
    fst.d   $f25, $sp, FP_OFF(25)
    fst.d   $f26, $sp, FP_OFF(26)
    fst.d   $f27, $sp, FP_OFF(27)
    fst.d   $f28, $sp, FP_OFF(28)
    fst.d   $f29, $sp, FP_OFF(29)
    fst.d   $f30, $sp, FP_OFF(30)
    fst.d   $f31, $sp, FP_OFF(31)
    movfcsr2gr $t0, $fcsr0
    st.d    $t0, $sp, FCSR_OFF

    csrrd   $t0, 0x6
    st.d    $t0, $sp, ERA_OFF
    csrrd   $t1, 0x1
    st.d    $t1, $sp, PRMD_OFF

    /* Kernel sp = current sp + CTX_SIZE (the pre-trap value) */
    addi.d  $t2, $sp, CTX_SIZE
    st.d    $t2, $sp, REG_OFF(3)    /* regs[3] = kernel sp */
    st.d    $tp, $sp, KTP_OFF       /* kernel tp from $tp */
    st.d    $t2, $sp, KSP_OFF

    move    $a0, $sp
    bl      trap_handler_la64
    b       .Lrestore_and_return

/* ---- Trap entry from user mode (PLV3 → PLV0) ----
 * On entry: $sp = user stack, SAVE0 = kernel sp, SAVE1 = task tp.
 *
 * Strategy:
 *   1. Swap $sp ← SAVE0 (kernel sp), save old $sp (user sp) to SAVE1
 *      (SAVE1's task tp is no longer needed — we read it first into a temp)
 *   2. Build trap context on kernel stack
 *   3. Set kernel_tp from saved tp
 */
    .align 4
__trap_from_user:
    csrwr   $sp, 0x30               /* SAVE0 = user sp, $sp = kernel sp */

    /* Now we are on the kernel stack! Allocate trap frame. */
    addi.d  $sp, $sp, -CTX_SIZE

    /* Retrieve original $t0 from TLBRSAVE (saved in trap_entry_la64),
     * then save both $t0 and $t1 so we can use them as temporaries */
    csrrd   $t0, 0x8B
    st.d    $t0, $sp, REG_OFF(12)
    st.d    $t1, $sp, REG_OFF(13)

    /* Read back user sp from SAVE0 into $t0, save to regs[3] */
    csrrd   $t0, 0x30
    st.d    $t0, $sp, REG_OFF(3)    /* regs[3] = user sp */

    /* Read task tp from SAVE1 into $t1, save to KTP_OFF */
    csrrd   $t1, 0x31
    st.d    $t1, $sp, KTP_OFF

    /* Now save all other GPRs */
    st.d    $zero, $sp, REG_OFF(0)
    st.d    $ra,   $sp, REG_OFF(1)
    st.d    $tp,   $sp, REG_OFF(2)
    /* REG_OFF(3) already saved */
    st.d    $a0,   $sp, REG_OFF(4)
    st.d    $a1,   $sp, REG_OFF(5)
    st.d    $a2,   $sp, REG_OFF(6)
    st.d    $a3,   $sp, REG_OFF(7)
    st.d    $a4,   $sp, REG_OFF(8)
    st.d    $a5,   $sp, REG_OFF(9)
    st.d    $a6,   $sp, REG_OFF(10)
    st.d    $a7,   $sp, REG_OFF(11)
    /* $t0 and $t1 already saved */
    st.d    $t2,   $sp, REG_OFF(14)
    st.d    $t3,   $sp, REG_OFF(15)
    st.d    $t4,   $sp, REG_OFF(16)
    st.d    $t5,   $sp, REG_OFF(17)
    st.d    $t6,   $sp, REG_OFF(18)
    st.d    $t7,   $sp, REG_OFF(19)
    st.d    $t8,   $sp, REG_OFF(20)

    st.d    $r21,  $sp, REG_OFF(21)
    st.d    $fp,   $sp, REG_OFF(22)
    st.d    $s0,   $sp, REG_OFF(23)
    st.d    $s1,   $sp, REG_OFF(24)
    st.d    $s2,   $sp, REG_OFF(25)
    st.d    $s3,   $sp, REG_OFF(26)
    st.d    $s4,   $sp, REG_OFF(27)
    st.d    $s5,   $sp, REG_OFF(28)
    st.d    $s6,   $sp, REG_OFF(29)
    st.d    $s7,   $sp, REG_OFF(30)
    st.d    $s8,   $sp, REG_OFF(31)

    fst.d   $f0,  $sp, FP_OFF(0)
    fst.d   $f1,  $sp, FP_OFF(1)
    fst.d   $f2,  $sp, FP_OFF(2)
    fst.d   $f3,  $sp, FP_OFF(3)
    fst.d   $f4,  $sp, FP_OFF(4)
    fst.d   $f5,  $sp, FP_OFF(5)
    fst.d   $f6,  $sp, FP_OFF(6)
    fst.d   $f7,  $sp, FP_OFF(7)
    fst.d   $f8,  $sp, FP_OFF(8)
    fst.d   $f9,  $sp, FP_OFF(9)
    fst.d   $f10, $sp, FP_OFF(10)
    fst.d   $f11, $sp, FP_OFF(11)
    fst.d   $f12, $sp, FP_OFF(12)
    fst.d   $f13, $sp, FP_OFF(13)
    fst.d   $f14, $sp, FP_OFF(14)
    fst.d   $f15, $sp, FP_OFF(15)
    fst.d   $f16, $sp, FP_OFF(16)
    fst.d   $f17, $sp, FP_OFF(17)
    fst.d   $f18, $sp, FP_OFF(18)
    fst.d   $f19, $sp, FP_OFF(19)
    fst.d   $f20, $sp, FP_OFF(20)
    fst.d   $f21, $sp, FP_OFF(21)
    fst.d   $f22, $sp, FP_OFF(22)
    fst.d   $f23, $sp, FP_OFF(23)
    fst.d   $f24, $sp, FP_OFF(24)
    fst.d   $f25, $sp, FP_OFF(25)
    fst.d   $f26, $sp, FP_OFF(26)
    fst.d   $f27, $sp, FP_OFF(27)
    fst.d   $f28, $sp, FP_OFF(28)
    fst.d   $f29, $sp, FP_OFF(29)
    fst.d   $f30, $sp, FP_OFF(30)
    fst.d   $f31, $sp, FP_OFF(31)
    movfcsr2gr $t0, $fcsr0
    st.d    $t0, $sp, FCSR_OFF

    csrrd   $t0, 0x6
    st.d    $t0, $sp, ERA_OFF
    csrrd   $t1, 0x1
    st.d    $t1, $sp, PRMD_OFF

    /* kernel_sp = current sp + CTX_SIZE (base of trap frame) */
    addi.d  $t2, $sp, CTX_SIZE
    st.d    $t2, $sp, KSP_OFF

    /* Load task tp into $tp for the C handler */
    ld.d    $tp, $sp, KTP_OFF

    move    $a0, $sp
    bl      trap_handler_la64
    /* fall through to restore */

    .global user_trap_return

/* ---- Restore and return to user/kernel ---- */
.Lrestore_and_return:
__return_to_user:
user_trap_return:
    /* Restore PGDL (page table base) from regs[0] / KScratch0.
     * This is critical after exec(): proc_exec sets regs[0] to the
     * new page directory's satp value, but the PGDL CSR still points
     * to the old (now freed) page directory.  Without this, TLB
     * refills after ertn would walk freed page tables. */
    ld.d    $t0, $sp, REG_OFF(0)
    csrrd   $t1, 0x19              /* current PGDL */
    bne     $t0, $t1, .Lflush_tlb
    csrwr   $t0, 0x19              /* PGDL unchanged, skip flush */
    b       .Lpgdl_done
.Lflush_tlb:
    csrwr   $t0, 0x19              /* PGDL */
    invtlb  0x3, $zero, $zero
.Lpgdl_done:

    ld.d    $t0, $sp, ERA_OFF
    csrwr   $t0, 0x6               /* ERA */
    ld.d    $t1, $sp, PRMD_OFF
    csrwr   $t1, 0x1               /* PRMD */

    /* Set up SAVE0/SAVE1 for next user→kernel trap:
     * SAVE0 = kernel sp (to restore on next trap)
     * SAVE1 = task tp  (to identify the task on next trap)
     */
    addi.d  $t2, $sp, CTX_SIZE
    csrwr   $t2, 0x30              /* SAVE0 = kernel sp */

    ld.d    $t3, $sp, KTP_OFF
    csrwr   $t3, 0x31              /* SAVE1 = task tp */

    /* Restore GPRs (except $sp — that's done last) */
    ld.d    $ra,  $sp, REG_OFF(1)
    ld.d    $tp,  $sp, REG_OFF(2)
    /* Skip REG_OFF(3) = sp; restore it after deallocating */
    ld.d    $a0,  $sp, REG_OFF(4)
    ld.d    $a1,  $sp, REG_OFF(5)
    ld.d    $a2,  $sp, REG_OFF(6)
    ld.d    $a3,  $sp, REG_OFF(7)
    ld.d    $a4,  $sp, REG_OFF(8)
    ld.d    $a5,  $sp, REG_OFF(9)
    ld.d    $a6,  $sp, REG_OFF(10)
    ld.d    $a7,  $sp, REG_OFF(11)
    ld.d    $t0,  $sp, REG_OFF(12)
    ld.d    $t1,  $sp, REG_OFF(13)
    ld.d    $t2,  $sp, REG_OFF(14)
    ld.d    $t3,  $sp, REG_OFF(15)
    ld.d    $t4,  $sp, REG_OFF(16)
    ld.d    $t5,  $sp, REG_OFF(17)
    ld.d    $t6,  $sp, REG_OFF(18)
    ld.d    $t7,  $sp, REG_OFF(19)
    ld.d    $t8,  $sp, REG_OFF(20)
    ld.d    $r21, $sp, REG_OFF(21)
    ld.d    $fp,  $sp, REG_OFF(22)
    ld.d    $s0,  $sp, REG_OFF(23)
    ld.d    $s1,  $sp, REG_OFF(24)
    ld.d    $s2,  $sp, REG_OFF(25)
    ld.d    $s3,  $sp, REG_OFF(26)
    ld.d    $s4,  $sp, REG_OFF(27)
    ld.d    $s5,  $sp, REG_OFF(28)
    ld.d    $s6,  $sp, REG_OFF(29)
    ld.d    $s7,  $sp, REG_OFF(30)
    ld.d    $s8,  $sp, REG_OFF(31)

    fld.d   $f0,  $sp, FP_OFF(0)
    fld.d   $f1,  $sp, FP_OFF(1)
    fld.d   $f2,  $sp, FP_OFF(2)
    fld.d   $f3,  $sp, FP_OFF(3)
    fld.d   $f4,  $sp, FP_OFF(4)
    fld.d   $f5,  $sp, FP_OFF(5)
    fld.d   $f6,  $sp, FP_OFF(6)
    fld.d   $f7,  $sp, FP_OFF(7)
    fld.d   $f8,  $sp, FP_OFF(8)
    fld.d   $f9,  $sp, FP_OFF(9)
    fld.d   $f10, $sp, FP_OFF(10)
    fld.d   $f11, $sp, FP_OFF(11)
    fld.d   $f12, $sp, FP_OFF(12)
    fld.d   $f13, $sp, FP_OFF(13)
    fld.d   $f14, $sp, FP_OFF(14)
    fld.d   $f15, $sp, FP_OFF(15)
    fld.d   $f16, $sp, FP_OFF(16)
    fld.d   $f17, $sp, FP_OFF(17)
    fld.d   $f18, $sp, FP_OFF(18)
    fld.d   $f19, $sp, FP_OFF(19)
    fld.d   $f20, $sp, FP_OFF(20)
    fld.d   $f21, $sp, FP_OFF(21)
    fld.d   $f22, $sp, FP_OFF(22)
    fld.d   $f23, $sp, FP_OFF(23)
    fld.d   $f24, $sp, FP_OFF(24)
    fld.d   $f25, $sp, FP_OFF(25)
    fld.d   $f26, $sp, FP_OFF(26)
    fld.d   $f27, $sp, FP_OFF(27)
    fld.d   $f28, $sp, FP_OFF(28)
    fld.d   $f29, $sp, FP_OFF(29)
    fld.d   $f30, $sp, FP_OFF(30)
    fld.d   $f31, $sp, FP_OFF(31)
    ld.d    $t0, $sp, FCSR_OFF
    movgr2fcsr $fcsr0, $t0

    /* Now restore user/kernel sp.
     * For user return: regs[3] = user sp.
     * For kernel return: regs[3] = kernel sp.
     * Deallocate trap frame, then load sp.
     */
    ld.d    $sp, $sp, REG_OFF(3)   /* $sp = saved (user or kernel) sp */

    ertn

/* ---- TLB Refill entry (CSR 0x88 = TLBRENTRY) ----
 *
 * 3-level page table walk using lddir + ldpte + tlbfill.
 */
    .align 12
tlb_refill_entry_la64:
    csrwr   $r21, 0x8B             /* TLBRSAVE = save $r21 (reserved, not used by user) */

    csrrd   $r21, 0x19             /* $r21 = PGDL */
    beqz    $r21, .Ltlb_miss

    lddir   $r21, $r21, 2
    beqz    $r21, .Ltlb_miss
    srli.d  $r21, $r21, 12
    slli.d  $r21, $r21, 12

    lddir   $r21, $r21, 1
    beqz    $r21, .Ltlb_miss
    srli.d  $r21, $r21, 12
    slli.d  $r21, $r21, 12

    ldpte   $r21, 0
    ldpte   $r21, 1
    tlbfill

    csrrd   $r21, 0x8B             /* restore $r21 */
    ertn

.Ltlb_miss:
    csrwr   $zero, 0x8C
    csrwr   $zero, 0x8D
    tlbfill
    csrrd   $r21, 0x8B             /* restore $r21 */
    ertn

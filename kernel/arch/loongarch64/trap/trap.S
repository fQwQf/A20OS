/*
 * LoongArch64 — Trap / Exception / TLB Refill Handling
 *
 * Trap context layout (trap_context_t, 102*8 = 816 bytes):
 *   [0..31]  = regs[32]   (32 GPRs; $r0=$zero, $r1=$ra, $r2=$tp, $r3=$sp, ...)
 *   [32]     = era        (Exception Return Address)
 *   [33]     = prmd       (Previous CRMD — PPLV[1:0], PIE[2])
 *   [34]     = kernel_tp  (task pointer for kernel use)
 *   [35]     = kernel_sp  (kernel stack pointer for return-to-user)
 *   [36..99] = vr[32][2]  (128-bit LSX registers; low halves alias FP regs)
 *   [100]    = fcsr0      (floating-point control/status)
 *   [101]    = packed FCC0..FCC7
 *
 * CSR usage for user↔kernel transitions:
 *   SAVE0 (0x30) = kernel stack pointer
 *   SAVE1 (0x31) = task pointer (tp)
 *   SAVE2 (0x32) = temporary save for original $t0 on general traps
 *
 * General exception entry = trap_entry_la64 (CSR EENTRY = 0xC)
 * TLB refill entry        = tlb_refill_entry_la64 (CSR TLBRENTRY = 0x88)
 *
 * Task context layout (task_context_t, 15*8 = 120 bytes):
 *   [0]  ra     [1] tp    [2..10] s0-s8   [11] fp
 *   [12] sp     [13] pgdl  [14] prmd
 */

#include "loongarch64_asm.h"
#include "mmu_layout.h"

#define CTX_SIZE    (102 * 8)
#define REG_OFF(n)  ((n) * 8)
#define ERA_OFF     (32 * 8)
#define PRMD_OFF    (33 * 8)
#define KTP_OFF     (34 * 8)
#define KSP_OFF     (35 * 8)
#define VR_OFF(n)   ((36 + ((n) * 2)) * 8)
#define FCSR_OFF    (100 * 8)
#define FCC_OFF     (101 * 8)

    .section .text
    .align 4
    .global trap_entry_la64
    .global tlb_refill_entry_la64
    .global __trap_from_kernel
    .global __trap_from_user
    .global __return_to_user
    .global user_trap_return
    .global arch_idle_wait
    .global loongarch64_idle_region_start
    .global loongarch64_idle_region_end

#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT) && \
    defined(CONFIG_LS2K_TRAP_TRACE)
    /* Return/trap checkpoints for physical-board userspace bring-up.  Callers
     * place these only after the interrupted t0/t1 values are safely stored. */
    .macro LS2K_TRAP_MARK value
    li.d    $t0, 0x800000001fe20000
1:
    ld.bu   $t1, $t0, 5
    andi    $t1, $t1, 0x20
    beqz    $t1, 1b
    li.w    $t1, \value
    st.b    $t1, $t0, 0
    .endm

    /* The refill handler cannot call C code and may fail before the regular
     * exception vector is usable.  Preserve its two diagnostic temporaries in
     * otherwise-unused KSAVE registers so single-byte checkpoints can identify
     * the exact page-walk operation reached on physical hardware. */
    .macro LS2K_REFILL_MARK value
    /* TLB refill runs with direct physical addressing, before a translated
     * DMW alias is guaranteed to be usable. */
    li.d    $t0, 0x1fe20000
1:
    ld.bu   $t1, $t0, 5
    andi    $t1, $t1, 0x20
    beqz    $t1, 1b
    li.w    $t1, \value
    st.b    $t1, $t0, 0
    .endm
#else
    .macro LS2K_TRAP_MARK value
    .endm

    .macro LS2K_REFILL_MARK value
    .endm
#endif

/* Entered with CRMD.IE clear and returns with it clear. If an interrupt is
 * recognized after IE is set but before idle executes, trap_bridge.c advances
 * ERA to loongarch64_idle_region_end so ertn cannot execute a stale idle. */
    .align 4
arch_idle_wait:
    csrrd   $t0, 0x0
    ori     $t1, $t0, 0x4
    csrwr   $t1, 0x0
loongarch64_idle_region_start:
    idle    0
loongarch64_idle_region_end:
    csrrd   $t0, 0x0
    li.d    $t1, -5
    and     $t0, $t0, $t1
    csrwr   $t0, 0x0
    jirl    $zero, $ra, 0

/* ---- General exception entry ----
 * Dispatched to from CSR EENTRY (0xC).
 * On entry: CRMD.PPLV = previous PLV, ERA = fault PC.
 * If PPLV==3 (user mode): sp = user sp, SAVE0 = kernel sp, SAVE1 = task tp.
 * If PPLV==0 (kernel mode): sp = kernel sp.
 */
#ifdef CONFIG_BOARD_LS2K1000
    /* LS2K1000 clears EENTRY[11:0].  Keep this physical-board-only so the
     * QEMU exception layout and object code remain unchanged. */
    .section .trap_entry, "ax"
    .p2align 12
#endif
trap_entry_la64:
    /* Save original $t0 before we clobber it.  TLBRSAVE belongs to the TLB
     * refill path; general traps use SAVE2 so interrupts cannot corrupt user
     * temporaries between adjacent user instructions. */
    csrwr   $t0, 0x32             /* SAVE2 = original $t0 */

    /* Read PRMD.PPLV to decide user vs kernel trap path */
    csrrd   $t0, 0x1              /* PRMD */
    andi    $t0, $t0, 0x3         /* PPLV bits [1:0] */
    bnez    $t0, __trap_from_user /* PPLV != 0 → came from user */
    b       __trap_from_kernel
#ifdef CONFIG_BOARD_LS2K1000
    .section .text
#endif

/* ---- Trap entry from kernel mode (PLV0 → PLV0) ----
 * sp is already the kernel stack.
 * SAVE0/SAVE1 may contain stale user-mode values — don't touch them.
 */
    .align 4
__trap_from_kernel:
    addi.d  $sp, $sp, -CTX_SIZE

    /*
     * regs[0] is repurposed by the generic trap-return path as the address
     * space token.  Preserve PGDL here; zeroing it would make the next return
     * path install PGDL=0 and strand user mode after the first kernel trap.
     */
    csrrd   $t0, 0x19
    st.d    $t0, $sp, REG_OFF(0)
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
    /* Retrieve original $t0 from SAVE2 (saved in trap_entry_la64) */
    csrrd   $t0, 0x32
    st.d    $t0,   $sp, REG_OFF(12)
    st.d    $t1,   $sp, REG_OFF(13)
    LS2K_TRAP_MARK 75              /* K: kernel exception frame active */
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

    vst     $vr0,  $sp, VR_OFF(0)
    vst     $vr1,  $sp, VR_OFF(1)
    vst     $vr2,  $sp, VR_OFF(2)
    vst     $vr3,  $sp, VR_OFF(3)
    vst     $vr4,  $sp, VR_OFF(4)
    vst     $vr5,  $sp, VR_OFF(5)
    vst     $vr6,  $sp, VR_OFF(6)
    vst     $vr7,  $sp, VR_OFF(7)
    vst     $vr8,  $sp, VR_OFF(8)
    vst     $vr9,  $sp, VR_OFF(9)
    vst     $vr10, $sp, VR_OFF(10)
    vst     $vr11, $sp, VR_OFF(11)
    vst     $vr12, $sp, VR_OFF(12)
    vst     $vr13, $sp, VR_OFF(13)
    vst     $vr14, $sp, VR_OFF(14)
    vst     $vr15, $sp, VR_OFF(15)
    vst     $vr16, $sp, VR_OFF(16)
    vst     $vr17, $sp, VR_OFF(17)
    vst     $vr18, $sp, VR_OFF(18)
    vst     $vr19, $sp, VR_OFF(19)
    vst     $vr20, $sp, VR_OFF(20)
    vst     $vr21, $sp, VR_OFF(21)
    vst     $vr22, $sp, VR_OFF(22)
    vst     $vr23, $sp, VR_OFF(23)
    vst     $vr24, $sp, VR_OFF(24)
    vst     $vr25, $sp, VR_OFF(25)
    vst     $vr26, $sp, VR_OFF(26)
    vst     $vr27, $sp, VR_OFF(27)
    vst     $vr28, $sp, VR_OFF(28)
    vst     $vr29, $sp, VR_OFF(29)
    vst     $vr30, $sp, VR_OFF(30)
    vst     $vr31, $sp, VR_OFF(31)
    movfcsr2gr $t0, $fcsr0
    st.d    $t0, $sp, FCSR_OFF
    movcf2gr $t0, $fcc0
    movcf2gr $t1, $fcc1
    slli.d  $t1, $t1, 1
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc2
    slli.d  $t1, $t1, 2
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc3
    slli.d  $t1, $t1, 3
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc4
    slli.d  $t1, $t1, 4
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc5
    slli.d  $t1, $t1, 5
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc6
    slli.d  $t1, $t1, 6
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc7
    slli.d  $t1, $t1, 7
    or      $t0, $t0, $t1
    st.d    $t0, $sp, FCC_OFF

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

    /* Retrieve original $t0 from SAVE2 (saved in trap_entry_la64),
     * then save both $t0 and $t1 so we can use them as temporaries */
    csrrd   $t0, 0x32
    st.d    $t0, $sp, REG_OFF(12)
    st.d    $t1, $sp, REG_OFF(13)
    LS2K_TRAP_MARK 85              /* U: user exception frame active */
#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT) && \
    defined(CONFIG_LS2K_TRAP_TRACE)
    csrrd   $t0, 0x5               /* ESTAT.Ecode */
    srli.d  $t0, $t0, 16
    andi    $t0, $t0, 0x3f
    li.w    $t1, 11                /* SYS from PLV3 */
    bne     $t0, $t1, .Lls2k_non_sys_trap
    LS2K_TRAP_MARK 115             /* s: expected first syscall */
    b       .Lls2k_trap_cause_done
.Lls2k_non_sys_trap:
    LS2K_TRAP_MARK 101             /* e: another exception code */
.Lls2k_trap_cause_done:
#endif

    /* Read back user sp from SAVE0 into $t0, save to regs[3] */
    csrrd   $t0, 0x30
    st.d    $t0, $sp, REG_OFF(3)    /* regs[3] = user sp */

    /* Read task tp from SAVE1 into $t1, save to KTP_OFF */
    csrrd   $t1, 0x31
    st.d    $t1, $sp, KTP_OFF

    /* Now save all other GPRs */
    /*
     * regs[0] carries the active PGDL through trap handling.  Unlike a real
     * GPR zero register, this slot must survive every user trap so syscalls
     * and interrupts can return to the same user page table.
     */
    csrrd   $t0, 0x19
    st.d    $t0, $sp, REG_OFF(0)
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
    LS2K_TRAP_MARK 86              /* V: all user GPRs saved */

    vst     $vr0,  $sp, VR_OFF(0)
    vst     $vr1,  $sp, VR_OFF(1)
    vst     $vr2,  $sp, VR_OFF(2)
    vst     $vr3,  $sp, VR_OFF(3)
    vst     $vr4,  $sp, VR_OFF(4)
    vst     $vr5,  $sp, VR_OFF(5)
    vst     $vr6,  $sp, VR_OFF(6)
    vst     $vr7,  $sp, VR_OFF(7)
    vst     $vr8,  $sp, VR_OFF(8)
    vst     $vr9,  $sp, VR_OFF(9)
    vst     $vr10, $sp, VR_OFF(10)
    vst     $vr11, $sp, VR_OFF(11)
    vst     $vr12, $sp, VR_OFF(12)
    vst     $vr13, $sp, VR_OFF(13)
    vst     $vr14, $sp, VR_OFF(14)
    vst     $vr15, $sp, VR_OFF(15)
    vst     $vr16, $sp, VR_OFF(16)
    vst     $vr17, $sp, VR_OFF(17)
    vst     $vr18, $sp, VR_OFF(18)
    vst     $vr19, $sp, VR_OFF(19)
    vst     $vr20, $sp, VR_OFF(20)
    vst     $vr21, $sp, VR_OFF(21)
    vst     $vr22, $sp, VR_OFF(22)
    vst     $vr23, $sp, VR_OFF(23)
    vst     $vr24, $sp, VR_OFF(24)
    vst     $vr25, $sp, VR_OFF(25)
    vst     $vr26, $sp, VR_OFF(26)
    vst     $vr27, $sp, VR_OFF(27)
    vst     $vr28, $sp, VR_OFF(28)
    vst     $vr29, $sp, VR_OFF(29)
    vst     $vr30, $sp, VR_OFF(30)
    vst     $vr31, $sp, VR_OFF(31)
    LS2K_TRAP_MARK 87              /* W: LSX state saved */
    movfcsr2gr $t0, $fcsr0
    st.d    $t0, $sp, FCSR_OFF
    movcf2gr $t0, $fcc0
    movcf2gr $t1, $fcc1
    slli.d  $t1, $t1, 1
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc2
    slli.d  $t1, $t1, 2
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc3
    slli.d  $t1, $t1, 3
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc4
    slli.d  $t1, $t1, 4
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc5
    slli.d  $t1, $t1, 5
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc6
    slli.d  $t1, $t1, 6
    or      $t0, $t0, $t1
    movcf2gr $t1, $fcc7
    slli.d  $t1, $t1, 7
    or      $t0, $t0, $t1
    st.d    $t0, $sp, FCC_OFF
    LS2K_TRAP_MARK 89              /* Y: scalar FP state saved */

    csrrd   $t0, 0x6
    st.d    $t0, $sp, ERA_OFF
    csrrd   $t1, 0x1
    st.d    $t1, $sp, PRMD_OFF

    /* kernel_sp = current sp + CTX_SIZE (base of trap frame) */
    addi.d  $t2, $sp, CTX_SIZE
    st.d    $t2, $sp, KSP_OFF

    /* Load task tp into $tp for the C handler */
    ld.d    $tp, $sp, KTP_OFF

    LS2K_TRAP_MARK 90              /* Z: entering C trap handler */
    move    $a0, $sp
    bl      trap_handler_la64
    /* fall through to restore */

    .global user_trap_return

/* ---- Restore and return to user/kernel ---- */
.Lrestore_and_return:
__return_to_user:
user_trap_return:
    LS2K_TRAP_MARK 73              /* I: return frame selected */
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
    LS2K_TRAP_MARK 74              /* J: user PGDL installed */

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
    LS2K_TRAP_MARK 75              /* K: return CSRs installed */

    /* Restore scalar FP and LSX state before GPRs. */
    vld     $vr0,  $sp, VR_OFF(0)
    vld     $vr1,  $sp, VR_OFF(1)
    vld     $vr2,  $sp, VR_OFF(2)
    vld     $vr3,  $sp, VR_OFF(3)
    vld     $vr4,  $sp, VR_OFF(4)
    vld     $vr5,  $sp, VR_OFF(5)
    vld     $vr6,  $sp, VR_OFF(6)
    vld     $vr7,  $sp, VR_OFF(7)
    vld     $vr8,  $sp, VR_OFF(8)
    vld     $vr9,  $sp, VR_OFF(9)
    vld     $vr10, $sp, VR_OFF(10)
    vld     $vr11, $sp, VR_OFF(11)
    vld     $vr12, $sp, VR_OFF(12)
    vld     $vr13, $sp, VR_OFF(13)
    vld     $vr14, $sp, VR_OFF(14)
    vld     $vr15, $sp, VR_OFF(15)
    vld     $vr16, $sp, VR_OFF(16)
    vld     $vr17, $sp, VR_OFF(17)
    vld     $vr18, $sp, VR_OFF(18)
    vld     $vr19, $sp, VR_OFF(19)
    vld     $vr20, $sp, VR_OFF(20)
    vld     $vr21, $sp, VR_OFF(21)
    vld     $vr22, $sp, VR_OFF(22)
    vld     $vr23, $sp, VR_OFF(23)
    vld     $vr24, $sp, VR_OFF(24)
    vld     $vr25, $sp, VR_OFF(25)
    vld     $vr26, $sp, VR_OFF(26)
    vld     $vr27, $sp, VR_OFF(27)
    vld     $vr28, $sp, VR_OFF(28)
    vld     $vr29, $sp, VR_OFF(29)
    vld     $vr30, $sp, VR_OFF(30)
    vld     $vr31, $sp, VR_OFF(31)
    ld.d    $t0, $sp, FCSR_OFF
    movgr2fcsr $fcsr0, $t0
    ld.d    $t0, $sp, FCC_OFF
    movgr2cf $fcc0, $t0
    srli.d  $t1, $t0, 1
    movgr2cf $fcc1, $t1
    srli.d  $t1, $t0, 2
    movgr2cf $fcc2, $t1
    srli.d  $t1, $t0, 3
    movgr2cf $fcc3, $t1
    srli.d  $t1, $t0, 4
    movgr2cf $fcc4, $t1
    srli.d  $t1, $t0, 5
    movgr2cf $fcc5, $t1
    srli.d  $t1, $t0, 6
    movgr2cf $fcc6, $t1
    srli.d  $t1, $t0, 7
    movgr2cf $fcc7, $t1
    LS2K_TRAP_MARK 76              /* L: vector state restored */

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

    LS2K_TRAP_MARK 77              /* M: ready to execute ertn */
    /* The marker uses caller temporaries; restore their user values again. */
    ld.d    $t0,  $sp, REG_OFF(12)
    ld.d    $t1,  $sp, REG_OFF(13)

    /* Now restore user/kernel sp.
     * For user return: regs[3] = user sp.
     * For kernel return: regs[3] = kernel sp.
     * Deallocate trap frame, then load sp.
     */
    ld.d    $sp, $sp, REG_OFF(3)   /* $sp = saved (user or kernel) sp */

    ertn

    .global arch_return_to_user
arch_return_to_user:
    move    $sp, $a0
    b       __return_to_user

/* ---- TLB Refill entry (CSR 0x88 = TLBRENTRY) ---- */
    /* Keep the refill vector in its own section.  The physical-board linker
     * script places this section at a 4 KiB boundary, which TLBRENTRY requires
     * and linker relaxation cannot move. */
    .section .tlb_refill, "ax"
    .p2align 12
tlb_refill_entry_la64:
    csrwr   $r21, 0x8B             /* TLBRSAVE = save $r21 (reserved, not used by user) */
#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT)
    csrwr   $t0, 0x33              /* SAVE3 = interrupted $t0 */
    csrwr   $t1, 0x34              /* SAVE4 = interrupted $t1 */
#endif
    LS2K_REFILL_MARK 78            /* N: refill vector entered */

    /* CSR.PGD selects PGDL/PGDH from TLBRBADV.  Both profiles start at Dir3;
     * only the LS2K1000LA profile has a non-folded Dir2. */
    csrrd   $r21, 0x1B             /* $r21 = selected PGD */
    beqz    $r21, .Ltlb_miss
    LS2K_REFILL_MARK 79            /* O: root selected */

    lddir   $r21, $r21, 3
    beqz    $r21, .Ltlb_miss
    bstrins.d $r21, $zero, 11, 0   /* directory entries carry software flags */
    LS2K_REFILL_MARK 80            /* P: Dir3 loaded */

#if LA64_PT_HAS_DIR2
    lddir   $r21, $r21, 2
    beqz    $r21, .Ltlb_miss
    bstrins.d $r21, $zero, 11, 0
    LS2K_REFILL_MARK 81            /* Q: Dir2 loaded */
#endif

    lddir   $r21, $r21, 1
    beqz    $r21, .Ltlb_miss
    bstrins.d $r21, $zero, 11, 0
    LS2K_REFILL_MARK 82            /* R: Dir1 loaded */

    ldpte   $r21, 0
    ldpte   $r21, 1
    LS2K_REFILL_MARK 83            /* S: leaf pair loaded */
    tlbfill
    LS2K_REFILL_MARK 84            /* T: refill installed */

#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT)
    csrrd   $t0, 0x33
    csrrd   $t1, 0x34
#endif
    csrrd   $r21, 0x8B             /* restore $r21 */
    ertn

.Ltlb_miss:
    LS2K_REFILL_MARK 88            /* X: missing directory entry */
    /*
     * ldpte normally installs the 4 KiB page size in TLBREHI.  An empty
     * intermediate directory branches here before ldpte, so set PS explicitly
     * while preserving the refill VPPN.  Without a valid PS the invalid entry
     * does not cover TLBRBADV and the CPU immediately refills again instead of
     * raising the regular PIL/PIS/PIF exception for demand paging.
     */
    csrrd   $r21, 0x8E             /* TLBREHI */
    srli.d  $r21, $r21, 6
    slli.d  $r21, $r21, 6          /* clear PS[5:0], preserve VPPN */
    ori     $r21, $r21, 12         /* PS = 4 KiB */
    csrwr   $r21, 0x8E
    csrwr   $zero, 0x8C
    csrwr   $zero, 0x8D
    tlbfill
#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT)
    csrrd   $t0, 0x33
    csrrd   $t1, 0x34
#endif
    csrrd   $r21, 0x8B             /* restore $r21 */
    ertn

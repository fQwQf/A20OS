/*
 * LoongArch32 — Trap / Exception / Software TLB Refill Handling
 *
 * Trap context layout (trap_context_t, 36*4 = 144 bytes):
 *   [0..31]  = regs[32]   (32 GPRs; $r0=$zero, $r1=$ra, $r2=$tp, $r3=$sp, ...)
 *   [32]     = era        (Exception Return Address)
 *   [33]     = prmd       (Previous CRMD — PPLV[1:0], PIE[2])
 *   [34]     = kernel_tp  (task pointer for kernel use)
 *   [35]     = kernel_sp  (kernel stack pointer for return-to-user)
 *
 * CSR usage for user↔kernel transitions:
 *   SAVE0 (0x30) = kernel stack pointer
 *   SAVE1 (0x31) = task pointer (tp)
 *   SAVE2 (0x32) = temporary save for original $t0 on general traps
 *   SAVE3 (0x33) = temporary save for $r21 on TLB refill
 *
 * General exception entry = trap_entry_la32 (CSR EENTRY = 0xC)
 * TLB refill entry        = tlb_refill_entry_la32 (CSR TLBRENTRY = 0x88)
 *
 * Task context layout (task_context_t, 15*4 = 60 bytes):
 *   [0]  ra     [1] tp    [2..10] s0-s8   [11] fp
 *   [12] sp     [13] pgdl  [14] prmd
 */

#define CTX_SIZE    (36 * 4)
#define REG_OFF(n)  ((n) * 4)
#define ERA_OFF     (32 * 4)
#define PRMD_OFF    (33 * 4)
#define KTP_OFF     (34 * 4)
#define KSP_OFF     (35 * 4)

/* EENTRY / TLBRENTRY zero the low 6 address bits, so both exception entry
 * points must land on a 64-byte boundary.  The la32 binutils `.balign` is
 * unreliable (off-by-four), so compute the padding explicitly. */
.macro ALIGN_TO_64
    .zero ((. + 63) & -64) - .
.endm

    .section .trapcode
    /* Force the whole section to a 64-byte-aligned base so the EENTRY /
     * TLBRENTRY targets below stay 64-byte aligned after linking. */
    .p2align 6
    .align 4
    .global trap_entry_la32
    .global tlb_refill_entry_la32
    .global __trap_from_kernel
    .global __trap_from_user
    .global __return_to_user
    .global user_trap_return
    .global arch_idle_wait

/* Entered with CRMD.IE clear.  The core advances ERA past `idle` on its own
 * when an interrupt is taken while idling, so no lost-wakeup fix-up is needed
 * here (unlike LA64 where it must be handled in C). */
    .align 4
arch_idle_wait:
    csrrd   $t0, 0x0
    ori     $t1, $t0, 0x4
    csrwr   $t1, 0x0
    idle    0
    csrrd   $t0, 0x0
    li.w    $t1, -5
    and     $t0, $t0, $t1
    csrwr   $t0, 0x0
    jirl    $zero, $ra, 0

/* ---- General exception entry ----
 * Dispatched to from CSR EENTRY (0xC).
 * On entry: CRMD.PPLV = previous PLV, ERA = fault PC.
 * If PPLV==3 (user mode): sp = user sp, SAVE0 = kernel sp, SAVE1 = task tp.
 * If PPLV==0 (kernel mode): sp = kernel sp.
 */
    ALIGN_TO_64
trap_entry_la32:
    /* Save original $t0 before we clobber it. */
    csrwr   $t0, 0x32             /* SAVE2 = original $t0 */

    /* Read PRMD.PPLV to decide user vs kernel trap path */
    csrrd   $t0, 0x1              /* PRMD */
    andi    $t0, $t0, 0x3         /* PPLV bits [1:0] */
    bnez    $t0, __trap_from_user /* PPLV != 0 → came from user */
    b       __trap_from_kernel

/* ---- Trap entry from kernel mode (PLV0 → PLV0) ---- */
    .align 4
__trap_from_kernel:
    addi.w  $sp, $sp, -CTX_SIZE

    csrrd   $t0, 0x19
    st.w    $t0, $sp, REG_OFF(0)   /* regs[0] = PGDL (addr space token) */
    st.w    $ra,   $sp, REG_OFF(1)
    st.w    $tp,   $sp, REG_OFF(2)
    st.w    $a0,   $sp, REG_OFF(4)
    st.w    $a1,   $sp, REG_OFF(5)
    st.w    $a2,   $sp, REG_OFF(6)
    st.w    $a3,   $sp, REG_OFF(7)
    st.w    $a4,   $sp, REG_OFF(8)
    st.w    $a5,   $sp, REG_OFF(9)
    st.w    $a6,   $sp, REG_OFF(10)
    st.w    $a7,   $sp, REG_OFF(11)
    csrrd   $t0, 0x32
    st.w    $t0,   $sp, REG_OFF(12)
    st.w    $t1,   $sp, REG_OFF(13)
    st.w    $t2,   $sp, REG_OFF(14)
    st.w    $t3,   $sp, REG_OFF(15)
    st.w    $t4,   $sp, REG_OFF(16)
    st.w    $t5,   $sp, REG_OFF(17)
    st.w    $t6,   $sp, REG_OFF(18)
    st.w    $t7,   $sp, REG_OFF(19)
    st.w    $t8,   $sp, REG_OFF(20)
    st.w    $r21,  $sp, REG_OFF(21)
    st.w    $fp,   $sp, REG_OFF(22)
    st.w    $s0,   $sp, REG_OFF(23)
    st.w    $s1,   $sp, REG_OFF(24)
    st.w    $s2,   $sp, REG_OFF(25)
    st.w    $s3,   $sp, REG_OFF(26)
    st.w    $s4,   $sp, REG_OFF(27)
    st.w    $s5,   $sp, REG_OFF(28)
    st.w    $s6,   $sp, REG_OFF(29)
    st.w    $s7,   $sp, REG_OFF(30)
    st.w    $s8,   $sp, REG_OFF(31)

    csrrd   $t0, 0x6
    st.w    $t0, $sp, ERA_OFF
    csrrd   $t1, 0x1
    st.w    $t1, $sp, PRMD_OFF

    addi.w  $t2, $sp, CTX_SIZE
    st.w    $t2, $sp, REG_OFF(3)   /* regs[3] = kernel sp */
    st.w    $tp, $sp, KTP_OFF
    st.w    $t2, $sp, KSP_OFF

    move    $a0, $sp
    bl      trap_handler_la32
    b       .Lrestore_and_return

/* ---- Trap entry from user mode (PLV3 → PLV0) ---- */
    .align 4
__trap_from_user:
    csrwr   $sp, 0x30               /* SAVE0 = user sp, $sp = kernel sp */
    addi.w  $sp, $sp, -CTX_SIZE

    csrrd   $t0, 0x32
    st.w    $t0, $sp, REG_OFF(12)
    st.w    $t1, $sp, REG_OFF(13)

    csrrd   $t0, 0x30
    st.w    $t0, $sp, REG_OFF(3)    /* regs[3] = user sp */

    csrrd   $t1, 0x31
    st.w    $t1, $sp, KTP_OFF

    csrrd   $t0, 0x19
    st.w    $t0, $sp, REG_OFF(0)    /* regs[0] = PGDL */
    st.w    $ra,   $sp, REG_OFF(1)
    st.w    $tp,   $sp, REG_OFF(2)
    st.w    $a0,   $sp, REG_OFF(4)
    st.w    $a1,   $sp, REG_OFF(5)
    st.w    $a2,   $sp, REG_OFF(6)
    st.w    $a3,   $sp, REG_OFF(7)
    st.w    $a4,   $sp, REG_OFF(8)
    st.w    $a5,   $sp, REG_OFF(9)
    st.w    $a6,   $sp, REG_OFF(10)
    st.w    $a7,   $sp, REG_OFF(11)
    st.w    $t2,   $sp, REG_OFF(14)
    st.w    $t3,   $sp, REG_OFF(15)
    st.w    $t4,   $sp, REG_OFF(16)
    st.w    $t5,   $sp, REG_OFF(17)
    st.w    $t6,   $sp, REG_OFF(18)
    st.w    $t7,   $sp, REG_OFF(19)
    st.w    $t8,   $sp, REG_OFF(20)
    st.w    $r21,  $sp, REG_OFF(21)
    st.w    $fp,   $sp, REG_OFF(22)
    st.w    $s0,   $sp, REG_OFF(23)
    st.w    $s1,   $sp, REG_OFF(24)
    st.w    $s2,   $sp, REG_OFF(25)
    st.w    $s3,   $sp, REG_OFF(26)
    st.w    $s4,   $sp, REG_OFF(27)
    st.w    $s5,   $sp, REG_OFF(28)
    st.w    $s6,   $sp, REG_OFF(29)
    st.w    $s7,   $sp, REG_OFF(30)
    st.w    $s8,   $sp, REG_OFF(31)

    csrrd   $t0, 0x6
    st.w    $t0, $sp, ERA_OFF
    csrrd   $t1, 0x1
    st.w    $t1, $sp, PRMD_OFF

    addi.w  $t2, $sp, CTX_SIZE
    st.w    $t2, $sp, KSP_OFF

    ld.w    $tp, $sp, KTP_OFF

    move    $a0, $sp
    bl      trap_handler_la32
    /* fall through to restore */

    .global user_trap_return

/* ---- Restore and return to user/kernel ---- */
.Lrestore_and_return:
__return_to_user:
user_trap_return:
    /* Restore PGDL (page table base) from regs[0].  Critical after exec():
     * the common code puts the new pgdir token here. */
    ld.w    $t0, $sp, REG_OFF(0)
    csrrd   $t1, 0x19              /* current PGDL */
    bne     $t0, $t1, .Lflush_tlb
    csrwr   $t0, 0x19              /* unchanged, skip flush */
    b       .Lpgdl_done
.Lflush_tlb:
    csrwr   $t0, 0x19
    invtlb  0x3, $zero, $zero
.Lpgdl_done:

    ld.w    $t0, $sp, ERA_OFF
    csrwr   $t0, 0x6               /* ERA */
    ld.w    $t1, $sp, PRMD_OFF
    csrwr   $t1, 0x1               /* PRMD */

    addi.w  $t2, $sp, CTX_SIZE
    csrwr   $t2, 0x30              /* SAVE0 = kernel sp */
    ld.w    $t3, $sp, KTP_OFF
    csrwr   $t3, 0x31              /* SAVE1 = task tp */

    ld.w    $ra,  $sp, REG_OFF(1)
    ld.w    $tp,  $sp, REG_OFF(2)
    ld.w    $a0,  $sp, REG_OFF(4)
    ld.w    $a1,  $sp, REG_OFF(5)
    ld.w    $a2,  $sp, REG_OFF(6)
    ld.w    $a3,  $sp, REG_OFF(7)
    ld.w    $a4,  $sp, REG_OFF(8)
    ld.w    $a5,  $sp, REG_OFF(9)
    ld.w    $a6,  $sp, REG_OFF(10)
    ld.w    $a7,  $sp, REG_OFF(11)
    ld.w    $t0,  $sp, REG_OFF(12)
    ld.w    $t1,  $sp, REG_OFF(13)
    ld.w    $t2,  $sp, REG_OFF(14)
    ld.w    $t3,  $sp, REG_OFF(15)
    ld.w    $t4,  $sp, REG_OFF(16)
    ld.w    $t5,  $sp, REG_OFF(17)
    ld.w    $t6,  $sp, REG_OFF(18)
    ld.w    $t7,  $sp, REG_OFF(19)
    ld.w    $t8,  $sp, REG_OFF(20)
    ld.w    $r21, $sp, REG_OFF(21)
    ld.w    $fp,  $sp, REG_OFF(22)
    ld.w    $s0,  $sp, REG_OFF(23)
    ld.w    $s1,  $sp, REG_OFF(24)
    ld.w    $s2,  $sp, REG_OFF(25)
    ld.w    $s3,  $sp, REG_OFF(26)
    ld.w    $s4,  $sp, REG_OFF(27)
    ld.w    $s5,  $sp, REG_OFF(28)
    ld.w    $s6,  $sp, REG_OFF(29)
    ld.w    $s7,  $sp, REG_OFF(30)
    ld.w    $s8,  $sp, REG_OFF(31)

    ld.w    $sp, $sp, REG_OFF(3)   /* restore user or kernel sp */
    ertn

    .global arch_return_to_user
arch_return_to_user:
    move    $sp, $a0
    b       __return_to_user

/* ---- TLB Refill entry (CSR 0x88 = TLBRENTRY) ----
 *
 * The NaiLoong Core has no hardware page-table walker, so on a TLB miss it
 * raises ecode 0x3F, switches CRMD to DA mode (physical addressing), records
 * the faulting VPN in TLBEHI, and jumps here.  We do a 2-level software walk
 * (level 1 = PGD via PGDL, level 0 = PTE), load the even/odd page PTEs into
 * TLBELO0/1, and tlbfill.  On ertn the core re-enables paging.
 *
 * A miss that finds no valid PTE still tlbfills an invalid (V=0) entry so
 * the re-executed access raises the precise PIL/PIS/PIF fault.
 */
    ALIGN_TO_64
tlb_refill_entry_la32:
    csrwr   $r21, 0x33             /* SAVE3 = $r21 */

    /* vppn = TLBEHI = va[31:13]; vpn20 = va[31:12] = vppn << 1 (bit12 = 0) */
    csrrd   $r21, 0x11
    slli.w  $r21, $r21, 1

    /* pgd_idx = (vpn20 >> 10) & 0x3FF = va[31:22] */
    srli.w  $t0, $r21, 10
    andi    $t0, $t0, 0x3FF

    /* pgd_base = PGDL (physical address of root table) */
    csrrd   $t1, 0x19
    beqz    $t1, .Lrefill_miss

    /* pgd_entry = *(uint32_t *)(pgd_base + pgd_idx * 4) */
    slli.w  $t0, $t0, 2
    add.w   $t1, $t1, $t0
    ld.w    $t1, $t1, 0
    andi    $t0, $t1, 1             /* V bit */
    beqz    $t0, .Lrefill_miss

    /* pte_base = pgd_entry & ~0xFFF (physical address of PTE table) */
    srli.w  $t1, $t1, 12
    slli.w  $t1, $t1, 12
    beqz    $t1, .Lrefill_miss

    /* pte_idx_even = vpn20 & 0x3FE (bit0 = 0), then ×4 for the byte offset */
    andi    $t0, $r21, 0x3FE
    slli.w  $t0, $t0, 2
    add.w   $t1, $t1, $t0

    ld.w    $t2, $t1, 0             /* pte0 (even page) */
    ld.w    $t3, $t1, 4             /* pte1 (odd page) */

    /* TLBELO = ((pte >> 12) << 8) | (pte & 0x7F)
     *   ppn [31:12] → [27:8]; keep V/D/PLV/MAT/G in [6:0]. */
    srli.w  $t4, $t2, 12
    slli.w  $t4, $t4, 8
    andi    $t5, $t2, 0x7F
    or      $t4, $t4, $t5
    csrwr   $t4, 0x12               /* TLBELO0 */

    srli.w  $t4, $t3, 12
    slli.w  $t4, $t4, 8
    andi    $t5, $t3, 0x7F
    or      $t4, $t4, $t5
    csrwr   $t4, 0x13               /* TLBELO1 */

    tlbfill

    csrrd   $r21, 0x33
    ertn

.Lrefill_miss:
    csrwr   $zero, 0x12
    csrwr   $zero, 0x13
    tlbfill
    csrrd   $r21, 0x33
    ertn

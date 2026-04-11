/* LoongArch64 — Trap / Exception Handling
 *
 * Handles:
 *   - General exceptions (ECFG-based dispatch)
 *   - System call (SYSCALL instruction, ecode=0xB)
 *   - TLB refill (TLBR, separate exception entry)
 *   - Timer interrupt (HWI0)
 *   - UART interrupt (HWI1)
 *
 * CSR layout:
 *   ERA  = exception return address (= PC of faulting instruction for most)
 *   PRMD = saved CRMD at exception entry
 *   ESTAT = exception cause bits
 *
 * Register save/restore: all 32 GPRs on kernel stack.
 */

#include "loongarch64_asm.h"

/* Exception context size = 32 GPRs * 8 bytes = 256 bytes */
#define CTX_SIZE    (32 * 8 + 8 + 8)   /* regs + era + prmd */
#define REG_OFF(n)  ((n) * 8)
#define ERA_OFF     (32 * 8)
#define PRMD_OFF    (33 * 8)

    .section .text
    .align 4
    .global trap_entry_la64
    .global tlb_refill_entry_la64

/* ---- General exception entry ---- */
trap_entry_la64:
    /* Save all GPRs to kernel stack */
    addi.d  $sp, $sp, -CTX_SIZE

    /* Save all 32 GPRs ($r0..$r31 → $zero..$ra, etc.) */
    st.d    $zero, $sp, REG_OFF(0)
    st.d    $ra,   $sp, REG_OFF(1)
    st.d    $tp,   $sp, REG_OFF(2)
    st.d    $sp,   $sp, REG_OFF(3)
    st.d    $a0,   $sp, REG_OFF(4)
    st.d    $a1,   $sp, REG_OFF(5)
    st.d    $a2,   $sp, REG_OFF(6)
    st.d    $a3,   $sp, REG_OFF(7)
    st.d    $a4,   $sp, REG_OFF(8)
    st.d    $a5,   $sp, REG_OFF(9)
    st.d    $a6,   $sp, REG_OFF(10)
    st.d    $a7,   $sp, REG_OFF(11)
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

    /* Save ERA and PRMD */
    csrrd   $t0, 0x6    /* ERA */
    st.d    $t0, $sp, ERA_OFF
    csrrd   $t1, 0x1    /* PRMD */
    st.d    $t1, $sp, PRMD_OFF

    /* Call C handler: trap_handler_la64(ctx) */
    move    $a0, $sp
    bl      trap_handler_la64

    /* Restore ERA and PRMD */
    ld.d    $t0, $sp, ERA_OFF
    csrwr   $t0, 0x6
    ld.d    $t1, $sp, PRMD_OFF
    csrwr   $t1, 0x1

    /* Restore all GPRs */
    ld.d    $ra,  $sp, REG_OFF(1)
    ld.d    $tp,  $sp, REG_OFF(2)
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

    addi.d  $sp, $sp, CTX_SIZE
    ertn                /* exception return */

/* ---- TLB Refill entry (CSR.TLBRERA) ---- */
    .align 4
tlb_refill_entry_la64:
    /* Save $t0, $t1 to KSave0/1 */
    csrwr   $t0, 0x30   /* KSave0 */
    csrwr   $t1, 0x31   /* KSave1 */

    /* Read bad VA → use PGD from CRMD/PGD CSR */
    csrrd   $t0, 0x7    /* BADV */

    /* For simplicity: invoke C TLB miss handler */
    /* Save all registers and call C code */
    csrrd   $t0, 0x30
    csrrd   $t1, 0x31
    ertn

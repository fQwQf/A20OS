/*
 * LoongArch64 context switch
 * void __switch(uint64_t next_kstack)
 *
 * $a0 = next_kstack: kernel stack pointer of the next task
 *                     (points to its task_context_t)
 *
 * tp = current task pointer (set by context_switch before calling)
 *
 * Layout: save callee-saved regs + PGDL + PRMD to current stack,
 *         store current sp into current_task->kstack via tp,
 *         then restore from next_kstack.
 */

    .section .text
    .global __switch_la64
    .global __switch

__switch_la64:
__switch:
    /* $a0 = next_kstack (sp of next task, pointing at task_context_t) */
    /* $tp = current task pointer */

    /* Allocate task context on current stack (120 bytes = 15*8) */
    addi.d  $sp, $sp, -120

    /* Save callee-saved registers */
    st.d    $ra, $sp, 0
    st.d    $tp, $sp, 8
    st.d    $s0, $sp, 16
    st.d    $s1, $sp, 24
    st.d    $s2, $sp, 32
    st.d    $s3, $sp, 40
    st.d    $s4, $sp, 48
    st.d    $s5, $sp, 56
    st.d    $s6, $sp, 64
    st.d    $s7, $sp, 72
    st.d    $s8, $sp, 80
    st.d    $fp, $sp, 88

    /* Save sp at offset 96 (the adjusted sp after -120) */
    st.d    $sp, $sp, 96

    /* Save PGDL (CSR 0x19) at offset 104 */
    csrrd   $t0, 0x19
    st.d    $t0, $sp, 104

    /* Save PRMD (CSR 0x1) at offset 112 */
    csrrd   $t1, 0x1
    st.d    $t1, $sp, 112

    /* Store current sp into current_task->kstack (tp points to task) */
    st.d    $sp, $tp, 0

    /* Switch to next task's stack */
    move    $sp, $a0

    /* Restore callee-saved registers */
    ld.d    $ra, $sp, 0
    ld.d    $tp, $sp, 8
    ld.d    $s0, $sp, 16
    ld.d    $s1, $sp, 24
    ld.d    $s2, $sp, 32
    ld.d    $s3, $sp, 40
    ld.d    $s4, $sp, 48
    ld.d    $s5, $sp, 56
    ld.d    $s6, $sp, 64
    ld.d    $s7, $sp, 72
    ld.d    $s8, $sp, 80
    ld.d    $fp, $sp, 88

    /* Restore PGDL (CSR 0x19) from offset 104 */
    ld.d    $t0, $sp, 104
    csrwr   $t0, 0x19

    /* Restore PRMD (CSR 0x1) from offset 112 */
    ld.d    $t1, $sp, 112
    csrwr   $t1, 0x1

    /* Flush non-global TLB entries after page table switch */
    invtlb  0x3, $zero, $zero

    /* Free task context from stack */
    addi.d  $sp, $sp, 120

    /* Return to ra of next task */
    jirl    $zero, $ra, 0

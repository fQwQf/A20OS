/*
 * LoongArch32 context switch
 * void __switch(uint32_t next_kstack)
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
    .global __switch_la32
    .global __switch

__switch_la32:
__switch:
    /* $a0 = next_kstack (sp of next task, pointing at task_context_t) */
    /* $tp = current task pointer */

    /* Allocate task context on current stack (60 bytes = 15*4) */
    addi.w  $sp, $sp, -60

    /* Save callee-saved registers */
    st.w    $ra, $sp, 0
    st.w    $tp, $sp, 4
    st.w    $s0, $sp, 8
    st.w    $s1, $sp, 12
    st.w    $s2, $sp, 16
    st.w    $s3, $sp, 20
    st.w    $s4, $sp, 24
    st.w    $s5, $sp, 28
    st.w    $s6, $sp, 32
    st.w    $s7, $sp, 36
    st.w    $s8, $sp, 40
    st.w    $fp, $sp, 44

    /* Save sp at offset 48 (the adjusted sp after -60) */
    st.w    $sp, $sp, 48

    /* Save PGDL (CSR 0x19) at offset 52 */
    csrrd   $t0, 0x19
    st.w    $t0, $sp, 52

    /* Save PRMD (CSR 0x1) at offset 56 */
    csrrd   $t1, 0x1
    st.w    $t1, $sp, 56

    /* Store current sp into current_task->kstack (tp points to task) */
    st.w    $sp, $tp, 0

    /* Switch to next task's stack */
    move    $sp, $a0

    /* Restore callee-saved registers */
    ld.w    $ra, $sp, 0
    ld.w    $tp, $sp, 4
    ld.w    $s0, $sp, 8
    ld.w    $s1, $sp, 12
    ld.w    $s2, $sp, 16
    ld.w    $s3, $sp, 20
    ld.w    $s4, $sp, 24
    ld.w    $s5, $sp, 28
    ld.w    $s6, $sp, 32
    ld.w    $s7, $sp, 36
    ld.w    $s8, $sp, 40
    ld.w    $fp, $sp, 44

    /* Restore PGDL (CSR 0x19) from offset 52 */
    ld.w    $t0, $sp, 52
    csrwr   $t0, 0x19

    /* Restore PRMD (CSR 0x1) from offset 56 */
    ld.w    $t1, $sp, 56
    csrwr   $t1, 0x1

    /* Flush non-global TLB entries after page table switch */
    invtlb  0x3, $zero, $zero

    /* Free task context from stack */
    addi.w  $sp, $sp, 60

    /* Return to ra of next task */
    jirl    $zero, $ra, 0

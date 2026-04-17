/* LoongArch64 context switch
 * void __switch_la64(uint64_t *old_sp, uint64_t new_sp)
 *
 * Saves callee-saved registers (s0-s8, ra, fp, sp) to *old_sp,
 * then restores from new_sp.
 *
 * LoongArch callee-saved: $s0-$s8, $fp, $ra, $sp
 * (ABI: $s0=$r23, $s1=$r24, ..., $s8=$r31, $fp=$r22, $ra=$r1)
 */

    .section .text
    .global __switch_la64

__switch_la64:
    /* $a0 = old_sp (pointer to save current sp into) */
    /* $a1 = new_sp (sp of next task) */

    /* Push callee-saved regs + ra onto current stack */
    addi.d  $sp, $sp, -(14 * 8)
    st.d    $ra, $sp, 0
    st.d    $fp, $sp, 8
    st.d    $s0, $sp, 16
    st.d    $s1, $sp, 24
    st.d    $s2, $sp, 32
    st.d    $s3, $sp, 40
    st.d    $s4, $sp, 48
    st.d    $s5, $sp, 56
    st.d    $s6, $sp, 64
    st.d    $s7, $sp, 72
    st.d    $s8, $sp, 80
    st.d    $tp, $sp, 88    /* save current task pointer */

    /* Save stack pointer to old_sp */
    st.d    $sp, $a0, 0

    /* Load new stack pointer */
    ld.d    $sp, $a1, 0
    /* Or: move $sp, $a1  (if a1 IS the sp, not a pointer) */
    /* Actually in our design a1 = new task_context pointer which IS sp */
    move    $sp, $a1

    /* Restore callee-saved regs from new stack */
    ld.d    $ra, $sp, 0
    ld.d    $fp, $sp, 8
    ld.d    $s0, $sp, 16
    ld.d    $s1, $sp, 24
    ld.d    $s2, $sp, 32
    ld.d    $s3, $sp, 40
    ld.d    $s4, $sp, 48
    ld.d    $s5, $sp, 56
    ld.d    $s6, $sp, 64
    ld.d    $s7, $sp, 72
    ld.d    $s8, $sp, 80
    ld.d    $tp, $sp, 88    /* restore task pointer */

    addi.d  $sp, $sp, (14 * 8)
    jirl    $zero, $ra, 0   /* return to ra of next task */

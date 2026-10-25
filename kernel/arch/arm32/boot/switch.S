    .syntax unified
    .arch armv7-a
    .fpu vfpv3-d16

/*
 * IRQ, prefetch-abort, data-abort and FIQ all run their vector prologue and
 * the 224-byte trap_context_t on a per-CPU banked entry stack. trap.S then
 * migrates the frame to the task's SVC stack before calling schedulable C.
 * Each bank has 4 KiB; keep the prologue and GIC acknowledge inside it.
 */
    .equ ARM32_TRAP_FRAME_SIZE, 224
    .equ ARM32_BANKED_SP_RESERVE, 4096
    .if ARM32_TRAP_FRAME_SIZE + 16 > ARM32_BANKED_SP_RESERVE
    .error "arm32 banked SP reserve is smaller than the trap frame it must hold"
    .endif

    .section .text
    .global __switch
__switch:
    /*
     * r0 / r1 = next task's kstack pointer (uint64_t argument, low 32 bits in r0)
     * TPIDRPRW still points to the *previous* task because context_switch()
     * restored it after publishing the new task.
     *
     * Save the previous task's kernel context as a task_context_t on its
     * kernel stack, store the new stack pointer into prev->kstack, then load
     * the next task's context and resume it.
     */
    cpsid   i

    /* r1 = previous task pointer */
    mrc     p15, 0, r1, c13, c0, 4

    /* GPR/TLS state plus AAPCS callee-saved VFP registers and FPSCR. */
    sub     sp, sp, #128

    /* task_context_t.ra = lr */
    str     lr, [sp, #0]
    /* task_context_t.tp = previous task pointer */
    str     r1, [sp, #4]
    /* task_context_t.r4-r11 */
    str     r4, [sp, #8]
    str     r5, [sp, #12]
    str     r6, [sp, #16]
    str     r7, [sp, #20]
    str     r8, [sp, #24]
    str     r9, [sp, #28]
    str     r10, [sp, #32]
    str     r11, [sp, #36]
    /* task_context_t.sp = stack pointer before this context was saved */
    add     r2, sp, #128
    str     r2, [sp, #40]
    /* task_context_t.ttbr0 */
#ifndef CONFIG_NOMMU
    mrc     p15, 0, r2, c2, c0, 0
#else
    mov     r2, #0
#endif
    str     r2, [sp, #44]
    /* task_context_t.cpsr (keep IRQs disabled for the resumed path) */
    mrs     r2, cpsr
    orr     r2, r2, #0x80
    str     r2, [sp, #48]
    /* task_context_t.user_tp (user-visible TLS pointer from TPIDRURO) */
    mrc     p15, 0, r2, c13, c0, 3
    str     r2, [sp, #52]

    add     r2, sp, #56
    vstmia  r2, {d8-d15}
    vmrs    r2, fpscr
    str     r2, [sp, #120]

    /* prev->kstack = sp */
    str     sp, [r1]

    /* Switch to the next task's kernel stack. */
    mov     sp, r0

    /* Load the next task's context. */
    add     r2, sp, #56
    vldmia  r2, {d8-d15}
    ldr     r2, [sp, #120]
    vmsr    fpscr, r2
    ldr     r1, [sp, #4]
    mcr     p15, 0, r1, c13, c0, 4
    ldr     r4, [sp, #8]
    ldr     r5, [sp, #12]
    ldr     r6, [sp, #16]
    ldr     r7, [sp, #20]
    ldr     r8, [sp, #24]
    ldr     r9, [sp, #28]
    ldr     r10, [sp, #32]
    ldr     r11, [sp, #36]
    ldr     r2, [sp, #40]
    ldr     r3, [sp, #44]
#ifndef CONFIG_NOMMU
    /* Two tasks that share a page table cannot have left anything stale
     * behind, so only an actual TTBR0 change earns the invalidation. */
    mrc     p15, 0, r0, c2, c0, 0
    cmp     r3, r0
    beq     .Lttbr0_unchanged
    mcr     p15, 0, r3, c2, c0, 0
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0
    dsb     ish
    isb
.Lttbr0_unchanged:
#endif
    ldr     r3, [sp, #48]
    ldr     r0, [sp, #52]
    mcr     p15, 0, r0, c13, c0, 3
    /* LR is banked between SVC and IRQ. Keep the target return address in
     * an unbanked caller-saved register until the target mode is installed;
     * otherwise a timer-driven IRQ -> SVC switch returns through stale LR. */
    ldr     r0, [sp, #0]

    /* If the target context is for a user task (CPSR mode == USR), stay in
     * SVC mode so that lr (user_trap_return) runs with kernel privileges.
     * user_trap_return will use the trap frame's CPSR to drop to user mode. */
    and     r12, r3, #0x1f
    cmp     r12, #0x10
    beq     1f

    /* Banked stacks hold entry scratch only; C handlers migrate to SVC.
     * Fixed per-CPU areas cannot overlap a deep task stack. r1 is unbanked
     * even in FIQ mode (r8-r14 are banked there). */
    mrc     p15, 0, r1, c0, c0, 5
    and     r1, r1, #255
    ldr     r12, =arm32_exception_stacks
    add     r1, r12, r1, lsl #14
    cps     #0x12
    add     sp, r1, #4096
    cps     #0x17
    add     sp, r1, #8192
    cps     #0x1B
    add     sp, r1, #12288
    cps     #0x11
    add     sp, r1, #16384
    cps     #0x13
    msr     cpsr_cxsf, r3
    mov     sp, r2
    mov     lr, r0
    bx      lr

1:
    /* Banked stacks hold entry scratch only; C handlers migrate to SVC.
     * Fixed per-CPU areas cannot overlap a deep task stack. r1 is unbanked
     * even in FIQ mode (r8-r14 are banked there). */
    mrc     p15, 0, r1, c0, c0, 5
    and     r1, r1, #255
    ldr     r12, =arm32_exception_stacks
    add     r1, r12, r1, lsl #14
    cps     #0x12
    add     sp, r1, #4096
    cps     #0x17
    add     sp, r1, #8192
    cps     #0x1B
    add     sp, r1, #12288
    cps     #0x11
    add     sp, r1, #16384
    cps     #0x13
    mov     sp, r2
    mov     lr, r0
    bx      lr

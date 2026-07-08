    .syntax unified
    .arch armv7-a

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

    /* Make room for task_context_t (14 * 4 = 56 bytes) on current stack. */
    sub     sp, sp, #56

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
    add     r2, sp, #56
    str     r2, [sp, #40]
    /* task_context_t.ttbr0 */
    mrc     p15, 0, r2, c2, c0, 0
    str     r2, [sp, #44]
    /* task_context_t.cpsr (keep IRQs disabled for the resumed path) */
    mrs     r2, cpsr
    orr     r2, r2, #0x80
    str     r2, [sp, #48]
    /* task_context_t.user_tp (user-visible TLS pointer from TPIDRURO) */
    mrc     p15, 0, r2, c13, c0, 3
    str     r2, [sp, #52]

    /* prev->kstack = sp */
    str     sp, [r1]

    /* Switch to the next task's kernel stack. */
    mov     sp, r0

    /* Load the next task's context. */
    ldr     lr, [sp, #0]
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
    mcr     p15, 0, r3, c2, c0, 0
    mov     r1, #0
    mcr     p15, 0, r1, c8, c7, 0
    dsb     ish
    isb
    ldr     r3, [sp, #48]
    ldr     r0, [sp, #52]            /* user_tp; r0 is free after stack switch */

    /* If the target context is for a user task (CPSR mode == USR), stay in
     * SVC mode so that lr (user_trap_return) runs with kernel privileges.
     * user_trap_return will use the trap frame's CPSR to drop to user mode. */
    and     r12, r3, #0x1f
    cmp     r12, #0x10
    beq     1f

    /* Keep the privileged exception-mode stack pointers in sync with the
     * next task's SVC stack so IRQ/abort handlers always run on a valid
     * kernel stack.  r2 holds the next SVC sp; r3 holds the target CPSR. */
    mov     r12, r2
    sub     r12, r12, #4096
    cps     #0x12                    /* IRQ mode */
    mov     sp, r12
    cps     #0x17                    /* ABT mode */
    mov     sp, r12
    cps     #0x1B                    /* UND mode */
    mov     sp, r12
    cps     #0x11                    /* FIQ mode */
    mov     sp, r12
    cps     #0x13                    /* SVC mode */
    msr     cpsr_cxsf, r3
    mov     sp, r2

    bx      lr

1:
    mov     r12, r2
    sub     r12, r12, #4096
    cps     #0x12
    mov     sp, r12
    cps     #0x17
    mov     sp, r12
    cps     #0x1B
    mov     sp, r12
    cps     #0x11
    mov     sp, r12
    cps     #0x13
    mov     sp, r2
    /* Restore the user-visible TLS pointer before entering user mode. */
    mcr     p15, 0, r0, c13, c0, 3
    bx      lr

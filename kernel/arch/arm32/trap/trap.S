    .syntax unified
    .arch armv7-a

    .equ MODE_USR, 0x10
    .equ MODE_SVC, 0x13
    .equ TASK_TRAP_CTX,         32

    .equ TF_SP,                 52
    .equ TF_LR,                 56
    .equ TF_PC,                 60
    .equ TF_CPSR,               64
    .equ TF_KERNEL_TP,          68
    .equ TF_KERNEL_SP,          72
    .equ TF_TTBR0,              76
    .equ TF_FAULT_ADDR,         80
    .equ TF_SIZE,               84

    /* Generic cause codes matching kernel/core/trap.h expectations. */
    .equ CAUSE_ECALL_U,          11
    .equ CAUSE_INSN_PAGE_FAULT,  12
    .equ CAUSE_LOAD_PAGE_FAULT,  13
    .equ CAUSE_STORE_PAGE_FAULT, 14
    .equ CAUSE_INTR_MASK,        (1 << 31)

    .section .text
    .align 5
    .global arm32_vector_table
arm32_vector_table:
    b reset_vector
    b undef_vector
    b svc_vector
    b pabort_vector
    b dabort_vector
    b reserved_vector
    b irq_vector
    b fiq_vector

reset_vector:
undef_vector:
reserved_vector:
fiq_vector:
    b .

svc_vector:
    sub     sp, sp, #8
    stmia   sp, {r0, r1}
    ldr     r0, =CAUSE_ECALL_U
    ldr     r1, =arm32_trap_flags
    str     r0, [r1]
    ldmia   sp, {r0, r1}
    add     sp, sp, #8
    b       trap_common

pabort_vector:
    sub     sp, sp, #8
    stmia   sp, {r0, r1}
    ldr     r0, =CAUSE_INSN_PAGE_FAULT
    ldr     r1, =arm32_trap_flags
    str     r0, [r1]
    ldmia   sp, {r0, r1}
    add     sp, sp, #8
    b       trap_common

dabort_vector:
    /* Distinguish load vs store via DFSR WnR (bit 11). */
    sub     sp, sp, #12
    stmia   sp, {r0-r2}
    mrc     p15, 0, r1, c5, c0, 0       /* DFSR */
    tst     r1, #(1 << 11)
    ldrne   r0, =CAUSE_STORE_PAGE_FAULT
    ldreq   r0, =CAUSE_LOAD_PAGE_FAULT
    ldr     r2, =arm32_trap_flags
    str     r0, [r2]
    ldmia   sp, {r0-r2}
    add     sp, sp, #12
    b       trap_common

irq_vector:
    /* r0 will be replaced below with CAUSE_INTR_MASK | irq. */
    sub     sp, sp, #8
    stmia   sp, {r0, r1}
    ldr     r0, =CAUSE_INTR_MASK
    ldr     r1, =arm32_trap_flags
    str     r0, [r1]
    ldmia   sp, {r0, r1}
    add     sp, sp, #8
    b       trap_common

    /*
     * Common kernel/user trap entry.
     * On entry:
     *   r0 = generic cause code (for IRQ, will be overwritten after ack)
     *   lr = link register (return address)
     *   spsr = saved CPSR
     */
trap_common:
    /* Build the trap frame on the exception-mode stack.  The extra word keeps
     * the C-call stack 8-byte aligned while trap_context_t remains 84 bytes. */
    sub     sp, sp, #88
    stmia   sp, {r0-r12}
    add     r1, sp, #TF_SP             /* r1 -> trap.sp */

    /*
     * Capture the trapped context's sp/lr.  For a user-mode trap the CPU
     * entered an exception mode with a banked sp, so the user sp/lr must
     * be read from System mode (which shares the USR bank).
     */
    mrs     r4, spsr
    and     r4, r4, #0x1f
    cmp     r4, #MODE_USR
    bne     1f

    /* User trap: read user sp/lr from System mode. */
    mrs     r5, cpsr
    bic     r6, r5, #0x1f
    orr     r6, r6, #0x1f              /* System mode */
    msr     cpsr_cxsf, r6
    mov     r6, sp
    mov     r7, lr
    msr     cpsr_cxsf, r5
    str     r6, [r1]                   /* trap.sp */
    str     r7, [r1, #4]               /* trap.lr */
    b       2f

1:  /* Kernel trap: current sp is the kernel exception stack. */
    mov     r6, sp
    add     r6, r6, #88
    str     r6, [r1]                   /* trap.sp */
    str     lr, [r1, #4]               /* trap.lr */

2:
    /* Compute trap.pc from the exception link register.
     *   IRQ:               LR = PC+4            -> LR-4
     *   SVC / prefetch:    LR = next instr + 4  -> LR-4
     *   Data abort:        LR = next instr + 8  -> LR-8
     */
    ldr     r2, =arm32_trap_flags
    ldr     r2, [r2]                   /* cause */
    ldr     r3, =CAUSE_INTR_MASK
    tst     r2, r3
    subne   r2, lr, #4                 /* IRQ */
    bne     2f
    ldr     r3, =CAUSE_STORE_PAGE_FAULT
    cmp     r2, r3
    cmpne   r2, #CAUSE_LOAD_PAGE_FAULT
    subeq   r2, lr, #8                 /* data abort */
    subne   r2, lr, #4                 /* SVC / prefetch abort */
2:
    str     r2, [r1, #8]               /* trap.pc */

    /* Capture DFAR for data aborts. */
    ldr     r2, =CAUSE_STORE_PAGE_FAULT
    ldr     r3, =arm32_trap_flags
    ldr     r3, [r3]
    cmp     r3, r2
    cmpne   r3, #CAUSE_LOAD_PAGE_FAULT
    bne     3f
    mrc     p15, 0, r2, c6, c0, 0
    ldr     r3, =arm32_fault_addr
    str     r2, [r3]
3:
    ldr     r2, [r1, #8]
    ldr     r3, =arm32_fault_pc
    str     r2, [r3]

    mrs     r2, spsr
    str     r2, [r1, #12]              /* trap.cpsr */
    mrc     p15, 0, r2, c13, c0, 4
    str     r2, [r1, #16]              /* trap.kernel_tp */
/* NOMMU keeps trap.ttbr0 as zero and never touches TTBR0/TLB state. */
#ifndef CONFIG_NOMMU
    mrc     p15, 0, r2, c2, c0, 0
#else
    mov     r2, #0
#endif
    str     r2, [r1, #24]              /* trap.ttbr0 */
    mrc     p15, 0, r2, c13, c0, 3
    str     r2, [r1, #28]              /* user-visible TLS pointer */

    add     r2, sp, #88
    mrs     r3, spsr
    and     r3, r3, #0x1f
    cmp     r3, #MODE_USR
    bne     4f
    ldr     r3, [r1, #16]              /* trap.kernel_tp */
    ldr     r3, [r3, #TASK_TRAP_CTX]
    cmp     r3, #0
    ldrne   r2, [r3, #TF_KERNEL_SP]
4:
    str     r2, [r1, #20]              /* trap.kernel_sp */

    /* IRQ: ack and overwrite cause code with the real interrupt number. */
    ldr     r1, =arm32_trap_flags
    ldr     r1, [r1]
    ldr     r2, =CAUSE_INTR_MASK
    tst     r1, r2
    beq     5f
    push    {r0-r3, lr}
    bl      arm32_gic_ack
    ldr     r2, =CAUSE_INTR_MASK
    orr     r0, r0, r2
    ldr     r2, =arm32_trap_flags
    str     r0, [r2]
    pop     {r0-r3, lr}
5:

    /* Record the cause for arch_read_cause(). */
    ldr     r2, =arm32_trap_flags
    ldr     r2, [r2]

    /* Dispatch to the appropriate C handler. */
    mrs     r2, spsr
    and     r2, r2, #0x1f
    cmp     r2, #MODE_USR
    beq     5f
    mov     r0, sp
    bl      kernel_trap_handler
    b       __return_to_user
5:
    ldr     r0, [sp, #TF_KERNEL_TP]
    ldr     r0, [r0, #TASK_TRAP_CTX]
    cmp     r0, #0
    moveq   r0, sp
    beq     6f
    ldr     r2, [r0, #TF_KERNEL_SP]    /* keep the persistent kernel stack top */
    cmp     r0, sp
    beq     6f
    mov     r3, sp
    ldmia   r3!, {r4-r11}
    stmia   r0!, {r4-r11}
    ldmia   r3!, {r4-r11}
    stmia   r0!, {r4-r11}
    ldmia   r3!, {r4-r8}
    stmia   r0!, {r4-r8}
    sub     r0, r0, #TF_SIZE
    str     r2, [r0, #TF_KERNEL_SP]
6:
    bl      trap_handler
    b       user_trap_return

    .global __trap_from_user
__trap_from_user:
    b svc_vector

    .global __trap_from_kernel
__trap_from_kernel:
    b svc_vector

    .global __return_to_user
    .global user_trap_return
__return_to_user:
    ldr     r1, [sp, #TF_CPSR]
    and     r1, r1, #0x1f
    cmp     r1, #MODE_USR
    beq     9f

    /* Kernel-mode return uses the dynamic exception frame already on sp. */
    ldr     lr, [sp, #TF_PC]
    ldr     r1, [sp, #TF_CPSR]
    msr     spsr_cxsf, r1
    ldr     r1, [sp, #TF_KERNEL_TP]
    mcr     p15, 0, r1, c13, c0, 4
#ifndef CONFIG_NOMMU
    ldr     r1, [sp, #TF_TTBR0]
    mcr     p15, 0, r1, c2, c0, 0
    mov     r1, #0
    mcr     p15, 0, r1, c8, c7, 0
    dsb     ish
    isb
#endif
    ldmia   sp, {r0-r12}
    add     sp, sp, #88                /* tear down the dynamic kernel trap frame */
    movs    pc, lr

9:
user_trap_return:
    mrc     p15, 0, r0, c13, c0, 4

10:
    /* User return always resumes from current->trap_ctx. */
    ldr     r0, [r0, #TASK_TRAP_CTX]
    ldr     r1, [r0, #TF_SP]
    ldr     r2, [r0, #TF_LR]
    ldr     lr, [r0, #TF_PC]
    ldr     r12, [r0, #TF_CPSR]
    msr     spsr_cxsf, r12
    ldr     r12, [r0, #TF_KERNEL_TP]
    mcr     p15, 0, r12, c13, c0, 4
    ldr     r12, [r0, #TF_FAULT_ADDR]
    mcr     p15, 0, r12, c13, c0, 3
#ifndef CONFIG_NOMMU
    ldr     r12, [r0, #TF_TTBR0]
    mcr     p15, 0, r12, c2, c0, 0
    mov     r12, #0
    mcr     p15, 0, r12, c8, c7, 0
    dsb     ish
    isb
#endif
    ldr     r12, [r0, #TF_KERNEL_SP]
    mrs     r3, cpsr
    cps     #0x1f                      /* System mode (USR register bank) */
    mov     sp, r1
    mov     lr, r2
    msr     cpsr_cxsf, r3              /* Back to the original exception mode */
    ldr     r3, [r0, #TF_CPSR]
    bic     r3, r3, #0x80              /* return to user with IRQs enabled */
    msr     spsr_cxsf, r3
    mov     sp, r12                    /* tear down any dynamic SVC trap frame */
    ldr     r1, [r0, #4]
    ldr     r2, [r0, #8]
    ldr     r4, [r0, #16]
    ldr     r5, [r0, #20]
    ldr     r6, [r0, #24]
    ldr     r7, [r0, #28]
    ldr     r8, [r0, #32]
    ldr     r9, [r0, #36]
    ldr     r10, [r0, #40]
    ldr     r11, [r0, #44]
    ldr     r3, [r0, #12]
    ldr     r12, [r0, #48]
    ldr     r0, [r0, #0]
    subs    pc, lr, #0

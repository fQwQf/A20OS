    .syntax unified
    .arch armv7-a
    .fpu vfpv3-d16

    .equ MODE_USR, 0x10
    .equ MODE_SVC, 0x13
    /* Keep in sync with offsetof(task_t, trap_ctx), asserted in proc.h. */
    .equ TASK_TRAP_CTX,         68

    .equ TF_SP,                 52
    .equ TF_LR,                 56
    .equ TF_PC,                 60
    .equ TF_CPSR,               64
    .equ TF_KERNEL_TP,          68
    .equ TF_KERNEL_SP,          72
    .equ TF_TTBR0,              76
    .equ TF_FAULT_ADDR,         80
    .equ TF_SIZE,               224
    .equ TF_VFP_D,              88
    .equ TF_FPSCR,              216

    /* Generic cause codes matching kernel/core/trap.h expectations. */
    .equ CAUSE_ECALL_U,          11
    .equ CAUSE_INSN_PAGE_FAULT,  12
    .equ CAUSE_LOAD_PAGE_FAULT,  13
    .equ CAUSE_STORE_PAGE_FAULT, 14
    .equ CAUSE_INTR_MASK,        (1 << 31)

    /* Trap metadata is per physical CPU; no global cause race on SMP. */
    .macro percpu_address dst, symbol
    push {r12}
    mrc p15,0,r12,c0,c0,5
    and r12,r12,#255
    ldr \dst,=\symbol
    add \dst,\dst,r12,lsl #2
    pop {r12}
    .endm
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
    percpu_address r1, arm32_trap_flags
    str     r0, [r1]
    ldmia   sp, {r0, r1}
    add     sp, sp, #8
    b       trap_common

pabort_vector:
    sub     sp, sp, #8
    stmia   sp, {r0, r1}
    ldr     r0, =CAUSE_INSN_PAGE_FAULT
    percpu_address r1, arm32_trap_flags
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
    percpu_address r2, arm32_trap_flags
    str     r0, [r2]
    ldmia   sp, {r0-r2}
    add     sp, sp, #12
    b       trap_common

irq_vector:
    /* r0 will be replaced below with CAUSE_INTR_MASK | irq. */
    sub     sp, sp, #8
    stmia   sp, {r0, r1}
    ldr     r0, =CAUSE_INTR_MASK
    percpu_address r1, arm32_trap_flags
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
    /* Both the trap frame and its VFP register block are 8-byte aligned. */
    sub     sp, sp, #224
    stmia   sp, {r0-r12}
    add     r1, sp, #TF_VFP_D
    vstmia  r1, {d0-d15}
    vmrs    r1, fpscr
    str     r1, [sp, #TF_FPSCR]
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
    add     r6, r6, #224
    str     r6, [r1]                   /* trap.sp */
    str     lr, [r1, #4]               /* trap.lr */

2:
    /* Compute trap.pc from the exception link register.
     *   IRQ:               LR = PC+4            -> LR-4
     *   SVC / prefetch:    LR = next instr + 4  -> LR-4
     *   Data abort:        LR = next instr + 8  -> LR-8
     */
    percpu_address r2, arm32_trap_flags
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

    /* Capture DFAR for data aborts and IFAR for prefetch aborts. */
    ldr     r2, =CAUSE_STORE_PAGE_FAULT
    percpu_address r3, arm32_trap_flags
    ldr     r3, [r3]
    cmp     r3, r2
    cmpne   r3, #CAUSE_LOAD_PAGE_FAULT
    beq     7f
    cmp     r3, #CAUSE_INSN_PAGE_FAULT
    bne     3f
    mrc     p15, 0, r2, c6, c0, 2       /* IFAR */
    b       8f
7:
    mrc     p15, 0, r2, c6, c0, 0       /* DFAR */
8:
    percpu_address r3, arm32_fault_addr
    str     r2, [r3]
3:
    ldr     r2, [r1, #8]
    percpu_address r3, arm32_fault_pc
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

    add     r2, sp, #224
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
    percpu_address r1, arm32_trap_flags
    ldr     r1, [r1]
    ldr     r2, =CAUSE_INTR_MASK
    tst     r1, r2
    beq     5f
    push    {r0-r3, r12, lr}
    bl      arm32_gic_ack
    ldr     r2, =CAUSE_INTR_MASK
    orr     r0, r0, r2
    percpu_address r2, arm32_trap_flags
    str     r0, [r2]
    pop     {r0-r3, r12, lr}
5:

    /* Record the cause for arch_read_cause(). */
    percpu_address r2, arm32_trap_flags
    ldr     r2, [r2]

    /* Dispatch to the appropriate C handler. */
    mrs     r2, spsr
    and     r2, r2, #0x1f
    cmp     r2, #MODE_USR
    beq     5f
    /* IRQ/abort stacks are entry scratch space, never schedulable stacks.
     * Move a privileged exception onto the interrupted task's SVC stack
     * before C may preempt it; otherwise another task reuses the banked
     * per-CPU stack while this handler is suspended there. */
    mrs     r2, cpsr
    and     r2, r2, #0x1f
    cmp     r2, #MODE_SVC
    beq     .Lkernel_frame_ready
    mov     r0, sp
    add     sp, sp, #224
    cps     #MODE_SVC
    mov     r1, lr
    mov     r2, sp
    sub     sp, sp, #224
    mov     r3, sp
    mov     r12, #7
.Lcopy_kernel_frame:
    ldmia   r0!, {r4-r11}
    stmia   r3!, {r4-r11}
    subs    r12, r12, #1
    bne     .Lcopy_kernel_frame
    str     r1, [sp, #TF_LR]
    str     r2, [sp, #TF_SP]
.Lkernel_frame_ready:
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
    mov     r12, #7
.Lcopy_trap_frame:
    ldmia   r3!, {r4-r11}
    stmia   r0!, {r4-r11}
    subs    r12, r12, #1
    bne     .Lcopy_trap_frame
    sub     r0, r0, #TF_SIZE
    str     r2, [r0, #TF_KERNEL_SP]
6:
    /* User exceptions also run C on their task's SVC stack. The persistent
     * frame at r0 remains above the call stack across scheduling. */
    mrs     r1, cpsr
    and     r1, r1, #0x1f
    cmp     r1, #MODE_SVC
    addne   sp, sp, #224
    cps     #MODE_SVC
    mov     sp, r0
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
    ldr     r1, [sp, #TF_KERNEL_TP]
    mcr     p15, 0, r1, c13, c0, 4
#ifndef CONFIG_NOMMU
    ldr     r1, [sp, #TF_TTBR0]
    /* Returning to the same translation regime cannot have left anything
     * stale behind, so only an actual TTBR0 change earns the invalidation. */
    mrc     p15, 0, r2, c2, c0, 0
    cmp     r1, r2
    beq     .Lttbr0_unchanged_k32
    mcr     p15, 0, r1, c2, c0, 0
    mov     r1, #0
    mcr     p15, 0, r1, c8, c7, 0
    dsb     ish
    isb
.Lttbr0_unchanged_k32:
#endif
    add     r1, sp, #TF_VFP_D
    vldmia  r1, {d0-d15}
    ldr     r1, [sp, #TF_FPSCR]
    vmsr    fpscr, r1
    /* The last eight scratch bytes become an RFE pair after VFP restore.
     * RFE restores PC/CPSR together and writeback recovers the original SP;
     * LR is the interrupted SVC LR, not the exception return address. */
    ldr     r1, [sp, #TF_PC]
    ldr     r2, [sp, #TF_CPSR]
    str     r1, [sp, #216]
    str     r2, [sp, #220]
    ldr     lr, [sp, #TF_LR]
    ldmia   sp, {r0-r12}
    add     sp, sp, #216
    rfeia   sp!

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
    mrc     p15, 0, r3, c2, c0, 0
    cmp     r12, r3
    beq     .Lttbr0_unchanged_u32
    mcr     p15, 0, r12, c2, c0, 0
    mov     r12, #0
    mcr     p15, 0, r12, c8, c7, 0
    dsb     ish
    isb
.Lttbr0_unchanged_u32:
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
    /* SVC owns the task kernel stack. IRQ/abort modes keep their dedicated
     * per-CPU stacks even after a user return, so a later kernel IRQ cannot
     * overwrite a live SVC frame on that task's stack. */
    mrs     r3, cpsr
    and     r3, r3, #31
    cmp     r3, #0x13
    moveq   sp, r12
    beq     .Luser_exception_stack_ready
    mrc     p15, 0, r12, c0, c0, 5
    and     r12, r12, #255
    ldr     sp, =arm32_exception_stacks
    add     sp, sp, r12, lsl #14
    cmp     r3, #0x12
    addeq   sp, sp, #4096
    cmp     r3, #0x17
    addeq   sp, sp, #8192
    cmp     r3, #0x1b
    addeq   sp, sp, #12288
    cmp     r3, #0x11
    addeq   sp, sp, #16384
.Luser_exception_stack_ready:
    add     r1, r0, #TF_VFP_D
    vldmia  r1, {d0-d15}
    ldr     r1, [r0, #TF_FPSCR]
    vmsr    fpscr, r1
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

    .global arch_return_to_user
arch_return_to_user:
    mov     sp, r0
    b       user_trap_return

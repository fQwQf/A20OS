    .altmacro
    .section .text
    .align 16

    .macro isr_no_err vec
    .globl isr_stub_\vec
isr_stub_\vec:
    pushq $0
    pushq $\vec
    jmp   isr_common
    .endm

    .macro isr_err vec
    .globl isr_stub_\vec
isr_stub_\vec:
    pushq $\vec
    jmp   isr_common
    .endm

isr_common:
    /* Allocate trap_context_t (192 bytes) on stack */
    subq  $192, %rsp

    /* Save general-purpose registers into trap_context_t */
    movq  %rax, 0(%rsp)
    movq  %rbx, 8(%rsp)
    movq  %rcx, 16(%rsp)
    movq  %rdx, 24(%rsp)
    movq  %rsi, 32(%rsp)
    movq  %rdi, 40(%rsp)
    movq  %rbp, 48(%rsp)
    /* rsp is filled later from CPU-pushed frame */
    movq  %r8,  64(%rsp)
    movq  %r9,  72(%rsp)
    movq  %r10, 80(%rsp)
    movq  %r11, 88(%rsp)
    movq  %r12, 96(%rsp)
    movq  %r13, 104(%rsp)
    movq  %r14, 112(%rsp)
    movq  %r15, 120(%rsp)

    /*
     * With IST=1 for all IDT entries, the CPU always pushes:
     *   old SS, old RSP, RFLAGS, CS, RIP
     * Plus error code (for isr_err) or nothing (for isr_no_err).
     * The ISR stub pushes vec (and dummy 0 for isr_no_err).
     * Total CPU+ISR stub = 56 bytes for both cases.
     *
     * Layout above trap_context_t (relative to current RSP):
     *   [RSP+192] = vec
     *   [RSP+200] = error_code (or 0)
     *   [RSP+208] = RIP
     *   [RSP+216] = CS
     *   [RSP+224] = RFLAGS
     *   [RSP+232] = old RSP
     *   [RSP+240] = old SS
     */

    /* Copy CPU-pushed values into trap_context_t */
    movq  208(%rsp), %rax
    movq  %rax, 128(%rsp)       /* ctx->rip */

    movq  224(%rsp), %rax
    movq  %rax, 136(%rsp)       /* ctx->rflags */

    movq  216(%rsp), %rax
    movq  %rax, 144(%rsp)       /* ctx->cs */

    testq $3, %rax
    jz    1f
    swapgs
1:

    movq  240(%rsp), %rax
    movq  %rax, 152(%rsp)       /* ctx->ss */

    movq  232(%rsp), %rax
    movq  %rax, 56(%rsp)        /* ctx->rsp */

    /* Save kernel_tp (GS base) */
    movl  $0xC0000101, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, 160(%rsp)       /* ctx->kernel_tp */

    /* Save user FS base for x86_64 TLS. */
    movl  $0xC0000100, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, 184(%rsp)       /* ctx->padding[1] */

    /* Read CR2 for kscratch0 (tval) */
    movq  %cr2, %rax
    movq  %rax, 168(%rsp)       /* ctx->kscratch0 */

    /*
     * Save kernel stack top for future CPL3 -> CPL0 transitions.  When
     * this trap came from user mode, TSS.rsp0 is the top above the CPU
     * frame (ctx + 192 + 56).
     */
    movq  $0, 176(%rsp)
    movq  144(%rsp), %rax       /* ctx->cs */
    testq $3, %rax
    jz    1f
    leaq  248(%rsp), %rax
    movq  %rax, 176(%rsp)       /* ctx->padding[0] */
1:

    /* Save global trap variables */
    movq  192(%rsp), %rax       /* vec */
    cmpq  $14, %rax             /* Is it #PF? */
    jne   3f
    /* It's a #PF. Check error code at 200(%rsp) */
    movq  200(%rsp), %rcx       /* error_code */
    testq $16, %rcx             /* bit 4: I/D (instruction fetch) */
    jnz   4f                    /* if set, CAUSE_INSN_PAGE_FAULT (12) */
    testq $2, %rcx              /* bit 1: W/R (write) */
    jnz   5f                    /* if set, CAUSE_STORE_PAGE_FAULT (14) */
    movq  $13, %rax             /* else, CAUSE_LOAD_PAGE_FAULT (13) */
    jmp   2f
4:
    movq  $12, %rax
    jmp   2f
5:
    movq  $14, %rax
    jmp   2f
3:
    cmpq  $0x80, %rax
    je    2f                    /* int $0x80 syscall */
    cmpq  $32, %rax
    jb    2f
    btsq  $63, %rax             /* Set CAUSE_INTR_MASK for IRQs */
2:
    movabsq $__x86_64_trap_cause, %rcx
    movq  %rax, (%rcx)

    movq  128(%rsp), %rax       /* ctx->rip */
    movabsq $__x86_64_trap_epc, %rcx
    movq  %rax, (%rcx)

    movq  168(%rsp), %rax       /* ctx->kscratch0 (CR2) */
    movabsq $__x86_64_trap_tval, %rcx
    movq  %rax, (%rcx)

    /* Dispatch by the interrupted privilege level.  User-mode page faults
     * and other exceptions must go through trap_handler so demand paging,
     * COW, and signal delivery work. */
    movq  144(%rsp), %rax
    testq $3, %rax
    jz    kernel_trap

    /* User-mode interrupt */
    movq  %rsp, %rdi
    call  trap_handler
    movq  %rsp, %rdi
    jmp   __return_to_user

kernel_trap:
    movq  %rsp, %rdi
    call  kernel_trap_handler

    /* Restore CPU-pushed frame from trap_context_t (in case C modified it) */
    movq  128(%rsp), %rax
    movq  %rax, 208(%rsp)       /* RIP */

    movq  136(%rsp), %rax
    movq  %rax, 224(%rsp)       /* RFLAGS */

    movq  144(%rsp), %rax
    movq  %rax, 216(%rsp)       /* CS */

    movq  152(%rsp), %rax
    movq  %rax, 240(%rsp)       /* SS */

    movq  56(%rsp), %rax
    movq  %rax, 232(%rsp)       /* old RSP */

    /* Restore general-purpose registers */
    movq  0(%rsp), %rax
    movq  8(%rsp), %rbx
    movq  16(%rsp), %rcx
    movq  24(%rsp), %rdx
    movq  32(%rsp), %rsi
    movq  40(%rsp), %rdi
    movq  48(%rsp), %rbp
    movq  64(%rsp), %r8
    movq  72(%rsp), %r9
    movq  80(%rsp), %r10
    movq  88(%rsp), %r11
    movq  96(%rsp), %r12
    movq  104(%rsp), %r13
    movq  112(%rsp), %r14
    movq  120(%rsp), %r15

    /* Free trap_context_t */
    addq  $192, %rsp

    /* Skip vec and error_code (or 0 and vec) from ISR stub */
    addq  $16, %rsp

    iretq

    .globl __return_to_user
__return_to_user:
    /* Keep TSS.rsp0 pointed at this task's kernel stack top. */
    movq  176(%rsp), %rdi       /* ctx->padding[0] */
    testq %rdi, %rdi
    jz    1f
    call  x86_64_set_tss_rsp0
1:

    /*
     * Build a fresh iret frame below trap_context_t.  This works both for
     * real traps and for first entry into a newly-created user task, where
     * no CPU-pushed frame exists above the saved context.
     *
     * temp layout at ctx - 56:
     *   +0  saved rax
     *   +8  saved r11
     *   +16 RIP
     *   +24 CS
     *   +32 RFLAGS
     *   +40 user RSP
     *   +48 SS
     */
    leaq  -56(%rsp), %r11
    movq  0(%rsp), %rax
    movq  %rax, 0(%r11)
    movq  88(%rsp), %rax
    movq  %rax, 8(%r11)
    movq  128(%rsp), %rax
    movq  %rax, 16(%r11)
    movq  144(%rsp), %rax
    movq  %rax, 24(%r11)
    movq  136(%rsp), %rax
    movq  %rax, 32(%r11)
    movq  56(%rsp), %rax
    movq  %rax, 40(%r11)
    movq  152(%rsp), %rax
    movq  %rax, 48(%r11)

    /* 
     * Reset data segment selectors to user data selector (0x23).
     * This prevents #GP if they contained kernel selectors (DPL=0).
     * Loading into FS/GS clears MSR_FS_BASE and MSR_GS_BASE,
     * but we immediately restore them below.
     */
    movl  $0x23, %eax
    movw  %ax, %ds
    movw  %ax, %es

    /* 
     * Swap GS base to put tss in MSR_KERNEL_GS_BASE and user GS base in MSR_GS_BASE.
     */
    swapgs

    /* Restore user FS base for TLS before iretq. */
    movq  184(%rsp), %rax
    movl  %eax, %edi
    shrq  $32, %rax
    movl  %eax, %edx
    movl  %edi, %eax
    movl  $0xC0000100, %ecx
    wrmsr

    /* Restore general-purpose registers except rax/r11. */
    movq  8(%rsp), %rbx
    movq  16(%rsp), %rcx
    movq  24(%rsp), %rdx
    movq  32(%rsp), %rsi
    movq  40(%rsp), %rdi
    movq  48(%rsp), %rbp
    movq  64(%rsp), %r8
    movq  72(%rsp), %r9
    movq  80(%rsp), %r10
    movq  96(%rsp), %r12
    movq  104(%rsp), %r13
    movq  112(%rsp), %r14
    movq  120(%rsp), %r15

    leaq  16(%r11), %rsp
    movq  -16(%rsp), %rax
    movq  -8(%rsp), %r11
    iretq

    .globl user_trap_return
user_trap_return:
    jmp   __return_to_user

    .globl __trap_from_user
__trap_from_user:
    jmp   isr_common

    .globl __trap_from_kernel
__trap_from_kernel:
    jmp   isr_common

    .globl syscall_entry
    .align 16
syscall_entry:
    swapgs
    movq  %rsp, %gs:12
    movq  %gs:4, %rsp
    
    subq  $192, %rsp
    
    /* Save ctx->rip (syscall RIP in RCX) and ctx->rflags (syscall RFLAGS in R11) immediately */
    movq  %rcx, 128(%rsp)
    movq  %r11, 136(%rsp)
    
    /* Update global epc */
    movabsq $__x86_64_trap_epc, %r11
    movq  %rcx, (%r11)
    
    /* Now we can use r11 as a scratch register */
    movq  %gs:12, %r11
    
    /* Save ctx->rsp */
    movq  %r11, 56(%rsp)
    
    /* Save general-purpose registers */
    movq  %rax, 0(%rsp)
    movq  %rbx, 8(%rsp)
    movq  %rcx, 16(%rsp)
    movq  %rdx, 24(%rsp)
    movq  %rsi, 32(%rsp)
    movq  %rdi, 40(%rsp)
    movq  %rbp, 48(%rsp)
    
    movq  %r8,  64(%rsp)
    movq  %r9,  72(%rsp)
    movq  %r10, 80(%rsp)
    movq  %r11, 88(%rsp) /* r11 originally had RFLAGS, but user r11 is lost in syscall. */
    movq  %r12, 96(%rsp)
    movq  %r13, 104(%rsp)
    movq  %r14, 112(%rsp)
    movq  %r15, 120(%rsp)
    
    /* Save ctx->cs = 0x2B (User Code 64-bit) */
    movq  $0x2B, 144(%rsp)
    /* Save ctx->ss = 0x23 (User Data) */
    movq  $0x23, 152(%rsp)
    
    /* Save kernel_tp into ctx */
    movl  $0xC0000101, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, 160(%rsp)
    
    /* Save user FS base */
    movl  $0xC0000100, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, 184(%rsp)
    
    /* CR2 for kscratch0 */
    movq  %cr2, %rax
    movq  %rax, 168(%rsp)
    
    /* Set ctx->padding[0] (TSS.rsp0 pointer) */
    leaq  192(%rsp), %rax
    movq  %rax, 176(%rsp)

    /* Set global trap cause to 0x80 (CAUSE_ECALL_U) */
    movabsq $__x86_64_trap_cause, %rcx
    movq  $0x80, (%rcx)

    /* Call trap_handler */
    movq  %rsp, %rdi
    call  trap_handler
    
    movq  %rsp, %rdi
    jmp   __return_to_user

    .align 16
    isr_no_err 0
    isr_no_err 1
    isr_no_err 2
    isr_no_err 3
    isr_no_err 4
    isr_no_err 5
    isr_no_err 6
    isr_no_err 7
    isr_err 8
    isr_no_err 9
    isr_err 10
    isr_err 11
    isr_err 12
    isr_err 13
    isr_err 14
    isr_no_err 15
    isr_no_err 16
    isr_err 17
    isr_no_err 18
    isr_no_err 19
    isr_no_err 20
    isr_no_err 21
    isr_no_err 22
    isr_no_err 23
    isr_no_err 24
    isr_no_err 25
    isr_no_err 26
    isr_no_err 27
    isr_no_err 28
    isr_no_err 29
    isr_no_err 30
    isr_no_err 31

    .set i, 32
    .rept 224
        isr_no_err %i
        .set i, i+1
    .endr

    .section .data
    .align 8
    .globl isr_stub_table
isr_stub_table:
    .macro isr_table_entry vec
        .quad isr_stub_\vec
    .endm
    .set i, 0
    .rept 256
        isr_table_entry %i
        .set i, i+1
    .endr

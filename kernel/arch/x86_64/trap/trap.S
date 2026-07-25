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
    /* Allocate trap_context_t (208 bytes) on stack */
    subq  $208, %rsp

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
     * Normal IDT entries do not use IST.  Traps from user mode switch to
     * TSS.rsp0 and push SS/RSP/RFLAGS/CS/RIP; traps from kernel mode remain
     * on the current task's stack and push only RFLAGS/CS/RIP.  Error-code
     * vectors plus the ISR stubs leave vec/error/RIP/CS/RFLAGS at the same
     * offsets in both cases.
     */

    /* Copy CPU-pushed values into trap_context_t */
    movq  224(%rsp), %rax
    movq  %rax, 128(%rsp)       /* ctx->rip */

    movq  240(%rsp), %rax
    movq  %rax, 136(%rsp)       /* ctx->rflags */

    movq  232(%rsp), %rax
    movq  %rax, 144(%rsp)       /* ctx->cs */

    movq  232(%rsp), %rax       /* CPU-pushed CS */
    testq $3, %rax
    jz    1f

    movq  256(%rsp), %rax
    movq  %rax, 152(%rsp)       /* ctx->ss */
    movq  248(%rsp), %rax
    movq  %rax, 56(%rsp)        /* ctx->rsp */
    jmp   2f

1:
    movq  $0x10, 152(%rsp)      /* current kernel SS */
    leaq  248(%rsp), %rax       /* interrupted kernel RSP */
    movq  %rax, 56(%rsp)
2:

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

    /* Save the per-task kernel stack top for future CPL3 -> CPL0 traps. */
    movq  $0, 176(%rsp)
    movq  144(%rsp), %rax       /* ctx->cs */
    testq $3, %rax
    jz    3f
    leaq  264(%rsp), %rax
    movq  %rax, 176(%rsp)       /* ctx->padding[0] */
3:

    /* A kernel IRQ may interrupt the now-preemptible syscall handler.  Save
     * its outer syscall state in fields unused by kernel-mode trap frames. */
    movq  144(%rsp), %rax
    testq $3, %rax
    jnz   5f
    leaq  176(%rsp), %rdi
    leaq  192(%rsp), %rsi
    leaq  200(%rsp), %rdx
    call  x86_64_save_trap_state
5:

    /* Save CPU-local trap variables. */
    movq  208(%rsp), %rax       /* vec */
    cmpq  $0x80, %rax
    je    2f                    /* int $0x80 syscall */
    cmpq  $32, %rax
    jb    2f
    btsq  $63, %rax             /* Set CAUSE_INTR_MASK for IRQs */
2:
    movq  %rax, %rdi
    movq  128(%rsp), %rsi       /* ctx->rip */
    movq  168(%rsp), %rdx       /* ctx->kscratch0 (CR2) */
    call  x86_64_set_trap_state

    /* Dispatch by the interrupted privilege level.  User-mode page faults
     * and other exceptions must go through trap_handler so demand paging,
     * COW, and signal delivery work. */
    movq  144(%rsp), %rax
    testq $3, %rax
    jz    kernel_trap

    /* User-mode interrupt */
    movq  %rsp, %rdi
    call  trap_handler
    jmp   __return_to_user

kernel_trap:
    movq  %rsp, %rdi
    call  kernel_trap_handler

    /* Restore an interrupted syscall's cause/epc/tval before its C handler
     * resumes and reads the CPU-local trap state. */
    movq  176(%rsp), %rdi
    movq  192(%rsp), %rsi
    movq  200(%rsp), %rdx
    call  x86_64_restore_trap_state

    /* Restore the common CPU-pushed frame in case C modified it. */
    movq  128(%rsp), %rax
    movq  %rax, 224(%rsp)       /* RIP */

    movq  136(%rsp), %rax
    movq  %rax, 240(%rsp)       /* RFLAGS */

    movq  144(%rsp), %rax
    movq  %rax, 232(%rsp)       /* CS */

    /*
     * Only privilege-changing traps have CPU-pushed RSP/SS slots.  Writing
     * them for a kernel-mode trap would corrupt the interrupted kernel stack.
     */
    movq  144(%rsp), %rax
    testq $3, %rax
    jz    4f
    movq  152(%rsp), %rax
    movq  %rax, 256(%rsp)       /* SS */
    movq  56(%rsp), %rax
    movq  %rax, 248(%rsp)       /* old RSP */
4:

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
    addq  $208, %rsp

    /* Skip vec and error_code (or 0 and vec) from ISR stub */
    addq  $16, %rsp

    iretq

    /*
     * syscall/sysret fast system call entry.
     *
     * The user library (musl x86_64 Linux ABI) issues the "syscall"
     * instruction, but the kernel only had an IDT entry for "int $0x80".
     * MSR_STAR/LSTAR/SFMASK and EFER.SCE are enabled in trap_init.
     *
     * On entry:
     *   rax = syscall number
     *   rdi/rsi/rdx/r10/r8/r9 = arguments
     *   rcx = user RIP
     *   r11 = user RFLAGS
     *   rsp = user RSP
     *
     * We construct a trap_context_t in the pre-allocated slot at the top of
     * the kernel stack and call trap_handler, then return via
     * __return_to_user (which uses iretq) so we do not need sysret.
     */
    .globl __x86_64_syscall_scratch
    .align 16

    .macro syscall_cpu_entry cpu
    .globl syscall_entry_\cpu
syscall_entry_\cpu:
    cli
    movq  %rax,  __x86_64_syscall_scratch+(\cpu * 128)+0(%rip)
    movq  %rcx,  __x86_64_syscall_scratch+(\cpu * 128)+8(%rip)
    movq  %rdx,  __x86_64_syscall_scratch+(\cpu * 128)+16(%rip)
    movq  %r10,  __x86_64_syscall_scratch+(\cpu * 128)+24(%rip)
    movq  %r11,  __x86_64_syscall_scratch+(\cpu * 128)+32(%rip)
    movq  %rsp,  __x86_64_syscall_scratch+(\cpu * 128)+40(%rip)
    movq  %r9,   __x86_64_syscall_scratch+(\cpu * 128)+48(%rip)
    movq  %r8,   __x86_64_syscall_scratch+(\cpu * 128)+56(%rip)
    movq  %rdi,  __x86_64_syscall_scratch+(\cpu * 128)+64(%rip)
    movq  %rsi,  __x86_64_syscall_scratch+(\cpu * 128)+72(%rip)
    movq  %rbx,  __x86_64_syscall_scratch+(\cpu * 128)+80(%rip)
    movq  %rbp,  __x86_64_syscall_scratch+(\cpu * 128)+88(%rip)
    movq  %r12,  __x86_64_syscall_scratch+(\cpu * 128)+96(%rip)
    movq  %r13,  __x86_64_syscall_scratch+(\cpu * 128)+104(%rip)
    movq  %r14,  __x86_64_syscall_scratch+(\cpu * 128)+112(%rip)
    movq  %r15,  __x86_64_syscall_scratch+(\cpu * 128)+120(%rip)
    leaq  __x86_64_syscall_scratch+(\cpu * 128)(%rip), %r11
    jmp syscall_entry_saved
    .endm

    syscall_cpu_entry 0
#if CONFIG_NR_CPUS > 1
    syscall_cpu_entry 1
#endif
#if CONFIG_NR_CPUS > 2
    syscall_cpu_entry 2
#endif
#if CONFIG_NR_CPUS > 3
    syscall_cpu_entry 3
#endif
#if CONFIG_NR_CPUS > 4
    syscall_cpu_entry 4
#endif
#if CONFIG_NR_CPUS > 5
    syscall_cpu_entry 5
#endif
#if CONFIG_NR_CPUS > 6
    syscall_cpu_entry 6
#endif
#if CONFIG_NR_CPUS > 7
    syscall_cpu_entry 7
#endif

syscall_entry_saved:
    /*
     * SYSCALL preserves the GS base MSR (current task pointer) but may leave
     * the GS segment selector in a state where %gs: memory operands fault.
     * Avoid %gs: entirely: read the GS base MSR directly to obtain the task
     * pointer, then use normal kernel addresses.
     *
     * On entry:
     *   rax = syscall number
     *   rdi/rsi/rdx/r10/r8/r9 = arguments 0-5
     *   rcx = user RIP, r11 = user RFLAGS, rsp = user RSP
     *
     * We must not write below the user RSP because the user code owns that
     * memory (red zone / stack frames).  Instead, stash the register state in
     * a small kernel scratch array while we compute the kernel stack top and
     * switch stacks.
     */
    /* Read GS base (task pointer) into r9. */
    movl  $0xC0000101, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, %r9             /* r9 = task pointer */

    /* r14 = saved task_context_t (task->kstack). */
    movq  0(%r9), %r14

    /*
     * Compute the real kernel stack top from the task structure.  The saved
     * context's tp field (offset 8) is the task pointer; task->kstack_base
     * is at offset 8 in task_t.
     */
    movq  8(%r14), %rax         /* rax = task pointer */
    movq  8(%rax), %rax         /* rax = task->kstack_base */
    addq  $0x10000, %rax        /* rax = kernel stack top */

    /* Switch to the kernel stack and allocate the trap_context_t. */
    movq  %rax, %rsp
    subq  $208, %rsp
    movq  %rax, %r15            /* r15 = kernel stack top (for later fields) */

    /* Copy the saved registers from the scratch array into the trap_context_t. */
    movq  0(%r11), %rax         /* syscall number */
    movq  %rax, 0(%rsp)
    movq  80(%r11), %rax        /* rbx */
    movq  %rax, 8(%rsp)
    movq  8(%r11), %rax         /* user RIP */
    movq  %rax, 16(%rsp)
    movq  16(%r11), %rax        /* rdx / arg2 */
    movq  %rax, 24(%rsp)
    movq  72(%r11), %rax        /* rsi / arg1 */
    movq  %rax, 32(%rsp)
    movq  64(%r11), %rax        /* rdi / arg0 */
    movq  %rax, 40(%rsp)
    movq  88(%r11), %rax        /* rbp */
    movq  %rax, 48(%rsp)
    movq  40(%r11), %rax        /* user RSP */
    movq  %rax, 56(%rsp)
    movq  56(%r11), %rax        /* r8 / arg4 */
    movq  %rax, 64(%rsp)
    movq  48(%r11), %rax        /* r9 / arg6 */
    movq  %rax, 72(%rsp)
    movq  24(%r11), %rax        /* r10 / arg3 */
    movq  %rax, 80(%rsp)
    movq  32(%r11), %rax        /* r11 / RFLAGS */
    movq  %rax, 88(%rsp)
    movq  96(%r11), %rax        /* r12 */
    movq  %rax, 96(%rsp)
    movq  104(%r11), %rax       /* r13 */
    movq  %rax, 104(%rsp)
    movq  112(%r11), %rax       /* r14 */
    movq  %rax, 112(%rsp)
    movq  120(%r11), %rax       /* r15 */
    movq  %rax, 120(%rsp)

    /* Return address, flags, and segments. */
    movq  8(%r11), %rax         /* user RIP */
    movq  %rax, 128(%rsp)
    movq  32(%r11), %rax        /* user RFLAGS */
    movq  %rax, 136(%rsp)
    movq  $0x1B, 144(%rsp)      /* user CS */
    movq  $0x23, 152(%rsp)      /* user SS */

    /* kernel_tp = current task pointer */
    movq  %r9, 160(%rsp)

    movq  $0, 168(%rsp)         /* kscratch0 */

    /* Kernel stack top for future CPL3->CPL0 transitions. */
    movq  %r15, 176(%rsp)

    /* User FS base for TLS. */
    movl  $0xC0000100, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, 184(%rsp)

    /* Original task->kstack so __return_to_user / __switch can restore it. */
    movq  %r14, 192(%rsp)

    /* Publish the syscall cause in this CPU's trap state. */
    movq  $0x80, %rdi
    movq  128(%rsp), %rsi
    xorl  %edx, %edx
    call  x86_64_set_trap_state

    /* The scratch slot is no longer live: the complete user state now resides
     * on the task's kernel stack.  Permit timer and reschedule IRQs in C. */
    sti
    movq  %rsp, %rdi
    call  trap_handler
    jmp   __return_to_user

    .globl __return_to_user
__return_to_user:
    /*
     * task->kstack is maintained by __switch and syscall_entry, so we do not
     * need to read or write %gs:0 here.  Avoiding %gs: prevents faults when
     * the GS segment selector is not usable.
     */

    /* Keep TSS.rsp0 pointed at this task's kernel stack top. */
    movq  176(%rsp), %rdi       /* ctx->padding[0] */
    testq %rdi, %rdi
    jz    1f
    call  x86_64_set_tss_rsp0
1:

    /* Restore kernel_tp (GS base) */
    movq  160(%rsp), %rax
    movl  %eax, %edi
    shrq  $32, %rax
    movl  %eax, %edx
    movl  %edi, %eax
    movl  $0xC0000101, %ecx
    wrmsr

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

    /*
     * Build the iret frame in-place at the bottom of trap_context_t.
     * This avoids writing below the trap frame and clobbering the
     * saved task_context_t that lives just underneath it.
     */
    movq  0(%rsp), %rax          /* original rax */
    movq  %rax, 0(%rsp)
    movq  88(%rsp), %rax         /* original r11 */
    movq  %rax, 8(%rsp)
    movq  128(%rsp), %rax
    movq  %rax, 16(%rsp)         /* RIP */
    movq  144(%rsp), %rax
    movq  %rax, 24(%rsp)         /* CS */
    movq  136(%rsp), %rax
    movq  %rax, 32(%rsp)         /* RFLAGS */
    movq  56(%rsp), %rax
    movq  %rax, 40(%rsp)         /* user RSP */
    movq  152(%rsp), %rax
    movq  %rax, 48(%rsp)         /* SS */

    /* Restore rax/r11, point the stack at the iret frame, and return. */
    movq  0(%rsp), %rax
    movq  8(%rsp), %r11
    leaq  16(%rsp), %rsp
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

    .globl syscall_entry_table
syscall_entry_table:
    .quad syscall_entry_0
#if CONFIG_NR_CPUS > 1
    .quad syscall_entry_1
#endif
#if CONFIG_NR_CPUS > 2
    .quad syscall_entry_2
#endif
#if CONFIG_NR_CPUS > 3
    .quad syscall_entry_3
#endif
#if CONFIG_NR_CPUS > 4
    .quad syscall_entry_4
#endif
#if CONFIG_NR_CPUS > 5
    .quad syscall_entry_5
#endif
#if CONFIG_NR_CPUS > 6
    .quad syscall_entry_6
#endif
#if CONFIG_NR_CPUS > 7
    .quad syscall_entry_7
#endif

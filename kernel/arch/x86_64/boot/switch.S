    .section .text
    .globl __switch
    .align 16

__switch:
    /*
     * %rdi = pointer to next task's saved task_context_t.
     *
     * On x86_64 the task_context_t lives at the bottom of the kernel stack.
     * Save the current task's context there instead of using the current RSP,
     * because the area just below the current RSP may be too close to (or
     * overlap with) the trap_context_t at the top of the kernel stack.
     */

    /* Read current task pointer from GS base MSR. */
    movl  $0xC0000101, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, %r9             /* r9 = current task pointer */

    /* r10 = base of current task's kernel stack (= task_context_t location). */
    movq  8(%r9), %r10          /* task->kstack_base */

    /*
     * Save the return address that was pushed by the call to __switch.
     * We will jump to it directly, so the saved RSP must be the value after
     * a normal ret (i.e. with the return address popped).
     */
    movq  0(%rsp), %rax
    movq  %rax, 0(%r10)         /* ra */

    leaq  8(%rsp), %rax
    movq  %rax, 64(%r10)        /* rsp after returning from __switch */

    /* Save callee-saved registers. */
    movq  %r9, 8(%r10)          /* tp */
    movq  %rbx, 16(%r10)
    movq  %rbp, 24(%r10)
    movq  %r12, 32(%r10)
    movq  %r13, 40(%r10)
    movq  %r14, 48(%r10)
    movq  %r15, 56(%r10)

    /* Save rflags */
    pushfq
    popq  %rax
    movq  %rax, 72(%r10)

    /* Save cr3 */
    movq  %cr3, %rax
    movq  %rax, 80(%r10)

    /* Save kernel stack top for syscall_entry. */
    movq  %r10, %rax
    addq  $0x10000, %rax
    movq  %rax, 88(%r10)

    /* Update current task->kstack to point to the saved context at the bottom. */
    movq  %r10, 0(%r9)

    /* Restore next task's context. */
    movq  0(%rdi), %r11         /* ra */
    movq  8(%rdi), %rcx         /* tp */
    movq  16(%rdi), %rbx
    movq  24(%rdi), %rbp
    movq  32(%rdi), %r12
    movq  40(%rdi), %r13
    movq  48(%rdi), %r14
    movq  56(%rdi), %r15
    movq  64(%rdi), %r8         /* rsp */
    movq  72(%rdi), %rdx        /* rflags */
    movq  80(%rdi), %rsi        /* cr3 */

    /* Set tp via gsbase */
    movq  %rcx, %rax
    movl  %eax, %edi
    shrq  $32, %rax
    movl  %eax, %edx
    movl  %edi, %eax
    movl  $0xC0000101, %ecx
    wrmsr

    /* Check if cr3 changed */
    movq  %cr3, %r9
    cmpq  %r9, %rsi
    je    1f
    movq  %rsi, %cr3
1:

    /* Restore rflags */
    pushq %rdx
    popfq

    /* Switch to next task's stack */
    movq  %r8, %rsp

    /* Jump to next task's return address */
    jmp   *%r11

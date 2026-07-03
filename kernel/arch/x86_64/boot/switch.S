    .section .text
    .globl __switch
    .align 16

__switch:
    /* %rdi = pointer to next task's saved task_context_t */

    subq  $128, %rsp

    /* Save callee-saved registers */
    movq  %rbx, 16(%rsp)
    movq  %rbp, 24(%rsp)
    movq  %r12, 32(%rsp)
    movq  %r13, 40(%rsp)
    movq  %r14, 48(%rsp)
    movq  %r15, 56(%rsp)

    /* Save return address (rip) from stack */
    movq  128(%rsp), %rax
    movq  %rax, 0(%rsp)

    /* Save task pointer (tp) via gsbase */
    movl  $0xC0000101, %ecx
    rdmsr
    shlq  $32, %rdx
    orq   %rdx, %rax
    movq  %rax, 8(%rsp)

    /* Save caller rsp as it would be after a normal ret. */
    movq  %rsp, %rax
    addq  $136, %rax
    movq  %rax, 64(%rsp)

    /* Save rflags */
    pushfq
    popq  %rax
    movq  %rax, 72(%rsp)

    /* Save cr3 */
    movq  %cr3, %rax
    movq  %rax, 80(%rsp)

    /* Save current context pointer into current task's kstack field */
    movq  8(%rsp), %rax       /* tp */
    movq  %rsp, (%rax)

    /* Restore next task's context */
    movq  0(%rdi), %r11       /* ra  -> preserve in callee-saved r11 */
    movq  8(%rdi), %rcx       /* tp */
    movq  16(%rdi), %rbx
    movq  24(%rdi), %rbp
    movq  32(%rdi), %r12
    movq  40(%rdi), %r13
    movq  48(%rdi), %r14
    movq  56(%rdi), %r15
    movq  64(%rdi), %r8       /* rsp */
    movq  72(%rdi), %rdx      /* rflags */
    movq  80(%rdi), %rsi      /* cr3 */

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

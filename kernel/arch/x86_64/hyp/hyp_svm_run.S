/* VMRUN saves RAX/RSP/RIP in the VMCB; all other GPRs are software state.
 * regs[] uses x86 instruction-register numbering: RAX RCX RDX RBX RSP RBP
 * RSI RDI R8..R15. No offset into hyp_vcpu_t is embedded here. */
.text
.global hyp_svm_run
.type hyp_svm_run,@function
hyp_svm_run:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rdi                    /* 48: guest VMCB PA */
    pushq %rsi                    /* 40: host VMCB PA */
    pushq %rdx                    /* 32: guest registers */
    pushq %rcx                    /* 24: host FXSAVE image */
    pushq %r8                     /* 16: guest FXSAVE image */
    subq $16, %rsp
    movq %cr2, %rax
    movq %rax, 0(%rsp)
    movq %cr0, %rax
    movq %rax, 8(%rsp)
    clgi
    andq $-13, %rax             /* clear host CR0.EM/TS during FX save */
    movq %rax, %cr0
    fxsave64 (%rcx)
    fxrstor64 (%r8)
    movq 40(%rsp), %rax
    vmsave %rax
    movq 48(%rsp), %rax
    vmload %rax
    movq 32(%rsp), %rbp
    movq 8(%rbp), %rcx
    movq 16(%rbp), %rdx
    movq 24(%rbp), %rbx
    movq 48(%rbp), %rsi
    movq 56(%rbp), %rdi
    movq 64(%rbp), %r8
    movq 72(%rbp), %r9
    movq 80(%rbp), %r10
    movq 88(%rbp), %r11
    movq 96(%rbp), %r12
    movq 104(%rbp), %r13
    movq 112(%rbp), %r14
    movq 120(%rbp), %r15
    movq 40(%rbp), %rbp
    movq 48(%rsp), %rax
    /* GIF remains zero until VMRUN. Host IF=1 lets physical IRQs exit even
     * when guest IF=0, with V_INTR_MASKING set. */
    sti
    vmrun %rax
    cli
    movq 32(%rsp), %rax
    movq %rcx, 8(%rax)
    movq %rdx, 16(%rax)
    movq %rbx, 24(%rax)
    movq %rbp, 40(%rax)
    movq %rsi, 48(%rax)
    movq %rdi, 56(%rax)
    movq %r8, 64(%rax)
    movq %r9, 72(%rax)
    movq %r10, 80(%rax)
    movq %r11, 88(%rax)
    movq %r12, 96(%rax)
    movq %r13, 104(%rax)
    movq %r14, 112(%rax)
    movq %r15, 120(%rax)
    movq 48(%rsp), %rax
    vmsave %rax
    movq 40(%rsp), %rax
    vmload %rax                  /* restore host GS before C/per-CPU access */
    movq 16(%rsp), %rax
    fxsave64 (%rax)
    movq 24(%rsp), %rax
    fxrstor64 (%rax)
    movq 0(%rsp), %rax
    movq %rax, %cr2
    movq 8(%rsp), %rax
    movq %rax, %cr0
    stgi
    addq $56, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size hyp_svm_run, .-hyp_svm_run
.section .note.GNU-stack,"",@progbits

/* SysV: regs, host_fx, guest_fx, resume. VM exits restore this stack/RIP.
 * Guest GPRs, CR2 and legacy FP/SSE are banked before returning to C. */
.text
.global hyp_vmx_run
.type hyp_vmx_run,@function
hyp_vmx_run:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    movq %cr2,%rax
    pushq %rax
    movq %cr0,%rax
    pushq %rax
    andq $-13,%rax
    movq %rax,%cr0
    fxsave64 (%rsi)
    fxrstor64 (%rdx)
    movq 512(%rdx),%rax
    movq %rax,%cr2
    movq 520(%rdx),%rax
    movq %rax,%dr6
    movq $0x6c14,%rax
    vmwrite %rsp,%rax
    leaq .Lvmx_exit(%rip),%rdx
    movq $0x6c16,%rax
    vmwrite %rdx,%rax
    movq 40(%rsp),%rbp
    movq 8(%rbp),%rcx
    movq 16(%rbp),%rdx
    movq 24(%rbp),%rbx
    movq 48(%rbp),%rsi
    movq 56(%rbp),%rdi
    movq 64(%rbp),%r8
    movq 72(%rbp),%r9
    movq 80(%rbp),%r10
    movq 88(%rbp),%r11
    movq 96(%rbp),%r12
    movq 104(%rbp),%r13
    movq 112(%rbp),%r14
    movq 120(%rbp),%r15
    movq 0(%rbp),%rax
    movq 40(%rbp),%rbp
    cmpq $0,16(%rsp)
    jne .Lresume
    vmlaunch
    jmp .Lfailed
.Lresume:
    vmresume
.Lfailed:
    movl $1,%eax
    jmp .Lrestore
.Lvmx_exit:
    pushq %rax
    movq 48(%rsp),%rax
    movq %rcx,8(%rax)
    movq %rdx,16(%rax)
    movq %rbx,24(%rax)
    movq %rbp,40(%rax)
    movq %rsi,48(%rax)
    movq %rdi,56(%rax)
    movq %r8,64(%rax)
    movq %r9,72(%rax)
    movq %r10,80(%rax)
    movq %r11,88(%rax)
    movq %r12,96(%rax)
    movq %r13,104(%rax)
    movq %r14,112(%rax)
    movq %r15,120(%rax)
    popq %rdx
    movq %rdx,0(%rax)
    xorl %eax,%eax
.Lrestore:
    movl %eax,%ebx
    movq 24(%rsp),%rax
    fxsave64 (%rax)
    movq %cr2,%rdx
    movq %rdx,512(%rax)
    movq %dr6,%rdx
    movq %rdx,520(%rax)
    movq 32(%rsp),%rax
    fxrstor64 (%rax)
    movq 8(%rsp),%rax
    movq %rax,%cr2
    movq 0(%rsp),%rax
    movq %rax,%cr0
    movl %ebx,%eax
    addq $48,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size hyp_vmx_run,.-hyp_vmx_run
.section .note.GNU-stack,"",@progbits

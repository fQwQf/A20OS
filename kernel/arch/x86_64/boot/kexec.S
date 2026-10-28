    .section .text
    .globl x86_64_kexec_enter
    .globl x86_64_kexec_trampoline
    .globl x86_64_kexec_trampoline_end

    .code64
x86_64_kexec_enter:
    movq %rdi, %r8
    movq %rsi, %rdi
    movq %rdx, %rax
    movq %rax, %cr3
    jmp *%r8

x86_64_kexec_trampoline:
    cli
    movq %rdi, %r15
    leaq 40(%r15), %rsi
    movl 24(%r15), %ecx
.Lsegment:
    testl %ecx, %ecx
    jz .Ljump
    movq 0(%rsi), %r8
    movq 8(%rsi), %r9
    movq 16(%rsi), %r10
    movq 24(%rsi), %r11
    xorq %r12, %r12
    xorq %r13, %r13
.Lbyte:
    cmpq %r11, %r12
    jae .Lnext
    cmpq $4088, %r13
    jb .Lsource
    testq %r8, %r8
    jz .Lfail
    movq 0(%r8), %r8
    movabsq $0xffff800000000000, %rax
    subq %rax, %r8
    xorq %r13, %r13
.Lsource:
    testq %r8, %r8
    jz .Lfail
    leaq 8(%r8,%r13), %r14
    cmpq %r10, %r12
    jae .Lzero
    movb (%r14), %al
    jmp .Lstore
.Lzero:
    xorb %al, %al
.Lstore:
    movb %al, (%r9)
    incq %r9
    incq %r12
    incq %r13
    jmp .Lbyte
.Lnext:
    addq $32, %rsi
    decl %ecx
    jmp .Lsegment
.Ljump:
    movq 8(%r15), %rbx
    movq 16(%r15), %rcx
    movq 32(%r15), %rdx
    movq 0(%r15), %r14
    movl $0xA20C0DE, %eax
    jmp *%r14
.Lfail:
    hlt
    jmp .Lfail
x86_64_kexec_trampoline_end:

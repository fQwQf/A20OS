    .section .rodata.ap_trampoline, "a"
    .code16
    .globl x86_64_ap_trampoline_start
    .globl x86_64_ap_trampoline_end
    .globl x86_64_ap_trampoline_gdt_base
    .globl x86_64_ap_trampoline_gdt
    .globl x86_64_ap_trampoline_cr3
    .globl x86_64_ap_trampoline_stack
    .globl x86_64_ap_trampoline_cpu
    .globl x86_64_ap_trampoline_entry

    .set AP_TRAMPOLINE_PA, 0x7000

x86_64_ap_trampoline_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x6ff0, %esp

    lgdtl AP_TRAMPOLINE_PA + (ap_gdt_desc - x86_64_ap_trampoline_start)

    movl %cr4, %eax
    orl  $0x30, %eax
    movl %eax, %cr4
    movl AP_TRAMPOLINE_PA + (x86_64_ap_trampoline_cr3 - x86_64_ap_trampoline_start), %eax
    movl %eax, %cr3

    movl $0xc0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr

    movl %cr0, %eax
    orl  $0x80000001, %eax
    movl %eax, %cr0
    ljmpl $0x08, $(AP_TRAMPOLINE_PA + (ap_long_mode - x86_64_ap_trampoline_start))

    .align 8
x86_64_ap_trampoline_gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
ap_gdt_desc:
    .word 23
x86_64_ap_trampoline_gdt_base:
    .long 0

    .align 8
x86_64_ap_trampoline_cr3:
    .long 0
    .long 0
x86_64_ap_trampoline_stack:
    .quad 0
x86_64_ap_trampoline_cpu:
    .long 0
x86_64_ap_trampoline_entry:
    .quad 0

    .code64
ap_long_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq AP_TRAMPOLINE_PA + (x86_64_ap_trampoline_stack - x86_64_ap_trampoline_start), %rsp
    movl AP_TRAMPOLINE_PA + (x86_64_ap_trampoline_cpu - x86_64_ap_trampoline_start), %edi
    movq AP_TRAMPOLINE_PA + (x86_64_ap_trampoline_entry - x86_64_ap_trampoline_start), %rax
    call *%rax
1:
    cli
    hlt
    jmp 1b

x86_64_ap_trampoline_end:

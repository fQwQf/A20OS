#if defined(__riscv)
    .section .text.start,"ax",@progbits
    .globl _start
    .type _start,@function
_start:
    call probe_main
    li a7, 93
    ecall
1:
    j 1b
    .size _start, .-_start
#elif defined(__loongarch__)
    .section .text.start,"ax",@progbits
    .globl _start
    .type _start,@function
_start:
    bl probe_main
    ori $a7, $zero, 93
    syscall 0
1:
    b 1b
    .size _start, .-_start
#else
#error unsupported BuildStorm probe architecture
#endif

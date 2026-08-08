#if defined(__riscv)
    .section .text
    .globl _start
_start:
    li a7, 81
    ecall
    li a0, 0xfee1dead
    li a1, 672274793
    li a2, 0x4321fedc
    li a3, 0
    li a7, 142
    ecall
    li a0, 1
    li a7, 93
    ecall
#elif defined(__loongarch__)
    .section .text
    .globl _start
_start:
    li.w $a7, 81
    syscall 0
    li.w $a0, 0xfee1dead
    li.w $a1, 672274793
    li.w $a2, 0x4321fedc
    move $a3, $zero
    li.w $a7, 142
    syscall 0
    li.w $a0, 1
    li.w $a7, 93
    syscall 0
#else
#error unsupported architecture
#endif

    .text
    .global _start
    .type _start, @function
_start:
    call _start_c
    mov $0x0200, %rax
    xor %rdi, %rdi
    int $0x80
1:  jmp 1b
    .size _start, . - _start

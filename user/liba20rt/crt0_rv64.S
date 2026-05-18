    .text
    .global _start
    .type _start, @function
_start:
    call _start_c
    li a7, 0x0200
    li a0, 0
    ecall
1:  j 1b
    .size _start, . - _start

    .text
    .global _start
    .type _start, @function
_start:
    bl _start_c
    li.w $a7, 0x0200
    li.w $a0, 0
    syscall 0
1:  b 1b
    .size _start, . - _start

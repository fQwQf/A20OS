    .text
    .global _start
    .type _start, @function
_start:
    bl _start_c
    li 0, 0x0200
    li 3, 0
    sc
1:  b 1b
    .size _start, . - _start

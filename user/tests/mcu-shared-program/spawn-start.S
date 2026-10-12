    .syntax unified
    .thumb
    .text
    .global _start
    .type _start, %function
    .thumb_func
_start:
    bl _start_c
1:  b 1b
    .size _start, . - _start

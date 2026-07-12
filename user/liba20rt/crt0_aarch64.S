    .text
    .global _start
    .type _start, %function
_start:
    bl _start_c
    mov x8, #0x0200
    mov x0, #0
    svc #0
1:  b 1b
    .size _start, . - _start

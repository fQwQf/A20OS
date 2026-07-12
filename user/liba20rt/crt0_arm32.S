    .text
    .global _start
    .type _start, %function
_start:
    bl _start_c
    mov r7, #0x0200
    mov r0, #0
    svc #0
1:  b 1b
    .size _start, . - _start

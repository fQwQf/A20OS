/* Test-only Linux syscall shim for qemu-riscv32. No libc or kernel substitution.
 * The production driver/atomic runtime and the common C fixture are unchanged. */
    .section .text
    .globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    call main
    j exit

    .globl exit
exit:
    li a7, 93
    ecall
1:  j 1b

    .globl puts
puts:
    mv a1, a0
    mv t0, a0
    li a2, 0
1:  lbu t1, 0(t0)
    beqz t1, 2f
    addi t0, t0, 1
    addi a2, a2, 1
    j 1b
2:  li a0, 1
    li a7, 64
    ecall
    la a1, newline
    li a0, 1
    li a2, 1
    li a7, 64
    ecall
    li a0, 0
    ret
    .section .rodata
newline:
    .byte 10

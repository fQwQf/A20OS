/* Relocatable, stackless VS-mode SBI contract payload, copied into guest RAM.
 * a1 supplies a guest report/data page. No kernel address enters the guest. */
    .section .rodata.hyp_sbi_test,"a",@progbits
    .balign 4
    .option push
    .option norvc
    .option norelax
    .globl hyp_sbi_test_start, hyp_sbi_test_end
hyp_sbi_test_start:
    mv s0, a1
    li s1, 0
    li t3, 0x12345678
    li t4, 0x76543210
    .macro call eid, fid
    addi s1, s1, 1
    sw s1, 0(s0)
    li a7, \eid
    li a6, \fid
    ecall
    li t0, 0x12345678
    bne t3, t0, .Lfail
    li t0, 0x76543210
    bne t4, t0, .Lfail
    .endm
    .macro error value
    li t0, \value
    bne a0, t0, .Lfail
    .endm
    .macro value expected
    li t0, \expected
    bne a1, t0, .Lfail
    .endm
    call 0x10, 0
    error 0
    value 2
    call 0x10, 1
    error 0
    value 0xa20
    call 0x10, 2
    error 0
    value 2
    li a0, 0x4442434e
    call 0x10, 3
    error 0
    value 1
    li a0, 2
    call 0x10, 3
    error 0
    value 1
    li a0, 0x504d55
    call 0x10, 3
    error 0
    value 0
    call 0x10, 4
    error 0
    value 0
    call 0x10, 5
    error 0
    value 0
    call 0x10, 6
    error 0
    value 0
    call 0x10, 7
    error -2
    call 0x12345678, 0
    error -2
    call 0x54494d45, 99
    error -2
    li a1, 0x1234
    call 3, 99
    error -2
    value 0x1234

    li a0, -1
    li a1, -1
    call 0x54494d45, 0
    error 0
    csrr t0, sip
    andi t0, t0, 32
    bnez t0, .Lfail
    li a0, 0x89abcdef
    li a1, 0x12345678
    call 0, 99
    error 0
    value 0x12345678
    csrr t0, 0x14d
    li t1, 0x89abcdef
    bne t0, t1, .Lfail
#if __riscv_xlen == 32
    csrr t0, 0x15d
    li t1, 0x12345678
    bne t0, t1, .Lfail
#endif
    li a0, -1
    li a1, -1
    call 0x54494d45, 0
    error 0
    li a0, 0
    call 0x48534d, 2
    error 0
    value 0
    li a0, 99
    call 0x48534d, 2
    error -3
    li a0, 99
    mv a1, s0
    call 0x48534d, 0
    error -3
    li a0, 0
    addi a1, s0, 1
    call 0x48534d, 0
    error -5
    li a0, 0
    mv a1, s0 /* NX report page must not be an executable hart entry. */
    call 0x48534d, 0
    error -5
    li a0, 0
    li t0, 4096
    sub a1, s0, t0
    call 0x48534d, 0
    error -6
    call 0x48534d, 99
    error -2

    li a0, 1
    li a1, 0
    call 0x735049, 0
    error 0
    csrr t0, sip
    andi t0, t0, 2
    beqz t0, .Lfail
    li t0, 2
    csrc sip, t0
    li a0, 1
    li a1, 0
    call 0x52464e43, 0
    error 0
    li a0, 1
    li a1, 0
    li a2, 0
    li a3, -1
    call 0x52464e43, 1
    error 0
    li a0, 1
    li a1, 0
    li a2, 0
    li a3, -1
    li a4, 7
    call 0x52464e43, 2
    error 0
    li a0, 1
    li a1, 99
    call 0x52464e43, 0
    error -3
    li a0, 0
    li a1, -1
    call 0x52464e43, 0
    error 0
    li a0, 0
    li a1, 2
    call 0x53525354, 0
    error -3
    li a0, 3
    li a1, 0
    call 0x53525354, 0
    error -3
    li a0, 1
    li a1, 0
    call 0x53525354, 0
    error -2
    li a0, 2
    li a1, 0
    call 0x53525354, 0
    error -2

    li a0, 2
    li t0, 4096
    sub a1, s0, t0 /* read into RX code must be denied before consuming input */
    li a2, 0
    call 0x4442434e, 1
    error -3
    li a0, 2
    li t0, 4095
    add a1, s0, t0 /* cross two writable guest pages */
    li a2, 0
    call 0x4442434e, 1
    error 0
    value 2
    li t0, 4095
    add t0, s0, t0
    lbu t1, 0(t0)
    li t2, 114 /* r */
    bne t1, t2, .Lfail
    lbu t1, 1(t0)
    li t2, 118 /* v */
    bne t1, t2, .Lfail
    li a0, 0
    call 2, 99
    error -1
    li a0, 3
    addi a1, s0, 32
    li a2, 0
    call 0x4442434e, 0
    error 0
    value 3
    li a0, 33 /* ! */
    call 0x4442434e, 2
    error 0
    value 0
    li a0, 1
    li a1, -1
    li a2, -1
    call 0x4442434e, 0
    error -3
    /* A VU ecall using the shutdown number is a guest syscall, not SBI. */
    addi s1, s1, 1
    sw s1, 0(s0)
    lla t0, .Luser_trap
    csrw stvec, t0
    lla t0, .Luser
    csrw sepc, t0
    csrw sstatus, zero
    sret
.Luser:
    li a7, 8
    ecall
    j .Lfail
.Luser_trap:
    csrr t0, scause
    li t1, 8
    bne t0, t1, .Lfail
    sw s1, 4(s0)
    li t0, 0x51b1
    sw t0, 0(s0)
    li a0, 0
    li a1, 0
    li a7, 0x53525354
    li a6, 0
    ecall
.Lfail:
    /* Leave the failing call ordinal in report[0]. */
    li a7, 8
    ecall
    j .Lfail
hyp_sbi_test_end:
    .option pop

    .balign 4
    .option push
    .option norvc
    .option norelax
    .globl hyp_sbi_smp_start, hyp_sbi_smp_end
hyp_sbi_smp_start:
    li t0, 1
    sw t0, 0(a1) /* preserve the common smoke's ready[hart] protocol */
    andi s0, a1, -8
    bnez a0, .Lsbi_secondary
.Lsbi_wait_secondary:
    lw t0, 4(s0)
    beqz t0, .Lsbi_wait_secondary
    fence rw, rw
    li a0, 1
    li a7, 0x48534d
    li a6, 2
    ecall
    bnez a0, .Lsbi_fail
    bnez a1, .Lsbi_fail /* secondary must still be STARTED */
    li a0, 1
    li t0, 4096
    sub a1, s0, t0
    addi a2, s0, 4
    li a7, 0x48534d
    li a6, 0
    ecall
    li t0, -6
    bne a0, t0, .Lsbi_fail
    /* Remote IPI; the secondary proves that VSSIP reaches its VS sip. */
    li a0, 2
    li a1, 0
    li a7, 0x735049
    li a6, 0
    ecall
    bnez a0, .Lsbi_fail
.Lsbi_wait_ipi:
    lw t0, 16(s0)
    beqz t0, .Lsbi_wait_ipi
    fence rw, rw
    /* Real remote instruction and VS-translation fences while both run. */
    li a0, 2
    li a1, 0
    li a7, 0x52464e43
    li a6, 0
    ecall
    bnez a0, .Lsbi_fail
    li a0, 3
    li a1, 0
    li a2, 0
    li a3, -1
    li a7, 0x52464e43
    li a6, 1
    ecall
    bnez a0, .Lsbi_fail
    li a0, 3
    li a1, 0
    li a2, 0
    li a3, -1
    li a4, 7
    li a7, 0x52464e43
    li a6, 2
    ecall
    bnez a0, .Lsbi_fail
    li t0, 0x51b1
    fence rw, rw
    sw t0, 12(s0)
    j .Lsbi_release
.Lsbi_secondary:
    csrr t0, sip
    andi t0, t0, 2
    beqz t0, .Lsbi_secondary
    li t0, 2
    csrc sip, t0
    li t0, 1
    fence rw, rw
    sw t0, 16(s0)
.Lsbi_wait_master:
    lw t0, 12(s0)
    li t1, 0x51b1
    bne t0, t1, .Lsbi_wait_master
.Lsbi_release:
    lw t0, 8(s0)
    beqz t0, .Lsbi_release
    fence rw, rw
    li a7, 0x48534d
    li a6, 1
    ecall
.Lsbi_fail:
    li t0, 0xdead
    sw t0, 12(s0)
    li a7, 8
    ecall
    j .Lsbi_fail
hyp_sbi_smp_end:
    .option pop

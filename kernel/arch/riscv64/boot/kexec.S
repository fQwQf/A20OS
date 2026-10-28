    .section .text
    .globl a20_rv64_kexec_trampoline
    .globl a20_rv64_kexec_trampoline_end
    .globl a20_rv64_kexec_enter

a20_rv64_kexec_enter:
    mv      t0, a0
    mv      t1, a1
    csrw    satp, a2
    sfence.vma
    fence.i
    mv      a0, t1
    jr      t0

a20_rv64_kexec_trampoline:
    csrw    satp, zero
    sfence.vma
    mv      s0, a0
    addi    s1, s0, 40
    lw      s2, 24(s0)
.Lsegment:
    beqz    s2, .Ljump
    ld      t0, 0(s1)
    ld      s3, 8(s1)
    ld      s4, 16(s1)
    ld      s5, 24(s1)
    li      s6, 0
    li      t3, 0
.Lbyte:
    bgeu    s6, s5, .Lnext_segment
    li      t4, 4088
    bltu    t3, t4, .Lsource
    beqz    t0, .Lfail
    ld      t0, 0(t0)
    li      t4, 0xFFFFFFC000000000
    sub     t0, t0, t4
    li      t3, 0
.Lsource:
    beqz    t0, .Lfail
    add     t4, t0, t3
    addi    t4, t4, 8
    bgeu    s6, s4, .Lzero
    lbu     t5, 0(t4)
    j       .Lstore
.Lzero:
    li      t5, 0
.Lstore:
    sb      t5, 0(s3)
    addi    s3, s3, 1
    addi    s6, s6, 1
    addi    t3, t3, 1
    j       .Lbyte
.Lnext_segment:
    addi    s1, s1, 32
    addi    s2, s2, -1
    j       .Lsegment
.Ljump:
    fence   rw, rw
    csrw    satp, zero
    sfence.vma
    fence.i
    ld      a0, 8(s0)
    ld      a1, 16(s0)
    ld      t0, 0(s0)
    jr      t0
.Lfail:
    wfi
    j       .Lfail
a20_rv64_kexec_trampoline_end:

/*
 * A20OS vDSO for riscv64 — user-space clock_gettime/gettimeofday/getcpu.
 *
 * Linked at vaddr 0 with file offsets == virtual offsets so that the raw
 * binary doubles as the in-memory image; musl computes the load bias from
 * PT_LOAD itself.  The vvar page address is absolute and must match
 * A20_VVAR_VA in kernel/include/mm/vdso.h.
 *
 * Time math: ns = base_ns + ((rdtime - base_cyc) * mult) >> 32, where
 * mult = (1e9 << 32) / timebase_freq.  The product is 128-bit wide
 * (mulhu/mul) and the seqlock retries while the kernel updates the anchor.
 */
    .equ VVAR_ADDR,  0x3FFC2000
    .equ VVAR_SEQ,   0
    .equ VVAR_MULT,  8
    .equ VVAR_BOOT,  16
    .equ VVAR_RTCYC, 24
    .equ VVAR_RTSEC, 32
    .equ VVAR_RTNS,  40

    .equ __NR_clock_gettime, 113
    .equ __NR_gettimeofday,  169

    .section .text

/* int __vdso_clock_gettime(clockid_t a0, struct timespec *a1) */
    .globl __vdso_clock_gettime
    .type  __vdso_clock_gettime,@function
__vdso_clock_gettime:
    li   t1, 1
    bgtu a0, t1, .Lcg_syscall
    lui  t6, %hi(VVAR_ADDR)
.Lcg_retry:
    lw   t0, VVAR_SEQ(t6)
    andi t2, t0, 1
    bnez t2, .Lcg_retry
    fence r, r
    ld   t5, VVAR_MULT(t6)
    beqz a0, .Lcg_real
    ld   t2, VVAR_BOOT(t6)
    li   t3, 0
    li   t4, 0
    j    .Lcg_calc
.Lcg_real:
    ld   t2, VVAR_RTCYC(t6)
    ld   t3, VVAR_RTSEC(t6)
    ld   t4, VVAR_RTNS(t6)
.Lcg_calc:
    csrr a5, time
    sub  a5, a5, t2
    mulhu a4, a5, t5
    mul  a5, a5, t5
    srli a5, a5, 32
    slli a4, a4, 32
    or   a5, a5, a4
    add  t4, t4, a5
    li   a4, 1000000000
    divu a2, t4, a4
    remu t4, t4, a4
    add  t3, t3, a2
    fence r, r
    lw   a3, VVAR_SEQ(t6)
    bne  a3, t0, .Lcg_retry
    sd   t3, 0(a1)
    sd   t4, 8(a1)
    li   a0, 0
    ret
.Lcg_syscall:
    li   a7, __NR_clock_gettime
    ecall
    ret

/* int __vdso_gettimeofday(struct timeval *a0, struct timezone *a1) */
    .globl __vdso_gettimeofday
    .type  __vdso_gettimeofday,@function
__vdso_gettimeofday:
    beqz a0, .Lgt_tz
    lui  t6, %hi(VVAR_ADDR)
.Lgt_retry:
    lw   t0, VVAR_SEQ(t6)
    andi t2, t0, 1
    bnez t2, .Lgt_retry
    fence r, r
    ld   t5, VVAR_MULT(t6)
    ld   t2, VVAR_RTCYC(t6)
    ld   t3, VVAR_RTSEC(t6)
    ld   t4, VVAR_RTNS(t6)
    csrr a5, time
    sub  a5, a5, t2
    mulhu a4, a5, t5
    mul  a5, a5, t5
    srli a5, a5, 32
    slli a4, a4, 32
    or   a5, a5, a4
    add  t4, t4, a5
    li   a4, 1000000000
    divu a2, t4, a4
    remu t4, t4, a4
    add  t3, t3, a2
    fence r, r
    lw   a3, VVAR_SEQ(t6)
    bne  a3, t0, .Lgt_retry
    li   a4, 1000
    divu t4, t4, a4
    sd   t3, 0(a0)
    sd   t4, 8(a0)
.Lgt_tz:
    beqz a1, .Lgt_done
    sw   zero, 0(a1)
    sw   zero, 4(a1)
.Lgt_done:
    li   a0, 0
    ret

/* int __vdso_getcpu(unsigned *a0, unsigned *a1, void *a2) */
    .globl __vdso_getcpu
    .type  __vdso_getcpu,@function
__vdso_getcpu:
    beqz a0, 1f
    sw   zero, 0(a0)
1:  beqz a1, 2f
    sw   zero, 0(a1)
2:  li   a0, 0
    ret

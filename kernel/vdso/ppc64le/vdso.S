/*
 * A20OS vDSO for ppc64le — user-space clock_gettime/gettimeofday/getcpu.
 *
 * Linked at vaddr 0 with file offsets == virtual offsets so that the raw
 * binary doubles as the in-memory image; musl computes the load bias from
 * PT_LOAD itself.  The absolute vvar address comes from the same shared
 * layout header used by the kernel mapper.
 *
 * Time math: ns = base_nsec + ((mftb - base_cyc) * mult) >> 32, where
 * mult = (1e9 << 32) / timebase_freq.  The product is 128-bit
 * (mulld/mulhdu) and the seqlock retries while the kernel updates the anchor.
 * This toolchain spells registers numerically (3, 9, ...), matching the
 * kernel's ppc64le assembly.
 */
#include "mm/vdso_layout.h"

    .equ VVAR_SEQ,   0
    .equ VVAR_MULT,  8
    .equ VVAR_BOOT,  16
    .equ VVAR_RTCYC, 24
    .equ VVAR_RTSEC, 32
    .equ VVAR_RTNS,  40

    .equ __NR_clock_gettime, 246
    .equ __NR_gettimeofday,  78

    .abiversion 2
    .text

/* int __vdso_clock_gettime(clockid_t r3, struct timespec *r4) */
    .globl __vdso_clock_gettime
    .type  __vdso_clock_gettime,@function
__vdso_clock_gettime:
    cmplwi  3, 1
    bgt     .Lcg_syscall
    lis     9, 0x3f7f
    ori     9, 9, 0xe000
.Lcg_retry:
    lwz     10, VVAR_SEQ(9)
    andi.   11, 10, 1
    bne     .Lcg_retry
    ld      11, VVAR_MULT(9)
    cmpwi   3, 0
    beq     .Lcg_real
    li      12, 0
    li      13, 0
    ld      14, VVAR_BOOT(9)
    b       .Lcg_calc
.Lcg_real:
    ld      14, VVAR_RTCYC(9)
    ld      12, VVAR_RTSEC(9)
    ld      13, VVAR_RTNS(9)
.Lcg_calc:
    mftb    15
    sub     15, 15, 14
    mulld   16, 15, 11
    mulhdu  17, 15, 11
    srdi    16, 16, 32
    sldi    17, 17, 32
    or      16, 16, 17
    add     16, 16, 13
    lis     17, 0x3b9a
    ori     17, 17, 0xca00
    divdu   18, 16, 17
    mulld   19, 18, 17
    sub     19, 16, 19
    add     18, 18, 12
    lwz     20, VVAR_SEQ(9)
    cmpw    20, 10
    bne     .Lcg_retry
    std     18, 0(4)
    std     19, 8(4)
    li      3, 0
    blr
.Lcg_syscall:
    li      0, __NR_clock_gettime
    sc
    blr

/* int __vdso_gettimeofday(struct timeval *r3, struct timezone *r4) */
    .globl __vdso_gettimeofday
    .type  __vdso_gettimeofday,@function
__vdso_gettimeofday:
    cmpdi   3, 0
    beq     .Lgt_tz
    lis     9, 0x3f7f
    ori     9, 9, 0xe000
.Lgt_retry:
    lwz     10, VVAR_SEQ(9)
    andi.   11, 10, 1
    bne     .Lgt_retry
    ld      11, VVAR_MULT(9)
    ld      14, VVAR_RTCYC(9)
    ld      12, VVAR_RTSEC(9)
    ld      13, VVAR_RTNS(9)
    mftb    15
    sub     15, 15, 14
    mulld   16, 15, 11
    mulhdu  17, 15, 11
    srdi    16, 16, 32
    sldi    17, 17, 32
    or      16, 16, 17
    add     16, 16, 13
    lis     17, 0x3b9a
    ori     17, 17, 0xca00
    divdu   18, 16, 17
    mulld   19, 18, 17
    sub     19, 16, 19
    add     18, 18, 12
    li      20, 1000
    divdu   19, 19, 20
    lwz     21, VVAR_SEQ(9)
    cmpw    21, 10
    bne     .Lgt_retry
    std     18, 0(3)
    std     19, 8(3)
.Lgt_tz:
    cmpdi   4, 0
    beq     .Lgt_done
    li      5, 0
    stw     5, 0(4)
    stw     5, 4(4)
.Lgt_done:
    li      3, 0
    blr

/* int __vdso_getcpu(unsigned *r3, unsigned *r4, void *r5) */
    .globl __vdso_getcpu
    .type  __vdso_getcpu,@function
__vdso_getcpu:
    li      6, 0
    cmpdi   3, 0
    beq     1f
    stw     6, 0(3)
1:  cmpdi   4, 0
    beq     2f
    stw     6, 0(4)
2:  li      3, 0
    blr

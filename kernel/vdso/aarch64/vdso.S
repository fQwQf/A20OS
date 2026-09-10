/*
 * A20OS vDSO for aarch64 — user-space clock_gettime/gettimeofday/getcpu.
 *
 * Linked at vaddr 0 with file offsets == virtual offsets so that the raw
 * binary doubles as the in-memory image; musl computes the load bias from
 * PT_LOAD itself.  The absolute vvar address comes from the same shared
 * layout header used by the kernel mapper.
 *
 * Time math: ns = base_nsec + ((cntvct_el0 - base_cyc) * mult) >> 32, where
 * mult = (1e9 << 32) / counter_freq.  The product is 128-bit (mul/umulh) and
 * the seqlock retries while the kernel updates the anchor.  EL0 reads the
 * virtual counter (cntvct_el0); the kernel uses the physical counter
 * (cntpct_el0), which QEMU keeps identical (CNTVOFF_EL2 == 0).
 */
#include "mm/vdso_layout.h"

    .equ VVAR_SEQ,   0
    .equ VVAR_MULT,  8
    .equ VVAR_BOOT,  16
    .equ VVAR_RTCYC, 24
    .equ VVAR_RTSEC, 32
    .equ VVAR_RTNS,  40

    .equ __NR_clock_gettime, 113
    .equ __NR_gettimeofday,  169

    .text

/* int __vdso_clock_gettime(clockid_t w0, struct timespec *x1) */
    .globl __vdso_clock_gettime
    .type  __vdso_clock_gettime,%function
__vdso_clock_gettime:
    cmp     w0, #1
    b.hi    .Lcg_syscall
    movz    x9, #((A20_VVAR_VA >> 0) & 0xffff)
    movk    x9, #((A20_VVAR_VA >> 16) & 0xffff), lsl #16
    movk    x9, #((A20_VVAR_VA >> 32) & 0xffff), lsl #32
    movk    x9, #((A20_VVAR_VA >> 48) & 0xffff), lsl #48
.Lcg_retry:
    ldr     w10, [x9, #VVAR_SEQ]
    tbnz    w10, #0, .Lcg_retry
    dmb     ishld
    ldr     x11, [x9, #VVAR_MULT]
    cbz     w0, .Lcg_real
    mov     x12, #0
    mov     x13, #0
    ldr     x14, [x9, #VVAR_BOOT]
    b       .Lcg_calc
.Lcg_real:
    ldr     x14, [x9, #VVAR_RTCYC]
    ldr     x12, [x9, #VVAR_RTSEC]
    ldr     x13, [x9, #VVAR_RTNS]
.Lcg_calc:
    mrs     x15, cntvct_el0
    sub     x15, x15, x14
    mul     x16, x15, x11
    umulh   x17, x15, x11
    lsr     x16, x16, #32
    lsl     x17, x17, #32
    orr     x16, x16, x17
    add     x16, x16, x13
    movz    x17, #0xca00
    movk    x17, #0x3b9a, lsl #16
    udiv    x18, x16, x17
    msub    x19, x18, x17, x16
    add     x18, x18, x12
    dmb     ishld
    ldr     w20, [x9, #VVAR_SEQ]
    cmp     w20, w10
    b.ne    .Lcg_retry
    str     x18, [x1, #0]
    str     x19, [x1, #8]
    mov     w0, #0
    ret
.Lcg_syscall:
    mov     x8, #__NR_clock_gettime
    svc     #0
    ret

/* int __vdso_gettimeofday(struct timeval *x0, struct timezone *x1) */
    .globl __vdso_gettimeofday
    .type  __vdso_gettimeofday,%function
__vdso_gettimeofday:
    cbz     x0, .Lgt_tz
    movz    x9, #((A20_VVAR_VA >> 0) & 0xffff)
    movk    x9, #((A20_VVAR_VA >> 16) & 0xffff), lsl #16
    movk    x9, #((A20_VVAR_VA >> 32) & 0xffff), lsl #32
    movk    x9, #((A20_VVAR_VA >> 48) & 0xffff), lsl #48
.Lgt_retry:
    ldr     w10, [x9, #VVAR_SEQ]
    tbnz    w10, #0, .Lgt_retry
    dmb     ishld
    ldr     x11, [x9, #VVAR_MULT]
    ldr     x14, [x9, #VVAR_RTCYC]
    ldr     x12, [x9, #VVAR_RTSEC]
    ldr     x13, [x9, #VVAR_RTNS]
    mrs     x15, cntvct_el0
    sub     x15, x15, x14
    mul     x16, x15, x11
    umulh   x17, x15, x11
    lsr     x16, x16, #32
    lsl     x17, x17, #32
    orr     x16, x16, x17
    add     x16, x16, x13
    movz    x17, #0xca00
    movk    x17, #0x3b9a, lsl #16
    udiv    x18, x16, x17
    msub    x19, x18, x17, x16
    add     x18, x18, x12
    mov     x20, #1000
    udiv    x19, x19, x20
    dmb     ishld
    ldr     w21, [x9, #VVAR_SEQ]
    cmp     w21, w10
    b.ne    .Lgt_retry
    str     x18, [x0, #0]
    str     x19, [x0, #8]
.Lgt_tz:
    cbz     x1, .Lgt_done
    str     wzr, [x1, #0]
    str     wzr, [x1, #4]
.Lgt_done:
    mov     w0, #0
    ret

/* int __vdso_getcpu(unsigned *x0, unsigned *x1, void *x2) */
    .globl __vdso_getcpu
    .type  __vdso_getcpu,%function
__vdso_getcpu:
    cbz     x0, 1f
    str     wzr, [x0, #0]
1:  cbz     x1, 2f
    str     wzr, [x1, #0]
2:  mov     w0, #0
    ret

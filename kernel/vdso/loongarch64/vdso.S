/*
 * A20OS vDSO for loongarch64 — user-space clock_gettime/gettimeofday/getcpu.
 *
 * Linked at vaddr 0 with file offsets == virtual offsets so that the raw
 * binary doubles as the in-memory image; musl computes the load bias from
 * PT_LOAD itself.  The absolute vvar address comes from the same shared
 * layout header used by the kernel mapper.
 *
 * Time math: ns = base_ns + ((rdtime.d - base_cyc) * mult) >> 32, where
 * mult = (1e9 << 32) / timebase_freq.  The product is 128-bit wide
 * (mulh.du/mul) and the seqlock retries while the kernel updates the anchor.
 * QEMU loongarch64 rdtime.d ticks at 100 MHz (10 ns period).
 */
#include "mm/vdso_layout.h"

    .equ VVAR_ADDR,  A20_VVAR_VA
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
    li.w   $t1, 1
    bgtu   $a0, $t1, .Lcg_syscall
    li.d   $t6, VVAR_ADDR

.Lcg_retry:
    ld.w   $t0, $t6, VVAR_SEQ
    andi   $t2, $t0, 1
    bnez   $t2, .Lcg_retry
    dbar   0
    ld.d   $t5, $t6, VVAR_MULT
    bnez   $a0, .Lcg_mono
    ld.d   $t2, $t6, VVAR_RTCYC
    ld.d   $t3, $t6, VVAR_RTSEC
    ld.d   $t4, $t6, VVAR_RTNS
    b      .Lcg_calc
.Lcg_mono:
    ld.d   $t2, $t6, VVAR_BOOT
    or     $t3, $zero, $zero
    or     $t4, $zero, $zero
.Lcg_calc:
    rdtime.d $a5, $zero
    sub.d  $a5, $a5, $t2
    mulh.du $a4, $a5, $t5
    mul.d  $a5, $a5, $t5
    srli.d $a5, $a5, 32
    slli.d $a4, $a4, 32
    or     $a5, $a5, $a4
    add.d  $t4, $t4, $a5
    li.w   $a4, 1000000000
    div.du $a2, $t4, $a4
    mod.du $t4, $t4, $a4
    add.d  $t3, $t3, $a2
    dbar   0
    ld.w   $a3, $t6, VVAR_SEQ
    bne    $a3, $t0, .Lcg_retry
    st.d   $t3, $a1, 0
    st.d   $t4, $a1, 8
    or     $a0, $zero, $zero
    jr     $ra
.Lcg_syscall:
    li.w   $a7, __NR_clock_gettime
    syscall 0
    jr     $ra
    .size  __vdso_clock_gettime, .-__vdso_clock_gettime

/* int __vdso_gettimeofday(struct timeval *a0, struct timezone *a1) */
    .globl __vdso_gettimeofday
    .type  __vdso_gettimeofday,@function
__vdso_gettimeofday:
    beqz   $a0, .Lgt_tz
    li.d   $t6, VVAR_ADDR

.Lgt_retry:
    ld.w   $t0, $t6, VVAR_SEQ
    andi   $t2, $t0, 1
    bnez   $t2, .Lgt_retry
    dbar   0
    ld.d   $t5, $t6, VVAR_MULT
    ld.d   $t2, $t6, VVAR_RTCYC
    ld.d   $t3, $t6, VVAR_RTSEC
    ld.d   $t4, $t6, VVAR_RTNS
    rdtime.d $a5, $zero
    sub.d  $a5, $a5, $t2
    mulh.du $a4, $a5, $t5
    mul.d  $a5, $a5, $t5
    srli.d $a5, $a5, 32
    slli.d $a4, $a4, 32
    or     $a5, $a5, $a4
    add.d  $t4, $t4, $a5
    li.w   $a4, 1000000000
    div.du $a2, $t4, $a4
    mod.du $t4, $t4, $a4
    add.d  $t3, $t3, $a2
    dbar   0
    ld.w   $a3, $t6, VVAR_SEQ
    bne    $a3, $t0, .Lgt_retry
    li.w   $a4, 1000
    div.du $t4, $t4, $a4
    st.d   $t3, $a0, 0
    st.d   $t4, $a0, 8
.Lgt_tz:
    beqz   $a1, .Lgt_done
    st.w   $zero, $a1, 0
    st.w   $zero, $a1, 4
.Lgt_done:
    or     $a0, $zero, $zero
    jr     $ra
    .size  __vdso_gettimeofday, .-__vdso_gettimeofday

/* int __vdso_getcpu(unsigned *a0, unsigned *a1, void *a2)
 *
 * Refuses with -ENOSYS rather than reporting a CPU.  -ENOSYS is the one
 * value musl's wrapper understands as "not available": src/sched/
 * sched_getcpu.c turns any other non-zero result into a hard error, but
 * falls through to the getcpu syscall on -ENOSYS, and that syscall returns
 * the true cpu_current_id() (kernel/abi/linux/sys_sched.c).  So the caller
 * gets the truth, and the fast path costs one wasted trap instead of a lie.
 *
 * The CPUID CSR (0x20) the kernel reads is not a usable substitute: its low
 * word is a packed descriptor (PRMD/REFGPL/UAL/RI/CC, with CPUNUM at bits
 * 23:10), not the dense logical id, and cpu_current_id() clamps anything at
 * or above NR_CPUS back to 0 (kernel/include/core/cpu.h).  A raw read would
 * hand callers a number the syscall never returns, which is a second kind of
 * wrong answer.  The global vvar page (kernel/mm/vdso.c) is shared by every
 * CPU, and A20OS keeps no per-CPU area pointer, so there is nothing else to
 * read.  *a0 and *a1 are left untouched: a plausible wrong CPU 0 is worse
 * than no answer at all (docs/security/hardening.md).
 * See kernel/include/mm/vdso.h. */
    .globl __vdso_getcpu
    .type  __vdso_getcpu,@function
__vdso_getcpu:
    li.w   $a0, -38
    jr     $ra
    .size  __vdso_getcpu, .-__vdso_getcpu

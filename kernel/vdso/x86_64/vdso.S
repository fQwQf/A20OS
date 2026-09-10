/*
 * A20OS vDSO for x86_64 — user-space clock_gettime/gettimeofday/getcpu.
 *
 * Linked at vaddr 0 with file offsets == virtual offsets so that the raw
 * binary doubles as the in-memory image; musl computes the load bias from
 * PT_LOAD itself.  The absolute vvar address comes from the same shared
 * layout header used by the kernel mapper.
 *
 * Time math: ns = base_nsec + ((rdtsc - base_cyc) * mult) >> 32, where
 * mult = (1e9 << 32) / tsc_freq (the kernel passes the raw TSC frequency,
 * not the ARCH_TIMER_FREQ-scaled tick).  The product is 128-bit (mul) and
 * the seqlock retries while the kernel updates the anchor.
 */
#include "mm/vdso_layout.h"

    .equ VVAR_SEQ,   0
    .equ VVAR_MULT,  8
    .equ VVAR_BOOT,  16
    .equ VVAR_RTCYC, 24
    .equ VVAR_RTSEC, 32
    .equ VVAR_RTNS,  40

    .equ __NR_clock_gettime, 228
    .equ __NR_gettimeofday,  96

    .text

/* int __vdso_clock_gettime(clockid_t clockid, struct timespec *ts) */
    .globl __vdso_clock_gettime
    .type  __vdso_clock_gettime,@function
__vdso_clock_gettime:
    cmp     $1, %edi
    ja      .Lcg_syscall
    push    %rbx
    movabs  $A20_VVAR_VA, %r8
.Lcg_retry:
    mov     VVAR_SEQ(%r8), %ebx
    test    $1, %bl
    jne     .Lcg_retry
    lfence
    mov     VVAR_MULT(%r8), %r9
    test    %edi, %edi
    jz      .Lcg_real
    xor     %r10d, %r10d
    xor     %r11d, %r11d
    mov     VVAR_BOOT(%r8), %rcx
    jmp     .Lcg_calc
.Lcg_real:
    mov     VVAR_RTCYC(%r8), %rcx
    mov     VVAR_RTSEC(%r8), %r10
    mov     VVAR_RTNS(%r8), %r11
.Lcg_calc:
    rdtsc
    shl     $32, %rdx
    or      %rdx, %rax
    sub     %rcx, %rax
    mul     %r9
    shrd    $32, %rdx, %rax
    add     %r11, %rax
    xor     %edx, %edx
    mov     $1000000000, %ecx
    div     %rcx
    add     %r10, %rax
    lfence
    mov     VVAR_SEQ(%r8), %r9d
    cmp     %ebx, %r9d
    jne     .Lcg_retry
    mov     %rax, 0(%rsi)
    mov     %rdx, 8(%rsi)
    xor     %eax, %eax
    pop     %rbx
    ret
.Lcg_syscall:
    mov     $__NR_clock_gettime, %eax
    syscall
    ret

/* int __vdso_gettimeofday(struct timeval *tv, struct timezone *tz) */
    .globl __vdso_gettimeofday
    .type  __vdso_gettimeofday,@function
__vdso_gettimeofday:
    push    %rbx
    test    %rdi, %rdi
    jz      .Lgt_tz
    movabs  $A20_VVAR_VA, %r8
.Lgt_retry:
    mov     VVAR_SEQ(%r8), %ebx
    test    $1, %bl
    jne     .Lgt_retry
    lfence
    mov     VVAR_MULT(%r8), %r9
    mov     VVAR_RTCYC(%r8), %rcx
    mov     VVAR_RTSEC(%r8), %r10
    mov     VVAR_RTNS(%r8), %r11
    rdtsc
    shl     $32, %rdx
    or      %rdx, %rax
    sub     %rcx, %rax
    mul     %r9
    shrd    $32, %rdx, %rax
    add     %r11, %rax
    xor     %edx, %edx
    mov     $1000000000, %ecx
    div     %rcx
    add     %r10, %rax
    mov     %rax, %r9
    mov     %rdx, %rax
    xor     %edx, %edx
    mov     $1000, %ecx
    div     %rcx
    lfence
    mov     VVAR_SEQ(%r8), %r11d
    cmp     %ebx, %r11d
    jne     .Lgt_retry
    mov     %r9, 0(%rdi)
    mov     %rax, 8(%rdi)
.Lgt_tz:
    test    %rsi, %rsi
    jz      .Lgt_done
    movl    $0, 0(%rsi)
    movl    $0, 4(%rsi)
.Lgt_done:
    xor     %eax, %eax
    pop     %rbx
    ret

/* int __vdso_getcpu(unsigned *cpu, unsigned *node, void *cache) */
    .globl __vdso_getcpu
    .type  __vdso_getcpu,@function
__vdso_getcpu:
    test    %rdi, %rdi
    jz      1f
    movl    $0, 0(%rdi)
1:  test    %rsi, %rsi
    jz      2f
    movl    $0, 0(%rsi)
2:  xor     %eax, %eax
    ret

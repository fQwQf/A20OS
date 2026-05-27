/*
 * A20OS trap handling for AArch64.
 */

#define X_OFF(n)        ((n) * 8)
#define SP_OFF          (31 * 8)
#define ELR_OFF         (32 * 8)
#define SPSR_OFF        (33 * 8)
#define TPIDR_OFF       (34 * 8)
#define TTBR0_OFF       (35 * 8)
#define KTP_OFF         (36 * 8)
#define KSP_OFF         (37 * 8)
#define V_OFF(n)        ((38 * 8) + ((n) * 16))
#define FPCR_OFF        ((102) * 8)
#define FPSR_OFF        ((103) * 8)
#define CTX_SIZE        (104 * 8)

    .macro VECTOR_ENTRY target
        b       \target
        .space  0x80 - 4
    .endm

    .section .text
    .align 11
    .global aarch64_vector_table
aarch64_vector_table:
    VECTOR_ENTRY aarch64_sync_el1
    VECTOR_ENTRY aarch64_irq_el1
    VECTOR_ENTRY aarch64_sync_el1
    VECTOR_ENTRY aarch64_sync_el1
    VECTOR_ENTRY aarch64_sync_el1
    VECTOR_ENTRY aarch64_irq_el1
    VECTOR_ENTRY aarch64_sync_el1
    VECTOR_ENTRY aarch64_sync_el1
    VECTOR_ENTRY aarch64_sync_el0
    VECTOR_ENTRY aarch64_irq_el0
    VECTOR_ENTRY aarch64_sync_el0
    VECTOR_ENTRY aarch64_sync_el0
    VECTOR_ENTRY aarch64_sync_el0
    VECTOR_ENTRY aarch64_irq_el0
    VECTOR_ENTRY aarch64_sync_el0
    VECTOR_ENTRY aarch64_sync_el0

    .global __trap_from_user
    .global __trap_from_kernel
    .global __return_to_user
    .global user_trap_return

aarch64_sync_el0:
    adrp    x9, aarch64_trap_flags
    add     x9, x9, :lo12:aarch64_trap_flags
    str     xzr, [x9]
    b       __trap_from_user

aarch64_irq_el0:
    adrp    x9, aarch64_trap_flags
    add     x9, x9, :lo12:aarch64_trap_flags
    mov     x10, #1
    str     x10, [x9]
    b       __trap_from_user

aarch64_sync_el1:
    adrp    x9, aarch64_trap_flags
    add     x9, x9, :lo12:aarch64_trap_flags
    str     xzr, [x9]
    b       __trap_from_kernel

aarch64_irq_el1:
    adrp    x9, aarch64_trap_flags
    add     x9, x9, :lo12:aarch64_trap_flags
    mov     x10, #1
    str     x10, [x9]
    b       __trap_from_kernel

__trap_from_user:
    sub     sp, sp, #CTX_SIZE
    str     x0,  [sp, #X_OFF(0)]
    str     x1,  [sp, #X_OFF(1)]
    str     x2,  [sp, #X_OFF(2)]
    str     x3,  [sp, #X_OFF(3)]
    str     x4,  [sp, #X_OFF(4)]
    str     x5,  [sp, #X_OFF(5)]
    str     x6,  [sp, #X_OFF(6)]
    str     x7,  [sp, #X_OFF(7)]
    str     x8,  [sp, #X_OFF(8)]
    str     x9,  [sp, #X_OFF(9)]
    str     x10, [sp, #X_OFF(10)]
    str     x11, [sp, #X_OFF(11)]
    str     x12, [sp, #X_OFF(12)]
    str     x13, [sp, #X_OFF(13)]
    str     x14, [sp, #X_OFF(14)]
    str     x15, [sp, #X_OFF(15)]
    str     x16, [sp, #X_OFF(16)]
    str     x17, [sp, #X_OFF(17)]
    str     x18, [sp, #X_OFF(18)]
    str     x19, [sp, #X_OFF(19)]
    str     x20, [sp, #X_OFF(20)]
    str     x21, [sp, #X_OFF(21)]
    str     x22, [sp, #X_OFF(22)]
    str     x23, [sp, #X_OFF(23)]
    str     x24, [sp, #X_OFF(24)]
    str     x25, [sp, #X_OFF(25)]
    str     x26, [sp, #X_OFF(26)]
    str     x27, [sp, #X_OFF(27)]
    str     x28, [sp, #X_OFF(28)]
    str     x29, [sp, #X_OFF(29)]
    str     x30, [sp, #X_OFF(30)]

    mrs     x9, sp_el0
    str     x9, [sp, #SP_OFF]
    mrs     x9, elr_el1
    str     x9, [sp, #ELR_OFF]
    mrs     x9, spsr_el1
    str     x9, [sp, #SPSR_OFF]
    mrs     x9, tpidr_el0
    str     x9, [sp, #TPIDR_OFF]
    mrs     x9, ttbr0_el1
    str     x9, [sp, #TTBR0_OFF]
    mrs     x9, tpidr_el1
    str     x9, [sp, #KTP_OFF]
    add     x9, sp, #CTX_SIZE
    str     x9, [sp, #KSP_OFF]
    stp     q0,  q1,  [sp, #V_OFF(0)]
    stp     q2,  q3,  [sp, #V_OFF(2)]
    stp     q4,  q5,  [sp, #V_OFF(4)]
    stp     q6,  q7,  [sp, #V_OFF(6)]
    stp     q8,  q9,  [sp, #V_OFF(8)]
    stp     q10, q11, [sp, #V_OFF(10)]
    stp     q12, q13, [sp, #V_OFF(12)]
    stp     q14, q15, [sp, #V_OFF(14)]
    stp     q16, q17, [sp, #V_OFF(16)]
    stp     q18, q19, [sp, #V_OFF(18)]
    stp     q20, q21, [sp, #V_OFF(20)]
    stp     q22, q23, [sp, #V_OFF(22)]
    stp     q24, q25, [sp, #V_OFF(24)]
    stp     q26, q27, [sp, #V_OFF(26)]
    stp     q28, q29, [sp, #V_OFF(28)]
    stp     q30, q31, [sp, #V_OFF(30)]
    mrs     x9, fpcr
    str     x9, [sp, #FPCR_OFF]
    mrs     x9, fpsr
    str     x9, [sp, #FPSR_OFF]

    mov     x0, sp
    bl      trap_handler
    b       __return_to_user

__trap_from_kernel:
    sub     sp, sp, #CTX_SIZE
    str     x0,  [sp, #X_OFF(0)]
    str     x1,  [sp, #X_OFF(1)]
    str     x2,  [sp, #X_OFF(2)]
    str     x3,  [sp, #X_OFF(3)]
    str     x4,  [sp, #X_OFF(4)]
    str     x5,  [sp, #X_OFF(5)]
    str     x6,  [sp, #X_OFF(6)]
    str     x7,  [sp, #X_OFF(7)]
    str     x8,  [sp, #X_OFF(8)]
    str     x9,  [sp, #X_OFF(9)]
    str     x10, [sp, #X_OFF(10)]
    str     x11, [sp, #X_OFF(11)]
    str     x12, [sp, #X_OFF(12)]
    str     x13, [sp, #X_OFF(13)]
    str     x14, [sp, #X_OFF(14)]
    str     x15, [sp, #X_OFF(15)]
    str     x16, [sp, #X_OFF(16)]
    str     x17, [sp, #X_OFF(17)]
    str     x18, [sp, #X_OFF(18)]
    str     x19, [sp, #X_OFF(19)]
    str     x20, [sp, #X_OFF(20)]
    str     x21, [sp, #X_OFF(21)]
    str     x22, [sp, #X_OFF(22)]
    str     x23, [sp, #X_OFF(23)]
    str     x24, [sp, #X_OFF(24)]
    str     x25, [sp, #X_OFF(25)]
    str     x26, [sp, #X_OFF(26)]
    str     x27, [sp, #X_OFF(27)]
    str     x28, [sp, #X_OFF(28)]
    str     x29, [sp, #X_OFF(29)]
    str     x30, [sp, #X_OFF(30)]

    add     x9, sp, #CTX_SIZE
    str     x9, [sp, #SP_OFF]
    mrs     x9, elr_el1
    str     x9, [sp, #ELR_OFF]
    mrs     x9, spsr_el1
    str     x9, [sp, #SPSR_OFF]
    mrs     x9, tpidr_el0
    str     x9, [sp, #TPIDR_OFF]
    mrs     x9, ttbr0_el1
    str     x9, [sp, #TTBR0_OFF]
    mrs     x9, tpidr_el1
    str     x9, [sp, #KTP_OFF]
    add     x9, sp, #CTX_SIZE
    str     x9, [sp, #KSP_OFF]
    stp     q0,  q1,  [sp, #V_OFF(0)]
    stp     q2,  q3,  [sp, #V_OFF(2)]
    stp     q4,  q5,  [sp, #V_OFF(4)]
    stp     q6,  q7,  [sp, #V_OFF(6)]
    stp     q8,  q9,  [sp, #V_OFF(8)]
    stp     q10, q11, [sp, #V_OFF(10)]
    stp     q12, q13, [sp, #V_OFF(12)]
    stp     q14, q15, [sp, #V_OFF(14)]
    stp     q16, q17, [sp, #V_OFF(16)]
    stp     q18, q19, [sp, #V_OFF(18)]
    stp     q20, q21, [sp, #V_OFF(20)]
    stp     q22, q23, [sp, #V_OFF(22)]
    stp     q24, q25, [sp, #V_OFF(24)]
    stp     q26, q27, [sp, #V_OFF(26)]
    stp     q28, q29, [sp, #V_OFF(28)]
    stp     q30, q31, [sp, #V_OFF(30)]
    mrs     x9, fpcr
    str     x9, [sp, #FPCR_OFF]
    mrs     x9, fpsr
    str     x9, [sp, #FPSR_OFF]

    mov     x0, sp
    bl      kernel_trap_handler

    mov     x2, sp
    ldr     x3, [x2, #SP_OFF]
    ldr     x9, [x2, #TTBR0_OFF]
    msr     ttbr0_el1, x9
    dsb     ishst
    tlbi    vmalle1
    dsb     ish
    isb
    ldr     x9, [x2, #ELR_OFF]
    msr     elr_el1, x9
    ldr     x9, [x2, #SPSR_OFF]
    msr     spsr_el1, x9
    ldp     q0,  q1,  [x2, #V_OFF(0)]
    ldp     q2,  q3,  [x2, #V_OFF(2)]
    ldp     q4,  q5,  [x2, #V_OFF(4)]
    ldp     q6,  q7,  [x2, #V_OFF(6)]
    ldp     q8,  q9,  [x2, #V_OFF(8)]
    ldp     q10, q11, [x2, #V_OFF(10)]
    ldp     q12, q13, [x2, #V_OFF(12)]
    ldp     q14, q15, [x2, #V_OFF(14)]
    ldp     q16, q17, [x2, #V_OFF(16)]
    ldp     q18, q19, [x2, #V_OFF(18)]
    ldp     q20, q21, [x2, #V_OFF(20)]
    ldp     q22, q23, [x2, #V_OFF(22)]
    ldp     q24, q25, [x2, #V_OFF(24)]
    ldp     q26, q27, [x2, #V_OFF(26)]
    ldp     q28, q29, [x2, #V_OFF(28)]
    ldp     q30, q31, [x2, #V_OFF(30)]
    ldr     x9, [x2, #FPCR_OFF]
    msr     fpcr, x9
    ldr     x9, [x2, #FPSR_OFF]
    msr     fpsr, x9
    ldr     x0,  [x2, #X_OFF(0)]
    ldr     x1,  [x2, #X_OFF(1)]
    ldr     x4,  [x2, #X_OFF(4)]
    ldr     x5,  [x2, #X_OFF(5)]
    ldr     x6,  [x2, #X_OFF(6)]
    ldr     x7,  [x2, #X_OFF(7)]
    ldr     x8,  [x2, #X_OFF(8)]
    ldr     x9,  [x2, #X_OFF(9)]
    ldr     x10, [x2, #X_OFF(10)]
    ldr     x11, [x2, #X_OFF(11)]
    ldr     x12, [x2, #X_OFF(12)]
    ldr     x13, [x2, #X_OFF(13)]
    ldr     x14, [x2, #X_OFF(14)]
    ldr     x15, [x2, #X_OFF(15)]
    ldr     x16, [x2, #X_OFF(16)]
    ldr     x17, [x2, #X_OFF(17)]
    ldr     x18, [x2, #X_OFF(18)]
    ldr     x19, [x2, #X_OFF(19)]
    ldr     x20, [x2, #X_OFF(20)]
    ldr     x21, [x2, #X_OFF(21)]
    ldr     x22, [x2, #X_OFF(22)]
    ldr     x23, [x2, #X_OFF(23)]
    ldr     x24, [x2, #X_OFF(24)]
    ldr     x25, [x2, #X_OFF(25)]
    ldr     x26, [x2, #X_OFF(26)]
    ldr     x27, [x2, #X_OFF(27)]
    ldr     x28, [x2, #X_OFF(28)]
    ldr     x29, [x2, #X_OFF(29)]
    ldr     x30, [x2, #X_OFF(30)]
    mov     sp, x3
    ldr     x3,  [x2, #X_OFF(3)]
    ldr     x2,  [x2, #X_OFF(2)]
    eret

__return_to_user:
user_trap_return:
    mov     x2, sp
    ldr     x3, [x2, #SP_OFF]
    ldr     x4, [x2, #KSP_OFF]
    ldr     x9, [x2, #TTBR0_OFF]
    msr     ttbr0_el1, x9
    dsb     ishst
    tlbi    vmalle1
    dsb     ish
    isb
    ldr     x9, [x2, #ELR_OFF]
    msr     elr_el1, x9
    ldr     x9, [x2, #SPSR_OFF]
    msr     spsr_el1, x9
    ldr     x9, [x2, #TPIDR_OFF]
    msr     tpidr_el0, x9
    ldr     x9, [x2, #KTP_OFF]
    msr     tpidr_el1, x9
    ldp     q0,  q1,  [x2, #V_OFF(0)]
    ldp     q2,  q3,  [x2, #V_OFF(2)]
    ldp     q4,  q5,  [x2, #V_OFF(4)]
    ldp     q6,  q7,  [x2, #V_OFF(6)]
    ldp     q8,  q9,  [x2, #V_OFF(8)]
    ldp     q10, q11, [x2, #V_OFF(10)]
    ldp     q12, q13, [x2, #V_OFF(12)]
    ldp     q14, q15, [x2, #V_OFF(14)]
    ldp     q16, q17, [x2, #V_OFF(16)]
    ldp     q18, q19, [x2, #V_OFF(18)]
    ldp     q20, q21, [x2, #V_OFF(20)]
    ldp     q22, q23, [x2, #V_OFF(22)]
    ldp     q24, q25, [x2, #V_OFF(24)]
    ldp     q26, q27, [x2, #V_OFF(26)]
    ldp     q28, q29, [x2, #V_OFF(28)]
    ldp     q30, q31, [x2, #V_OFF(30)]
    ldr     x9, [x2, #FPCR_OFF]
    msr     fpcr, x9
    ldr     x9, [x2, #FPSR_OFF]
    msr     fpsr, x9
    ldr     x0,  [x2, #X_OFF(0)]
    ldr     x1,  [x2, #X_OFF(1)]
    ldr     x5,  [x2, #X_OFF(5)]
    ldr     x6,  [x2, #X_OFF(6)]
    ldr     x7,  [x2, #X_OFF(7)]
    ldr     x8,  [x2, #X_OFF(8)]
    ldr     x9,  [x2, #X_OFF(9)]
    ldr     x10, [x2, #X_OFF(10)]
    ldr     x11, [x2, #X_OFF(11)]
    ldr     x12, [x2, #X_OFF(12)]
    ldr     x13, [x2, #X_OFF(13)]
    ldr     x14, [x2, #X_OFF(14)]
    ldr     x15, [x2, #X_OFF(15)]
    ldr     x16, [x2, #X_OFF(16)]
    ldr     x17, [x2, #X_OFF(17)]
    ldr     x18, [x2, #X_OFF(18)]
    ldr     x19, [x2, #X_OFF(19)]
    ldr     x20, [x2, #X_OFF(20)]
    ldr     x21, [x2, #X_OFF(21)]
    ldr     x22, [x2, #X_OFF(22)]
    ldr     x23, [x2, #X_OFF(23)]
    ldr     x24, [x2, #X_OFF(24)]
    ldr     x25, [x2, #X_OFF(25)]
    ldr     x26, [x2, #X_OFF(26)]
    ldr     x27, [x2, #X_OFF(27)]
    ldr     x28, [x2, #X_OFF(28)]
    ldr     x29, [x2, #X_OFF(29)]
    ldr     x30, [x2, #X_OFF(30)]
    msr     sp_el0, x3
    mov     sp, x4
    ldr     x3,  [x2, #X_OFF(3)]
    ldr     x4,  [x2, #X_OFF(4)]
    ldr     x2,  [x2, #X_OFF(2)]
    eret

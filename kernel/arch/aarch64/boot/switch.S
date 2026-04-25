/*
 * A20OS context switch for AArch64.
 *
 * void __switch(uint64_t next_kstack)
 *   x0 = next task's saved task_context_t
 */

    .section .text
    .global __switch
__switch:
    sub     sp, sp, #128

    str     x30, [sp, #(0 * 8)]
    mrs     x9, tpidr_el1
    str     x9,  [sp, #(1 * 8)]
    str     x19, [sp, #(2 * 8)]
    str     x20, [sp, #(3 * 8)]
    str     x21, [sp, #(4 * 8)]
    str     x22, [sp, #(5 * 8)]
    str     x23, [sp, #(6 * 8)]
    str     x24, [sp, #(7 * 8)]
    str     x25, [sp, #(8 * 8)]
    str     x26, [sp, #(9 * 8)]
    str     x27, [sp, #(10 * 8)]
    str     x28, [sp, #(11 * 8)]
    str     x29, [sp, #(12 * 8)]
    mrs     x10, ttbr0_el1
    str     x10, [sp, #(14 * 8)]
    mrs     x10, daif
    str     x10, [sp, #(15 * 8)]
    mov     x10, sp
    str     x10, [sp, #(13 * 8)]

    str     x10, [x9]

    mov     sp, x0

    ldr     x30, [sp, #(0 * 8)]
    ldr     x9,  [sp, #(1 * 8)]
    msr     tpidr_el1, x9
    ldr     x19, [sp, #(2 * 8)]
    ldr     x20, [sp, #(3 * 8)]
    ldr     x21, [sp, #(4 * 8)]
    ldr     x22, [sp, #(5 * 8)]
    ldr     x23, [sp, #(6 * 8)]
    ldr     x24, [sp, #(7 * 8)]
    ldr     x25, [sp, #(8 * 8)]
    ldr     x26, [sp, #(9 * 8)]
    ldr     x27, [sp, #(10 * 8)]
    ldr     x28, [sp, #(11 * 8)]
    ldr     x29, [sp, #(12 * 8)]
    ldr     x10, [sp, #(14 * 8)]
    msr     ttbr0_el1, x10
    ldr     x10, [sp, #(15 * 8)]
    msr     daif, x10
    dsb     ishst
    tlbi    vmalle1
    dsb     ish
    isb

    add     sp, sp, #128
    ret

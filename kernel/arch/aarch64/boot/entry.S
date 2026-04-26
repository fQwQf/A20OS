/*
 * A20OS kernel entry point for AArch64 (QEMU virt, EL1).
 *
 * QEMU loads the ELF segments at their LMA (physical address) and starts the
 * CPU in EL1 with the MMU off.  The kernel itself is linked at
 * VA = PAGE_OFFSET + PA.
 */

    .section .text.entry
    .global _start
_start:
    adrp    x0, _stack_end
    add     x0, x0, :lo12:_stack_end
    mov     sp, x0

    adrp    x0, _bss_start
    add     x0, x0, :lo12:_bss_start
    adrp    x1, _bss_end
    add     x1, x1, :lo12:_bss_end
1:
    cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    adrp    x0, boot_pgdir
    add     x0, x0, :lo12:boot_pgdir
    adrp    x1, boot_l1
    add     x1, x1, :lo12:boot_l1
    orr     x2, x1, #0x3
    str     x2, [x0, #(0 * 8)]
    str     x2, [x0, #(1 * 8)]

    ldr     x2, =0x0060000000000401  /* L1 block: PA 0x0, device, PXN|UXN */
    str     x2, [x1, #(0 * 8)]
    ldr     x2, =0x0040000040000705  /* L1 block: PA 0x40000000, normal, UXN */
    str     x2, [x1, #(1 * 8)]

    /* Enable FP/SIMD access at EL1/EL0. */
    mrs     x2, cpacr_el1
    orr     x2, x2, #(3 << 20)
    msr     cpacr_el1, x2
    isb

    /* MAIR: Attr0 = device nGnRnE, Attr1 = normal WBWA cacheable. */
    ldr     x2, =0x000000000000FF00
    msr     mair_el1, x2

    /* TCR_EL1: 48-bit VA, 4KB granule, inner-shareable WBWA, TTBR1 disabled. */
    ldr     x2, =0x0000000280003510
    msr     tcr_el1, x2

    msr     ttbr0_el1, x0
    dsb     ish
    isb

    mrs     x2, sctlr_el1
    orr     x2, x2, #(1 << 0)   /* M */
    orr     x2, x2, #(1 << 2)   /* C */
    orr     x2, x2, #(1 << 12)  /* I */
    msr     sctlr_el1, x2
    isb

    adrp    x0, .Lhh_entry
    add     x0, x0, :lo12:.Lhh_entry
    movz    x1, #0x0080, lsl #32
    add     x0, x0, x1
    br      x0

.Lhh_entry:
    adrp    x0, _stack_end
    add     x0, x0, :lo12:_stack_end
    mov     sp, x0

    adrp    x0, aarch64_vector_table
    add     x0, x0, :lo12:aarch64_vector_table
    msr     vbar_el1, x0
    isb

    bl      kernel_main

.Lhalt:
    wfi
    b       .Lhalt

    .section .bss
    .align 12
    .global boot_pgdir
boot_pgdir:
    .space 4096

    .align 12
boot_l1:
    .space 4096

    .section .bss.stack
    .align 12
_stack_bottom:
    .space 4096 * 16
    .global _stack_end
_stack_end:

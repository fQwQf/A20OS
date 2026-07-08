    .syntax unified
    .arch armv7-a

    .section .text.entry
    .global _start
_start:
    /* Running with the MMU off at the physical load address.
     * Use a small physical stack inside the entry section. */
    ldr     sp, =_early_stack_end

    /* Early debug: send 'A' to the physical PL011 UART. */
    ldr     r10, =0x09000000
1:
    ldr     r11, [r10, #0x18]
    tst     r11, #(1 << 5)
    bne     1b
    mov     r11, #'A'
    str     r11, [r10]

#ifndef CONFIG_NOMMU
    /* boot_pgdir is placed in this physical section. */
    ldr     r0, =boot_pgdir

    /* Map the first 16 MiB of physical memory (where the kernel is loaded)
     * to the low identity window (0x40000000) so that the MMU can be turned
     * on while we are still executing at the physical load address. */
    mov     r1, #0
    ldr     r2, =0x40000000
    ldr     r6, =0x60E
1:
    add     r3, r0, #(0x400 * 4)
    add     r3, r3, r1, lsl #2
    orr     r4, r2, r6
    str     r4, [r3]

    add     r2, r2, #0x100000
    add     r1, r1, #1
    cmp     r1, #16
    blo     1b

    /* Map all of physical RAM (1 GiB at PA 0x40000000) into the kernel
     * high virtual window at 0xC0000000 using 1 MiB sections.  This
     * matches the pfn_to_virt/PAGE_OFFSET convention used by the rest of
     * the kernel. */
    mov     r1, #0
    ldr     r2, =0x40000000
    ldr     r6, =0x60E
2:
    add     r3, r0, #(0xC00 * 4)
    add     r3, r3, r1, lsl #2
    orr     r4, r2, r6
    str     r4, [r3]

    add     r2, r2, #0x100000
    add     r1, r1, #1
    cmp     r1, #1024
    blo     2b

    /* Map the QEMU virt device region so that UART/GIC/virtio are usable
     * at their PAGE_OFFSET-relative virtual addresses. */
    ldr     r7, =(0x880 * 4)
    ldr     r1, =0x08000616          /* PA 0x08000000 -> VA 0x88000000, device */
    add     r3, r0, r7
    str     r1, [r3]

    ldr     r7, =(0x890 * 4)
    ldr     r1, =0x09000616          /* PA 0x09000000 -> VA 0x89000000 (UART) */
    add     r3, r0, r7
    str     r1, [r3]

    ldr     r7, =(0x8A0 * 4)
    ldr     r1, =0x0A000616          /* PA 0x0A000000 -> VA 0x8A000000 (virtio) */
    add     r3, r0, r7
    str     r1, [r3]

    /* Ensure page table writes reach memory before enabling the MMU. */
    dsb
#endif

    /* Enable VFP/NEON coprocessor access so that compiler-generated
     * hard-float code and libgcc helpers can run without causing an
     * undefined instruction exception. */
    mrc     p15, 0, r1, c1, c0, 2
    orr     r1, r1, #(0x3 << 22)     /* CP10 access: enable */
    orr     r1, r1, #(0x3 << 20)     /* CP11 access: enable */
    mcr     p15, 0, r1, c1, c0, 2
    isb
    mov     r1, #(1 << 30)
    fmxr    fpexc, r1                /* Enable VFP/NEON */
    isb

#ifndef CONFIG_NOMMU
    /* Invalidate TLBs and set the translation table base. */
    mov     r1, #0
    mcr     p15, 0, r1, c8, c7, 0
    mcr     p15, 0, r0, c2, c0, 0
    mcr     p15, 0, r1, c2, c0, 1
    ldr     r1, =0xFFFFFFFF          /* all domains manager */
    mcr     p15, 0, r1, c3, c0, 0

    /* Enable MMU only first; caches must stay off until the page tables
     * are visible to the translation walk in main memory. */
    mrc     p15, 0, r1, c1, c0, 0
    orr     r1, r1, #(1 << 0)        /* M  */
    bic     r1, r1, #(1 << 2)        /* C  */
    bic     r1, r1, #(1 << 12)       /* I  */
    mcr     p15, 0, r1, c1, c0, 0
    dsb
    isb
#endif

    /* Switch to the high virtual address space. */
    ldr     pc, =_high_start

    .ltorg

    .section .text
    .global _high_start
_high_start:
    /* Debug: send 'B' through the high virtual UART. */
#ifdef CONFIG_NOMMU
    ldr     r10, =0x09000000
#else
    ldr     r10, =0x89000000
#endif
1:
    ldr     r11, [r10, #0x18]
    tst     r11, #(1 << 5)
    bne     1b
    mov     r11, #'B'
    str     r11, [r10]

    /* Now running at high virtual addresses.  Use the linked BSS/stack. */
    ldr     sp, =_stack_end

    /* Set the exception-mode stack pointers to the same boot stack so that
     * IRQ/abort handlers have a valid stack before the scheduler is ready. */
    mrs     r4, cpsr
    cps     #0x12                    /* IRQ mode */
    ldr     sp, =_stack_end
    cps     #0x17                    /* ABT mode */
    ldr     sp, =_stack_end
    cps     #0x1B                    /* UND mode */
    ldr     sp, =_stack_end
    cps     #0x11                    /* FIQ mode */
    ldr     sp, =_stack_end
    cps     #0x13                    /* SVC mode */
    msr     cpsr_cxsf, r4

    /* Zero BSS. */
    ldr     r0, =_bss_start
    ldr     r1, =_bss_end
    mov     r2, #0
2:
    cmp     r0, r1
    bhs     3f
    str     r2, [r0], #4
    b       2b
3:
    /* Install the vector table. */
    ldr     r0, =arm32_vector_table
    mcr     p15, 0, r0, c12, c0, 0
    isb

    /* Now that we run from high virtual addresses, enable caches and
     * branch prediction. */
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #(1 << 2)        /* C  */
    orr     r0, r0, #(1 << 12)       /* I  */
    orr     r0, r0, #(1 << 11)       /* Z  */
    mcr     p15, 0, r0, c1, c0, 0
    isb

    bl      kernel_main

4:
    wfi
    b       4b

    .section .text.entry
    .align 14
    .global boot_pgdir
boot_pgdir:
    .space 16384

    .align 12
    .global boot_l2_low
boot_l2_low:
    .space 1024

    .align 12
_early_stack_bottom:
    .space 4096
_early_stack_end:

    .section .bss.stack
    .align 12
_stack_bottom:
    .space 16384
    .global _stack_end
_stack_end:

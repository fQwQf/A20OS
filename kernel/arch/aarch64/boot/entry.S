/*
 * A20OS kernel entry point for AArch64 (QEMU virt, EL1).
 *
 * QEMU loads the ELF segments at their LMA (physical address) and starts the
 * CPU in EL1 with the MMU off.  The kernel itself is linked at
 * VA = PAGE_OFFSET + PA.
 */

    .section .text.entry
    .global _start
    .extern early_uart_init
_start:
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    /* The UEFI loader passes the ACPI RSDP physical address in x0. */
    mov     x4, x0
#endif
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
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    adrp    x3, aarch64_boot_acpi_rsdp
    add     x3, x3, :lo12:aarch64_boot_acpi_rsdp
    str     x4, [x3]
#endif
    /* Enable FP/SIMD access at EL1/EL0. */
    mrs     x2, cpacr_el1
    orr     x2, x2, #(3 << 20)
    msr     cpacr_el1, x2
    isb

    /*
     * Before translation is enabled, linker-generated ADRP values name the
     * linked virtual address.  Convert those addresses back to the physical
     * load address used by QEMU.
     */
    adrp    x0, boot_pgdir
    add     x0, x0, :lo12:boot_pgdir
    adrp    x1, boot_l1
    add     x1, x1, :lo12:boot_l1
    adrp    x4, boot_l2
    add     x4, x4, :lo12:boot_l2
    orr     x2, x1, #0x3
    str     x2, [x0, #(0 * 8)]
    str     x2, [x0, #(1 * 8)]

#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    /*
     * VirtualBox ARM places RAM at 0x08000000 and its platform MMIO near
     * 0xfc000000.  The shared L1 table supplies both the physical bootstrap
     * map and the PAGE_OFFSET alias used after the high-half branch.
     */
    ldr     x2, =0x0040000000000705  /* PA 0..1 GiB, normal, UXN */
    str     x2, [x1, #(0 * 8)]
    ldr     x2, =0x00600000c0000401  /* PA 3..4 GiB, device, PXN|UXN */
    str     x2, [x1, #(3 * 8)]
    /*
     * PA 1..2 GiB is mixed on VBox: UEFI ACPI memory lives below
     * 0x48000000, while VMSVGA VRAM starts at 0x48000000 and PCI BARs are
     * placed around 0x50000000.  A 1-GiB Normal-memory block caches MMIO and
     * produces corrupted scanout.  Split it into 2-MiB blocks instead.
     */
    orr     x2, x4, #0x3
    str     x2, [x1, #(1 * 8)]
    mov     x5, xzr
    ldr     x6, =0x0040000040000705  /* normal, UXN */
    ldr     x7, =0x0060000040000401  /* device, PXN|UXN */
.Lvbox_l2_loop:
    cmp     x5, #64                  /* 0x40000000..0x47ffffff: ACPI RAM */
    csel    x2, x6, x7, lo
    str     x2, [x4, x5, lsl #3]
    add     x5, x5, #1
    add     x6, x6, #(1 << 21)
    add     x7, x7, #(1 << 21)
    cmp     x5, #512
    b.lo    .Lvbox_l2_loop
    ldr     x2, =0x0060000080000401  /* PA 2..3 GiB, device, PXN|UXN */
    str     x2, [x1, #(2 * 8)]
#else
    ldr     x2, =0x0060000000000401  /* L1 block: PA 0x0, device, PXN|UXN */
    str     x2, [x1, #(0 * 8)]
#endif
#ifdef CONFIG_NOMMU
    /*
     * Split DRAM into 2 MiB blocks.  Kernel text remains privileged while the
     * allocator-backed portion of RAM is accessible to EL0.  A single 1 GiB
     * AP=01 block makes writable kernel text executable from EL0 and also
     * trips architectural W^X controls on some CPUs.
     */
    orr     x2, x4, #0x3
    str     x2, [x1, #(1 * 8)]

    mov     x5, xzr
    ldr     x6, =0x0000000040000701  /* privileged normal-memory block */
    ldr     x7, =0x0000000040000741  /* EL0 RWX normal-memory block */
    /*
     * Keep every 2 MiB block occupied by the linked kernel privileged.  Derive
     * the boundary from _end so kernel growth cannot silently expose kernel
     * data to EL0.
     */
    adrp    x8, _end
    add     x8, x8, :lo12:_end
    ldr     x9, =0x40000000
    sub     x8, x8, x9
    ldr     x9, =0x1fffff
    add     x8, x8, x9
    lsr     x8, x8, #21
.Lnommu_l2_loop:
    cmp     x5, x8
    csel    x2, x6, x7, lo
    str     x2, [x4, x5, lsl #3]
    add     x5, x5, #1
    add     x6, x6, #(1 << 21)
    add     x7, x7, #(1 << 21)
    cmp     x5, #256                 /* 512 MiB of configured DRAM */
    b.lo    .Lnommu_l2_loop
#elif defined(CONFIG_BOARD_VIRTUALBOX_AARCH64)
    /* RAM and the mixed ACPI/MMIO window are already covered above. */
#else
    ldr     x2, =0x0040000040000705  /* L1 block: PA 0x40000000, normal, UXN */
    str     x2, [x1, #(1 * 8)]
#endif

    /* MAIR: Attr0 = device nGnRnE, Attr1 = normal WBWA cacheable. */
    ldr     x2, =0x000000000000FF00
    msr     mair_el1, x2

    /* TCR_EL1: 48-bit VA, 4KB granule, inner-shareable WBWA, TTBR1 disabled. */
    ldr     x2, =0x0000000280003510
    msr     tcr_el1, x2

    msr     ttbr0_el1, x0
    dsb     sy
    isb

    mrs     x2, sctlr_el1
    /* Do not inherit firmware byte order or execute-never policy.  A20OS's
     * initial direct map deliberately contains writable executable kernel
     * blocks, so WXN must remain clear until the kernel map is split. */
    bic     x2, x2, #(1 << 25)  /* EE: little-endian EL1 */
    bic     x2, x2, #(1 << 24)  /* E0E: little-endian EL0 */
    bic     x2, x2, #(1 << 19)  /* WXN */
#ifdef CONFIG_NOMMU
    bic     x2, x2, #(1 << 3)   /* SA: kernel stack alignment check */
    bic     x2, x2, #(1 << 4)   /* SA0: EL0 stack alignment check */
    bic     x2, x2, #(1 << 5)   /* CP15BEN: avoid legacy EL0 controls */
    bic     x2, x2, #(1 << 22)  /* UCI: trap EL0 cache maintenance */
    bic     x2, x2, #(1 << 23)  /* nTWE */
#endif
    orr     x2, x2, #(1 << 0)   /* M */
    orr     x2, x2, #(1 << 2)   /* C */
    orr     x2, x2, #(1 << 12)  /* I */
    msr     sctlr_el1, x2
    isb
    b       .Ltranslation_live

    /*
     * Keep the post-enable instruction in the literal-pool-free entry page.
     * A branch forces the pipeline to refetch under the new translation
     * regime before any address-generating instruction runs.
     */
.Ltranslation_live:

#ifndef CONFIG_NOMMU
    adrp    x0, .Lhh_entry
    add     x0, x0, :lo12:.Lhh_entry
    movz    x1, #0x0080, lsl #32
    add     x0, x0, x1
    br      x0
#else
    /*
     * NOMMU still uses a single identity map.  AArch64 treats accesses with
     * translation disabled as Device memory, where libc's exclusive atomics
     * (LDAXR/STLXR) fault.  The identity map provides Normal memory
     * attributes without giving processes distinct virtual address spaces.
     */
    b       .Lhh_entry
#endif

.Lhh_entry:
    adrp    x0, _stack_end
    add     x0, x0, :lo12:_stack_end
    mov     sp, x0

    bl      early_uart_init

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

    .align 12
boot_l2:
    .space 4096

#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    .align 3
    .global aarch64_boot_acpi_rsdp
aarch64_boot_acpi_rsdp:
    .quad 0
#endif

    .section .bss.stack
    .align 12
_stack_bottom:
    .space 4096 * 16
    .global _stack_end
_stack_end:

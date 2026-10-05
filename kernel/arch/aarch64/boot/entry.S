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
    mov     x5, x0
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    /* The UEFI loader passes the ACPI RSDP physical address in x0. */
    mov     x4, x0
    /* ...and the firmware's LoadOptions cmdline in x2, or 0 if it gave none. */
    mov     x6, x2
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
    adrp    x3, aarch64_boot_dtb
    add     x3, x3, :lo12:aarch64_boot_dtb
    str     x5, [x3]
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    adrp    x3, aarch64_boot_acpi_rsdp
    add     x3, x3, :lo12:aarch64_boot_acpi_rsdp
    str     x4, [x3]

    adrp    x3, aarch64_boot_cmdline
    add     x3, x3, :lo12:aarch64_boot_cmdline
    str     x6, [x3]
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
     * One identity map over all of DRAM, in 2 MiB blocks, privileged (AP=00)
     * and Normal memory (MAIR AttrIndx 1) -- the same attributes the MMU
     * build's DRAM block uses.
     *
     * AttrIndx 1 is the point of running an identity map with the MMU on
     * under NOMMU: with translation disabled AArch64 treats every access as
     * Device, where libc's exclusive atomics (LDAXR/STLXR) fault.  These
     * descriptors used to encode AttrIndx 0 (Device-nGnRnE) while calling
     * themselves normal-memory, so they never delivered the attributes the
     * map exists to provide.
     *
     * The blocks deliberately do NOT grant EL0 access.  AArch64 makes a
     * stage-1 page that is writable by EL0 execute-never for every higher
     * Exception level -- a privileged fetch from an EL0-writable page raises
     * Execute Never -- and that rule holds regardless of SCTLR_EL1.WXN.
     * An AP=01 map is therefore executable *by EL0* but not by the kernel.
     * This used to split DRAM at _end, keeping the kernel image privileged
     * and marking the allocator-backed remainder AP=01; the kernel escaped
     * the rule because it lives only _end bytes into DRAM, but the first
     * thing the kernel executes out of the frame pool did not, and the
     * first drvmod entry point took an instruction abort:
     *
     *   ESR_EL1=0x8600000e  EC=0x21 (IABT from current EL)  ISS=0xe (XN)
     *   L2[510]=0x7fc00741 -> stval=0x7fd9e134  (mapping present, no PXN)
     *
     * With a flat map there is no AP that serves both: kernel code allocated
     * from the pool and EL0 user memory want opposite permissions, and
     * there is no second range giving EL0 a different view of the same page.
     * Privileged wins here because a NOMMU instance with no root filesystem
     * has no EL0 process to run, and under this build it could not get one:
     * virtio-blk is itself a drvmod, so leaving the pool EL0-writable is
     * what makes the disk unreachable in the first place.  The MMU builds
     * keep the real high-half alias and are unaffected.
     */
    orr     x2, x4, #0x3
    str     x2, [x1, #(1 * 8)]

    ldr     x6, =0x0000000040000705  /* AP=00, AttrIndx 1, AF, inner-shareable */
    /*
     * The map has to end where the frame allocator's window ends.  This used
     * to stop at a hardcoded 256 blocks (512 MiB) while PHYS_MEMORY_END is
     * 1 GiB above PHYS_MEMORY_BASE, so pfa_init handed out frames in the top
     * half of DRAM that no L2 block covers -- and the first kmalloc that
     * landed there took a store page fault writing its own header.  Derive
     * the bound from the same window the MMU build covers with its single
     * 1 GiB L1 block, so the two builds cannot drift apart again.
     */
    ldr     x9, =0x40000000
    ldr     x10, =0x80000000
    sub     x10, x10, x9
    lsr     x10, x10, #21
    mov     x5, xzr
.Lnommu_l2_loop:
    str     x6, [x4, x5, lsl #3]
    add     x5, x5, #1
    add     x6, x6, #(1 << 21)
    cmp     x5, x10
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
     * blocks, so WXN must remain clear until arch_kernel_wx_finalize()
     * (arch/aarch64/mm/kwx.c) splits the map and sets it from C. */
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

    .global aarch64_secondary_start
aarch64_secondary_start:
    /* PSCI enters with the MMU off and the logical CPU number in x0. */
    msr     daifset, #2
    mov     x19, x0
    adrp    x1, _stack_end
    add     x1, x1, :lo12:_stack_end
    mov     x2, #(4096 * 16)
    mul     x2, x19, x2
    sub     sp, x1, x2

    mrs     x2, cpacr_el1
    orr     x2, x2, #(3 << 20)
    msr     cpacr_el1, x2
    isb

    adrp    x0, boot_pgdir
    add     x0, x0, :lo12:boot_pgdir
    ldr     x2, =0x000000000000FF00
    msr     mair_el1, x2
    ldr     x2, =0x0000000280003510
    msr     tcr_el1, x2
    msr     ttbr0_el1, x0
    dsb     sy
    isb
    mrs     x2, sctlr_el1
    bic     x2, x2, #(1 << 25)
    bic     x2, x2, #(1 << 24)
#if !defined(CONFIG_NOMMU) && !defined(CONFIG_BOARD_VIRTUALBOX_AARCH64)
    /* Secondaries enable the MMU after arch_kernel_wx_finalize() has split
     * the boot map into W^X-clean sections, so they can adopt WXN from the
     * start (the primary sets it in C once the split is done). */
    orr     x2, x2, #(1 << 19)
#else
    bic     x2, x2, #(1 << 19)
#endif
    orr     x2, x2, #(1 << 0)
    orr     x2, x2, #(1 << 2)
    orr     x2, x2, #(1 << 12)
    msr     sctlr_el1, x2
    isb
    b       .Lsecondary_translation_live

.Lsecondary_translation_live:
#ifndef CONFIG_NOMMU
    adrp    x0, .Lsecondary_hh_entry
    add     x0, x0, :lo12:.Lsecondary_hh_entry
    movz    x1, #0x0080, lsl #32
    add     x0, x0, x1
    br      x0
#else
    b       .Lsecondary_hh_entry
#endif

.Lsecondary_hh_entry:
    adrp    x1, _stack_end
    add     x1, x1, :lo12:_stack_end
    mov     x2, #(4096 * 16)
    mul     x2, x19, x2
    sub     sp, x1, x2
    adrp    x1, aarch64_vector_table
    add     x1, x1, :lo12:aarch64_vector_table
    msr     vbar_el1, x1
    isb
    mov     x0, x19
    bl      aarch64_secondary_entry
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

    .align 3
    .global aarch64_boot_dtb
aarch64_boot_dtb:
    .quad 0

#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    .align 3
    .global aarch64_boot_acpi_rsdp
aarch64_boot_acpi_rsdp:
    .quad 0

    .align 3
    .global aarch64_boot_cmdline
aarch64_boot_cmdline:
    .quad 0
#endif

    .section .bss.stack
    .align 12
_stack_bottom:
    .space 4096 * 16 * CONFIG_NR_CPUS
    .global _stack_end
_stack_end:

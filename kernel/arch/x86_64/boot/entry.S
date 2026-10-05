    .set PAGE_OFFSET, 0xFFFF800000000000
    .set PHYS_BASE,   0x00200000
    /* With VIRT_BASE = PAGE_OFFSET + PHYS_BASE, physical addr = symbol - PAGE_OFFSET */

    /* Multiboot v1 header with load addresses */
    .section .multiboot, "a"
    .align 4
multiboot_header:
    .long 0x1BADB002
    .long 0x00010000           /* MULTIBOOT_HEADER_HAS_ADDR */
    .long -(0x1BADB002 + 0x00010000)
    .long PHYS_BASE            /* header_addr */
    .long PHYS_BASE            /* load_addr */
    .long 0                    /* load_end_addr (0 = entire file) */
    .long 0                    /* bss_end_addr */
    .long (PHYS_BASE + 0x20)   /* entry_addr (_start is 0x20 bytes after header) */

    .section .text.entry, "ax"
    .code32
    .globl _start
_start:
    cli
    cld

    /* Multiboot leaves a magic value in EAX and a pointer to its info
     * structure in EBX.  Save both into .data before the BSS clear below
     * zeroes EAX; the info pointer is what lets C size RAM from the real
     * memory map instead of a hardcoded 1 GiB. */
    movl  %eax, (g_mb_magic - PAGE_OFFSET)
    movl  %ebx, (g_mb_info - PAGE_OFFSET)

    /* Set up stack */
    movl  $(_stack_end - PAGE_OFFSET), %esp

    /* Clear BSS */
    movl  $(_bss_start - PAGE_OFFSET), %edi
    movl  $(_bss_end - PAGE_OFFSET), %ecx
    subl  %edi, %ecx
    xorl  %eax, %eax
    rep   stosb

    /* Set up page tables */
    movl  $(boot_pgdir - PAGE_OFFSET), %edi
    xorl  %eax, %eax
    movl  $(4 * 1024), %ecx
    rep   stosl

    /* PML4[0] -> PDPT_id, PML4[256] -> PDPT_hh */
    movl  $(boot_pgdir - PAGE_OFFSET), %edi
    movl  $(boot_pdpt_id - PAGE_OFFSET + 0x003), %eax
    movl  %eax, (%edi)
    movl  $(boot_pdpt_hh - PAGE_OFFSET + 0x003), %eax
    movl  %eax, 2048(%edi)

    /* PDPT_id[0] -> PD_id */
    movl  $(boot_pdpt_id - PAGE_OFFSET), %edi
    movl  $(boot_pd_id - PAGE_OFFSET + 0x003), %eax
    movl  %eax, (%edi)

    /* Identity map: PD_id[0..511] = 0-1GB (2MB pages) */
    movl  $(boot_pd_id - PAGE_OFFSET), %edi
    movl  $0x83, %eax
    movl  $512, %ecx
1:
    movl  %eax, (%edi)
    addl  $8, %edi
    addl  $0x200000, %eax
    loop  1b

    /*
     * Higher-half map using 1GB pages.
     * PDPT_hh[0..3] map 0xFFFF800000000000 -> physical 0-4GB.
     *
     * 0-2GB: RAM — normal WB caching (flags 0x83).
     * 2-4GB: MMIO (PCI ECAM, LAPIC, IOAPIC, PCI BARs) — PCD=1 (bit 3)
     *        forces Uncacheable access so MMIO reads return live values
     *        instead of stale cache lines.  On QEMU 2G RAM this entire
     *        range is device memory so blanket PCD is safe.
     */
    movl  $(boot_pdpt_hh - PAGE_OFFSET), %edi
    movl  $0x00000083, %eax    /* 1GB page: physical 0-1GB (RAM, WB) */
    movl  %eax, (%edi)
    movl  $0x40000083, %eax    /* 1GB page: physical 1-2GB (RAM, WB) */
    movl  %eax, 8(%edi)
    movl  $0x8000008B, %eax    /* 1GB page: physical 2-3GB (MMIO, UC) */
    movl  %eax, 16(%edi)
    movl  $0xC000008B, %eax    /* 1GB page: physical 3-4GB (MMIO, UC) */
    movl  %eax, 24(%edi)

    /* Enable PAE and 2 MiB pages used by the temporary identity map. */
    movl  %cr4, %eax
    orl   $0x30, %eax
    movl  %eax, %cr4

    /* Load PML4 */
    movl  $(boot_pgdir - PAGE_OFFSET), %eax
    movl  %eax, %cr3

    /* Enable long mode */
    movl  $0xC0000080, %ecx
    rdmsr
    orl   $0x100, %eax
    wrmsr

    /* Load GDT */
    lgdtl (gdt_desc - PAGE_OFFSET)

    /* Enable paging */
    movl  %cr0, %eax
    orl   $0x80000000, %eax
    movl  %eax, %cr0

    /*
     * Reload CS with a 64-bit code descriptor.  The old iretq sequence built
     * its return frame while still executing in 32-bit mode: pushf pushed a
     * 32-bit value but iretq consumed 64-bit slots, so it read a corrupt RIP
     * and reset before the kernel could print anything.  A 16:32 far jump is
     * the architectural long-mode transition and needs no synthetic frame.
     */
    ljmp    $0x20, $(_start64 - PAGE_OFFSET)

    .align 8
gdt:
    .quad 0x0000000000000000      /* 0x00: null */
    .quad 0x0000000000000000      /* 0x08: null */
    .quad 0x0000000000000000      /* 0x10: null */
    .quad 0x00CF92000000FFFF      /* 0x18: data, limit=4GB, G=1 */
    .quad 0x00AF98000000FFFF      /* 0x20: 64-bit code, limit=4GB, G=1 */
gdt_end:

gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt - PAGE_OFFSET

    /* ==================================================================
     * UEFI entry.  Entered directly from kernel/boot/uefi/x86_64_loader.c
     * in 64-bit long mode with the firmware's identity map still active.
     *
     * GRUB cannot be used under UEFI because GRUB 2.12 does not fill the
     * multiboot ACPI tags, so the kernel never learns the RSDP address and
     * ends up with no MCFG, no PCI and no root filesystem.  The loader reads
     * the RSDP out of the firmware configuration table instead and passes it
     * here.
     *
     * Doing the handover in long mode rather than dropping to 32-bit first is
     * deliberate.  A loader that returns to 32-bit protected mode has to build
     * a GDT, load it, reload CS and clear EFER.LME/CR4.PAE/CR0.PG in an
     * order where any mistake is a #GP with no console left to report it.  The
     * firmware already leaves us in the exact mode the page tables below want,
     * so this path does none of that.
     *
     * Register contract, set by the loader:
     *   rdi = physical address of a multiboot v1 information block it built
     *   rsi = physical address of the ACPI RSDP
     * Both are physical; the firmware map is an identity map, so the same
     * direct addressing the multiboot path uses is valid here.
     * ================================================================== */
    .section .uefi64, "ax"
    .code64
    .globl _start_uefi
_start_uefi:
    cli
    cld

    /* Adopt the loader's multiboot block.  Reusing the v1 layout means
     * x86_ram_detect() and multiboot_cmdline() need no UEFI-specific path,
     * and it is why this entry can stay so small. */
    /* These are physical addresses, not the symbols' VMAs.  At this point the
     * only map is the firmware's identity map, so a write through the
     * higher-half alias is a #PF (error 2) with nothing to report it on. */
    movl   $0x2BADB002, %eax
    movabsq $g_mb_magic - PAGE_OFFSET, %rcx
    movl   %eax, (%rcx)
    movabsq $g_mb_info - PAGE_OFFSET, %rcx
    movl   %edi, (%rcx)                /* g_mb_info is uint32_t, like the
                                         multiboot path's EBX */
    movabsq $x86_boot_acpi_rsdp - PAGE_OFFSET, %rcx
    movq   %rsi, (%rcx)

    /* The stack has to be a physical address too: at this point only the
     * firmware's identity map exists, so the higher-half alias is not
     * translated yet. */
    movabsq $(_stack_end - PAGE_OFFSET), %rsp

    /* Clear BSS */
    movabsq $(_bss_start - PAGE_OFFSET), %rdi
    movabsq $(_bss_end - PAGE_OFFSET), %rcx
    subq  %rdi, %rcx
    xorl  %eax, %eax
    rep stosb

    /* Build the boot page tables: PML4 -> PDPT_id/PDPT_hh -> PD_id, an
     * identity map of the first gigabyte in 2 MiB pages, and a higher-half
     * window over 0-4 GiB in 1 GiB pages.  2-4 GiB is marked uncacheable
     * because that is where PCI ECAM and the APICs live. */
    movabsq $(boot_pgdir - PAGE_OFFSET), %rdi
    xorl  %eax, %eax
    movl   $4096, %ecx
    rep stosl

    movabsq $(boot_pgdir - PAGE_OFFSET), %rdi
    movabsq $(boot_pdpt_id - PAGE_OFFSET), %rax
    orq    $0x003, %rax
    movq   %rax, (%rdi)
    movabsq $(boot_pdpt_hh - PAGE_OFFSET), %rax
    orq    $0x003, %rax
    movq   %rax, 2048(%rdi)

    movabsq $(boot_pdpt_id - PAGE_OFFSET), %rdi
    movabsq $(boot_pd_id - PAGE_OFFSET), %rax
    orq    $0x003, %rax
    movq   %rax, (%rdi)

    movabsq $(boot_pd_id - PAGE_OFFSET), %rdi
    movl   $0x83, %eax
    movl   $512, %ecx
1:
    movq   %rax, (%rdi)
    addq   $8, %rdi
    addl   $0x200000, %eax
    loop   1b

    movabsq $(boot_pdpt_hh - PAGE_OFFSET), %rdi
    movl   $0x00000083, %eax
    movq   %rax, (%rdi)
    movl   $0x40000083, %eax
    movq   %rax, 8(%rdi)
    movl   $0x8000008B, %eax
    movq   %rax, 16(%rdi)
    movl   $0xC000008B, %eax
    movq   %rax, 24(%rdi)

    /* PAE and the 2 MiB pages the identity map uses.  LME and PG are already
     * set: we were started in long mode by the firmware.  These are 64-bit
     * forms because that is the only encoding .code64 accepts for CR access. */
    movq   %cr4, %rax
    orq    $0x30, %rax
    movq   %rax, %cr4

    movabsq $(boot_pgdir - PAGE_OFFSET), %rax
    movq   %rax, %cr3

    /* Reload CS from our own GDT, then continue in the higher half.
     *
     * gdt_desc cannot be reused here: in 64-bit mode a bare LGDT reads a
     * 10-byte pointer, and gdt_desc is the 6-byte form the 32-bit path uses
     * (16-bit limit plus a 32-bit base).  Reading 10 bytes from it would take
     * the high half of the base from whatever follows, so the far jump would
     * load CS from an address nothing is mapped at. */
    movabsq $gdt_desc64 - PAGE_OFFSET, %rax
    lgdt   (%rax)
    /* A far return rather than a far jump with an immediate target: GAS will
     * not accept ljmpq against a label in .code64.  The frame matches here --
     * both halves are 64-bit because we are already in long mode.  (The 32-bit
     * path above cannot use this, which is why it uses an architectural far
     * jump instead.) */
    movabsq $_start64, %rax
    pushq   $0x20
    pushq   %rax
    lretq

    .align 8
gdt_desc64:
    .word  gdt_end - gdt - 1
    .quad  gdt                        /* 64-bit base: this path is already high */

    .section .data
    .align 8
    .globl x86_boot_acpi_rsdp
x86_boot_acpi_rsdp:
    .quad 0

    .section .text.entry, "ax"
    .code32

    .code64
_start64:
    /* Set up data segments */
    movw  $0x18, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs
    movw  %ax, %ss

    /* Set up higher-half stack */
    movabsq $_stack_end, %rsp

    /* Call kernel_main */
    movabsq $kernel_main, %rax
    call  *%rax

    cli
1:  hlt
    jmp   1b

    .section .bss
    .align 4096
    .globl boot_pgdir
boot_pgdir:
    .space 4096
    .align 4096
boot_pdpt_id:
    .space 4096
    .align 4096
    .globl boot_pdpt_hh
boot_pdpt_hh:
    .space 4096
    .align 4096
boot_pd_id:
    .space 4096

/* BSP boot stack, 64 KiB -- the same size the other arches reserve (see
 * arch/aarch64/boot/entry.S).  .bss.stack is collected first by
 * boot/ldscript.ld, so this range sits at the bottom of .bss and nothing else
 * can be placed inside it; it also stays below _bss_end, which is where the
 * physical frame allocator's reserved region begins. */
    .section .bss.stack
    .align 4096
    .globl _stack_bottom
_stack_bottom:
    .space 4096 * 16
    .globl _stack_end
_stack_end:

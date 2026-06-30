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
     * This covers the kernel (PHYS_BASE), LAPIC, IOAPIC, and PCI ECAM.
     */
    movl  $(boot_pdpt_hh - PAGE_OFFSET), %edi
    movl  $0x00000083, %eax    /* 1GB page: physical 0-1GB */
    movl  %eax, (%edi)
    movl  $0x40000083, %eax    /* 1GB page: physical 1-2GB */
    movl  %eax, 8(%edi)
    movl  $0x80000083, %eax    /* 1GB page: physical 2-3GB */
    movl  %eax, 16(%edi)
    movl  $0xC0000083, %eax    /* 1GB page: physical 3-4GB */
    movl  %eax, 24(%edi)

    /* Enable PAE */
    movl  %cr4, %eax
    orl   $0x20, %eax
    movl  %eax, %cr4

    /* Load PML4 */
    movl  $(boot_pgdir - PAGE_OFFSET), %eax
    movl  %eax, %cr3

    /* Enable long mode, NX, and SCE */
    movl  $0xC0000080, %ecx
    rdmsr
    orl   $0x901, %eax
    wrmsr

    /* Load GDT */
    lgdtl (gdt_desc - PAGE_OFFSET)

    /* Enable paging */
    movl  %cr0, %eax
    orl   $0x80000000, %eax
    movl  %eax, %cr0

    /* Jump to 64-bit code using iretq (REX.W + iret) */
    .byte 0x48, 0x9C                    /* pushfq (64-bit) */
    .byte 0x48, 0x68, 0x20, 0x00, 0x00, 0x00   /* pushq $0x20 */
    .byte 0x48, 0x68
    .long _start64 - PAGE_OFFSET                /* pushq $RIP (physical addr) */
    .byte 0x48, 0xCF                    /* iretq */

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
boot_pdpt_hh:
    .space 4096
    .align 4096
boot_pd_id:
    .space 4096

    .align 4096
_stack_bottom:
    .space 4096 * 16
    .globl _stack_end
_stack_end:

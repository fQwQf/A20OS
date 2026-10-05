OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PROVIDE(PHYS_BASE = 0x00200000);
PROVIDE(VIRT_BASE = 0xFFFF800000200000);
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

SECTIONS
{
    . = VIRT_BASE;
    __text_start = .;

    .text : AT(ADDR(.text) - LOAD_DELTA) {
        *(.multiboot)
        *(.text.entry)
        /* Page-align the UEFI entry so its physical address is a constant the
         * loader can name without a symbol lookup: PHYS_BASE + 0x1000.  It has
         * to come after .text.entry, because the multiboot header hardcodes
         * _start at PHYS_BASE + 0x20 and GRUB still boots this path. */
        . = ALIGN(4096);
        __uefi_entry_phys = . - LOAD_DELTA;   /* the counter, not ADDR(.text) */
        *(.uefi64)
        *(.text .text.*)
    }

    . = ALIGN(4096);
    __rodata_start = .;
    .rodata : AT(ADDR(.rodata) - LOAD_DELTA) {
        KEEP(*(.rodata.ap_trampoline))
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    __data_start = .;
    .data : AT(ADDR(.data) - LOAD_DELTA) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    . = ALIGN(8);
    .driver_init : AT(ADDR(.driver_init) - LOAD_DELTA) {
        __driver_init_start = .;
        KEEP(*(.driver_init))
        __driver_init_end = .;
    }

    .board_init : AT(ADDR(.board_init) - LOAD_DELTA) {
        __board_init_start = .;
        KEEP(*(.board_init))
        __board_init_end = .;
    }

    .bss (NOLOAD) : AT(ADDR(.bss) - LOAD_DELTA) {
        _bss_start = .;
        /* The BSP boot stack goes first, so it sits at the bottom of .bss and
         * no other kernel object can be placed inside its address range.  It
         * has to live *inside* .bss: everything from _bss_end upwards belongs
         * to the physical frame allocator (mm_init passes va_to_pa(_bss_end)
         * to pfa_init, which zeroes its frame-metadata bitmap starting at
         * ROUND_UP(kernel_end, 64)), so a stack reserved above _bss_end is
         * memset out from under itself the moment the allocator comes up.
         *
         * This section used to be shadowed from here.  The script assigned
         * `_stack_end = .` a few bytes past _bss_end, which beat the identical
         * label in entry.S, so the BSP ran on a stack that began inside .bss
         * and grew down through all of it.  That was invisible until lwIP's
         * 512 KiB ram_heap landed last in .bss (MEMP_MEM_MALLOC=1,
         * net/lwip_port/lwipopts.h): mem_init()'s sentinel `struct mem` landed
         * on the live stack of net_init(), and the next printf() returned into
         * the middle of kernel_main's `call *%rax`, faulting at
         * kernel_main+0x1f1.  A stack 64 KiB *below* .bss's top is what every
         * other arch already does (see arch/aarch64/boot/ldscript.ld). */
        KEEP(*(.bss.stack))
        . = ALIGN(4096);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        *(.lbss .lbss.*)
        _bss_end = .;
    }

    /* Both invariants of the layout above, stated so a later edit that breaks
     * either fails the link instead of booting into silent corruption. */
    ASSERT(_stack_bottom == _bss_start,
           "x86_64: boot stack must be the first thing in .bss")
    ASSERT(_stack_end <= _bss_end,
           "x86_64: boot stack must stay below _bss_end (pfa owns everything above it)")
    ASSERT(_stack_end - _stack_bottom >= 64 * 1024,
           "x86_64: boot stack is below the 64 KiB every other arch reserves")

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
        *(.gnu.hash)
    }
}

OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
}

SECTIONS
{
    /* DMW-mapped kernel VA; QEMU loads it at physical address 0x00200000. */
    . = 0x9000000000200000;
    .text : AT(0x00200000) {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

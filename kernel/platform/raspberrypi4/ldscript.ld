/* Reuse the architecture's section layout and W^X boundaries. */
PHYS_BASE = 0x00080000;
VIRT_BASE = 0x0000008000080000;
INCLUDE kernel/arch/aarch64/boot/ldscript.ld

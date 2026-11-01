/*
 * Embedded static-PIE image for the MCU learner demos.  The ELF itself is
 * built by tools/mcu-demo.mk; embedding it keeps flashing to one artefact
 * (kernel A) on Hi3861 and one -kernel argument on QEMU.
 */
#if defined(CONFIG_MCU_USER_DEMO) && defined(MCU_DEMO_ELF_PATH)
    .section .rodata.mcu_demo, "a", %progbits
    .balign 8
    .global mcu_demo_elf
    .global mcu_demo_elf_end
mcu_demo_elf:
    .incbin MCU_DEMO_ELF_PATH
mcu_demo_elf_end:
#endif

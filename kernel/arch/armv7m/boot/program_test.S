#if defined(CONFIG_MCU_NATIVE_PROGRAM) && defined(CONFIG_MCU_SHARED_TEST)
    .section .rodata.mcu_program, "a", %progbits
    .balign 8
    .global mcu_test_elf, mcu_test_elf_end
mcu_test_elf:
    .incbin MCU_TEST_ELF_PATH
mcu_test_elf_end:
#endif

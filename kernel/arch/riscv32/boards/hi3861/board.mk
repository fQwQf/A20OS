include tools/riscv-mcu.mk
# The kernel is executed in place from SPI flash; the RV32 entry code copies
# .data down to the SRAM window before clearing .bss.
ARCH_CFLAGS += -DCONFIG_MCU_COPY_DATA -DCONFIG_MCU_DRIVER_MODEL
# The learner demo enters U-mode from a boot task.  It stays opt-in until the
# physical part has been checked; `tools/a20 ... -- HI3861_LITEOS_DEMO=1`
# builds the same image as QEMU.
ifeq ($(HI3861_LITEOS_DEMO),1)
ARCH_CFLAGS += -DCONFIG_MCU_USER_DEMO
endif

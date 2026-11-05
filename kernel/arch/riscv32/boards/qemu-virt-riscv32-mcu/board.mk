include tools/mk/riscv-mcu.mk
QEMU_FLAGS_BASE_qemu-virt-riscv32-mcu := -machine virt -bios none -cpu lowrisc-ibex
ARCH_CFLAGS += -DCONFIG_MCU_USER_DEMO -DA20_CPU_HZ=1000000000UL

# CMSIS demo tick must match ARCH_TIMER_FREQ (1 kHz on QEMU virt).
MCU_TICK_HZ := 1000

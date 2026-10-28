include tools/riscv-mcu.mk
ARCH_KERNEL_C_SRC += kernel/drivers/char/esp_uart.c kernel/drivers/timer/esp_systimer.c
# Bin output from objcopy spans two SRAM aliases; only elf2image is flashable.

# Espressif QEMU is optional; the distro QEMU does not contain this machine.
ESP32C3_QEMU ?= build/esp32-c3/qemu/qemu/bin/qemu-system-riscv32
QEMU := $(ESP32C3_QEMU)
QEMU_FLAGS_BASE_esp32-c3 := -machine esp32c3

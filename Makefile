# A20OS Makefile

# Architecture selection
ARCH ?= riscv64
MODE ?= release

# Compiler and tools
ifeq ($(ARCH), riscv64)
    CROSS_PREFIX = riscv64-unknown-elf-
    ARCH_CFLAGS = -march=rv64gc -mabi=lp64d -mcmodel=medany
    ARCH_LDFLAGS =
    QEMU = qemu-system-riscv64
    QEMU_FLAGS = -machine virt -m 128M -nographic -smp 1 -bios default -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), loongarch64)
    CROSS_PREFIX = loongarch64-unknown-elf-
    ARCH_CFLAGS = -march=loongarch64 -mabi=lp64d
    ARCH_LDFLAGS =
    QEMU = qemu-system-loongarch64
    QEMU_FLAGS = -machine virt -m 128M -nographic -smp 1
endif

QEMU_FLAGS += -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0


# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include

# Compiler flags
CFLAGS = -Wall -Wextra -O2 -ffreestanding -nostdlib \
         -nostartfiles -fno-builtin -fno-common -std=gnu99 \
         -I$(INCLUDE_DIR) $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z)

LDFLAGS = -nostdlib -nostartfiles -T $(KERNEL_DIR)/arch/$(ARCH)/ldscript.ld $(ARCH_LDFLAGS)

# Source files
KERNEL_SRC = $(wildcard $(KERNEL_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/lib/*.c) \
             $(wildcard $(KERNEL_DIR)/mm/*.c) \
             $(wildcard $(KERNEL_DIR)/proc/*.c) \
             $(wildcard $(KERNEL_DIR)/fs/*.c) \
             $(wildcard $(KERNEL_DIR)/drv/*.c) \
             $(wildcard $(KERNEL_DIR)/syscall/*.c) \
             $(wildcard $(KERNEL_DIR)/trap/*.c) \
             $(wildcard $(KERNEL_DIR)/shell/*.c)

# Object files
KERNEL_OBJ = $(KERNEL_SRC:.c=.o)

# ASM sources
ASM_SRC = $(wildcard $(KERNEL_DIR)/arch/$(ARCH)/*.S)
ASM_OBJ = $(ASM_SRC:.S=.o)

# Kernel image
KERNEL_ELF = kernel.elf
KERNEL_BIN = kernel.bin

# Targets
.PHONY: all clean run-riscv64 run-loongarch64 user_apps fs_img

all: $(KERNEL_BIN) user_apps fs_img
	@echo "Build complete: $(KERNEL_BIN) and fs.img"

user_apps:
	$(MAKE) -C user ARCH=$(ARCH)

fs_img: user_apps
	@echo "Building FAT32 image..."
	dd if=/dev/zero of=fs.img bs=1M count=32
	mkfs.fat -F 32 fs.img
	# Need tools like mcopy from mtools to copy files without root
	mcopy -i fs.img user/build/init ::/init
	mcopy -i fs.img user/build/sh ::/sh
	mmd -i fs.img ::/bin
	mcopy -i fs.img user/build/ls ::/bin/ls
	mcopy -i fs.img user/build/cat ::/bin/cat
	mcopy -i fs.img user/build/mkdir ::/bin/mkdir
	mcopy -i fs.img user/build/rm ::/bin/rm
	mcopy -i fs.img user/build/cp ::/bin/cp
	mcopy -i fs.img user/build/ps ::/bin/ps
	@printf 'Hello from A20OS FAT32!\nThis file is on the FAT32 filesystem at /mnt/test.txt\n' | mcopy -i fs.img - ::/test.txt
	cp fs.img fs_test.img

$(KERNEL_BIN): $(KERNEL_ELF)
	$(CROSS_PREFIX)objcopy -O binary $< $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ)
	$(CROSS_PREFIX)gcc $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f $(KERNEL_OBJ) $(ASM_OBJ) $(KERNEL_ELF) $(KERNEL_BIN) fs.img
	$(MAKE) -C user clean

run-riscv64:
	$(MAKE) ARCH=riscv64 all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_BIN)

run-loongarch64:
	$(MAKE) ARCH=loongarch64 all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_BIN)

# --- Debug Targets ---

# 过滤掉 -O2，替换为 -O0 以获得更好的单步调试体验，并加上 -g
DEBUG_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O0 -g -DDEBUG

# 编译调试版内核
debug-build:
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" clean all

# RISC-V 64 调试运行 (启动 QEMU 并冻结，等待 GDB 连接)
debug-riscv64: debug-build
	$(MAKE) ARCH=riscv64 run-qemu-debug

# LoongArch 64 调试运行
debug-loongarch64: debug-build
	$(MAKE) ARCH=loongarch64 run-qemu-debug

# 实际启动 QEMU 的内部目标
# 注意：这里使用 $(KERNEL_ELF) 而不是 $(KERNEL_BIN)
run-qemu-debug:
	@echo "Waiting for GDB connection on port 1234..."
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s
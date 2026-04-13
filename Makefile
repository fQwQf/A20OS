# A20OS Makefile

# Architecture selection
ARCH ?= riscv64
MODE ?= release
BRINGUP ?= 0

# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include
ARCH_DIR = $(KERNEL_DIR)/arch/$(ARCH)

# Compiler and tools
ifeq ($(ARCH), riscv64)
    CROSS_PREFIX = riscv64-unknown-elf-
    ARCH_CFLAGS = -march=rv64gc -mabi=lp64d -mcmodel=medany
    ARCH_LDFLAGS =
    QEMU = qemu-system-riscv64
    QEMU_FLAGS = -machine virt -m 128M -nographic -smp 1 -bios default -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), loongarch64)
    CROSS_PREFIX = loongarch64-linux-gnu-
    ARCH_CFLAGS = -march=loongarch64 -mabi=lp64d
    ARCH_LDFLAGS =
    QEMU = qemu-system-loongarch64
    QEMU_FLAGS = -machine virt -m 128M -nographic -smp 1
else
$(error Unsupported ARCH='$(ARCH)')
endif

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
QEMU_FLAGS += -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += -drive file=ext4.img,if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
endif



# Compiler flags
CFLAGS = -Wall -Wextra -O2 -ffreestanding -nostdlib \
         -nostartfiles -fno-builtin -fno-common -std=gnu99 \
         -I$(INCLUDE_DIR) \
         -I$(ARCH_DIR)/include \
         $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z)

# Bringup compile mode marker for conditional code.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif

LDFLAGS = -nostdlib -nostartfiles -T $(ARCH_DIR)/boot/ldscript.ld $(ARCH_LDFLAGS)

# Source files
ARCH_SRC = $(wildcard $(ARCH_DIR)/*.c)
ARCH_SRC_NO_STUBS = $(filter-out $(ARCH_DIR)/bringup_stubs.c,$(ARCH_SRC))
ARCH_EXTRA_C = $(wildcard $(ARCH_DIR)/boot/*.c) \
               $(wildcard $(ARCH_DIR)/drv/*.c)

KERNEL_SRC_FULL = $(wildcard $(KERNEL_DIR)/*.c) \
                  $(wildcard $(KERNEL_DIR)/lib/*.c) \
                  $(wildcard $(KERNEL_DIR)/mm/*.c) \
                  $(wildcard $(KERNEL_DIR)/proc/*.c) \
                  $(wildcard $(KERNEL_DIR)/fs/*.c) \
                  $(wildcard $(KERNEL_DIR)/syscall/*.c) \
                  $(wildcard $(KERNEL_DIR)/trap/*.c) \
                  $(wildcard $(KERNEL_DIR)/shell/*.c) \
                  $(wildcard $(KERNEL_DIR)/drv/*.c) \
                  $(ARCH_SRC_NO_STUBS) \
                  $(ARCH_EXTRA_C)

KERNEL_SRC_BRINGUP = $(KERNEL_DIR)/main.c \
                     $(KERNEL_DIR)/lib/printf.c \
                     $(KERNEL_DIR)/lib/string.c \
                     $(KERNEL_DIR)/drv/uart.c \
                     $(KERNEL_DIR)/drv/clint.c \
                     $(ARCH_SRC_NO_STUBS) \
					 $(ARCH_DIR)/drv/plat_irq.c \
                     $(ARCH_DIR)/bringup_stubs.c

ifeq ($(BRINGUP),1)
KERNEL_SRC = $(KERNEL_SRC_BRINGUP)
ASM_SRC = $(ARCH_DIR)/boot/entry.S
else
KERNEL_SRC = $(KERNEL_SRC_FULL)
ASM_SRC = $(wildcard $(ARCH_DIR)/*.S) \
          $(wildcard $(ARCH_DIR)/boot/*.S)
endif

# Object files
KERNEL_OBJ = $(KERNEL_SRC:.c=.o)
ASM_OBJ = $(ASM_SRC:.S=.o)

# Kernel image
KERNEL_ELF = kernel.elf
KERNEL_BIN = kernel.bin

# Targets
.PHONY: all clean run-riscv64 run-loongarch64 user_apps fs_img kernel-only

all: $(KERNEL_BIN) user_apps fs_img ext4_img_only
	@echo "Build complete: $(KERNEL_BIN), fs.img and ext4.img"

kernel-only: $(KERNEL_BIN)
	@echo "Build complete: $(KERNEL_BIN)"

user_apps:
	$(MAKE) -C user ARCH=$(ARCH)

fs_img: user_apps
	@echo "Building FAT32 image..."
	dd if=/dev/zero of=fs.img bs=1M count=32
	mkfs.fat -F 32 fs.img
	mcopy -i fs.img user/build/init ::/init
	mcopy -i fs.img user/build/sh ::/sh
	mcopy -i fs.img user/build/ls ::/ls
	mcopy -i fs.img user/build/cat ::/cat
	mcopy -i fs.img user/build/mkdir ::/mkdir
	mcopy -i fs.img user/build/rm ::/rm
	mcopy -i fs.img user/build/cp ::/cp
	mcopy -i fs.img user/build/ps ::/ps
	mcopy -i fs.img user/build/aed ::/aed
	mcopy -i fs.img user/build/touch ::/touch
	mcopy -i fs.img user/build/poweroff ::/poweroff
	mcopy -i fs.img user/build/reboot ::/reboot
	mcopy -i fs.img user/build/pwd ::/pwd
	mcopy -i fs.img user/build/echo ::/echo
	mcopy -i fs.img user/build/env ::/env
	mcopy -i fs.img user/build/clear ::/clear
	mcopy -i fs.img user/build/help ::/help
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i fs.img - ::/test.txt
	cp fs.img fs_test.img

ext4_img_only: user_apps
	@echo "Building ext4 image..."
	@rm -rf /tmp/a20os_ext4_staging && mkdir -p /tmp/a20os_ext4_staging
	cp user/build/ls   /tmp/a20os_ext4_staging/ls
	cp user/build/cat  /tmp/a20os_ext4_staging/cat
	cp user/build/mkdir /tmp/a20os_ext4_staging/mkdir
	cp user/build/rm   /tmp/a20os_ext4_staging/rm
	cp user/build/cp   /tmp/a20os_ext4_staging/cp
	printf 'Hello from ext4!\nThis file is on the ext4 filesystem.\n' > /tmp/a20os_ext4_staging/test.txt
	dd if=/dev/zero of=ext4.img bs=1M count=32
	mkfs.ext4 -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index -d /tmp/a20os_ext4_staging ext4.img
	@rm -rf /tmp/a20os_ext4_staging

ext4_img: user_apps ext4_img_only
	cp ext4.img fs_test.img

$(KERNEL_BIN): $(KERNEL_ELF)
	$(CROSS_PREFIX)objcopy -O binary $< $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ)
	$(CROSS_PREFIX)gcc $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -f $(KERNEL_ELF) $(KERNEL_BIN) fs.img
	$(MAKE) -C user clean

run-riscv64:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=riscv64 BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=riscv64 all
endif
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_BIN)

run-loongarch64:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=loongarch64 BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=loongarch64 all
endif
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

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

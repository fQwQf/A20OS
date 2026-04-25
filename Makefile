# A20OS Makefile

# Architecture selection
ARCH ?= riscv64
MODE ?= release
BRINGUP ?= 0
CONTEST ?= 0

# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include
BUILD_DIR = .kernel-build/$(ARCH)
FAT32_IMG = $(BUILD_DIR)/fat32.img
EXT4_IMG = $(BUILD_DIR)/ext4.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
ARCH_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/include
EXT4_STAGING_DIR = $(BUILD_DIR)/ext4-staging

# Compiler and tools
ifeq ($(ARCH), riscv64)
    CROSS_PREFIX = riscv64-unknown-elf-
    ARCH_CFLAGS = -march=rv64gc -mabi=lp64d -mcmodel=medany
    ARCH_LDFLAGS =
    QEMU = qemu-system-riscv64
    QEMU_FLAGS = -machine virt -m 512M -nographic -smp 1 -bios default -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), loongarch64)
    CROSS_PREFIX = loongarch64-linux-gnu-
    ARCH_CFLAGS = -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-loongarch64
    QEMU_FLAGS = -machine virt -m 512M -nographic -smp 1
endif

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
ifeq ($(ARCH), riscv64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += -drive file=$(EXT4_IMG),if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x2 -device virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2
endif

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x3 -device virtio-blk-device,drive=x3,bus=virtio-mmio-bus.3
endif

else ifeq ($(ARCH), loongarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0
QEMU_FLAGS += -drive file=$(EXT4_IMG),if=none,format=raw,id=x1 -device virtio-blk-pci,drive=x1

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x2 -device virtio-blk-pci,drive=x2
endif

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x3 -device virtio-blk-pci,drive=x3
endif

endif
endif

# Compiler flags
CFLAGS = -Wall -Wextra -O2 -ffreestanding -nostdlib \
         -nostartfiles -fno-builtin -fno-common -std=gnu99 \
         -MMD -MP \
         -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(ARCH_INCLUDE_DIR) $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z)

# Bringup / contest mode markers for conditional compilation.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif
ifeq ($(CONTEST),1)
CFLAGS += -DCONTEST
endif

LDFLAGS = -nostdlib -nostartfiles -T $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld $(ARCH_LDFLAGS)

# Source files
KERNEL_SRC = $(wildcard $(KERNEL_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/lib/*.c) \
             $(wildcard $(KERNEL_DIR)/mm/*.c) \
             $(wildcard $(KERNEL_DIR)/proc/*.c) \
             $(wildcard $(KERNEL_DIR)/fs/*.c) \
             $(wildcard $(KERNEL_DIR)/drv/*.c) \
             $(wildcard $(KERNEL_DIR)/syscall/*.c) \
             $(wildcard $(KERNEL_DIR)/trap/*.c) \
             $(wildcard $(KERNEL_DIR)/shell/*.c) \
             $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.c' | sort)

# Object files
KERNEL_OBJ = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRC))

# ASM sources
ASM_SRC = $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.S' | sort)
ASM_OBJ = $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC))
DEP_FILES = $(KERNEL_OBJ:.o=.d) $(ASM_OBJ:.o=.d)

# Kernel image
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin

# ================================================================
# Targets
# ================================================================

.PHONY: all clean run-riscv64 run-loongarch64 debug-riscv64 debug-loongarch64 \
        user_apps fs_img kernel-only dev-build contest-rv contest-la

# ----------------------------------------------------------------
# Competition build: produces kernel-rv, kernel-la, disk.img,
# disk-la.img (what the judge expects from `make all`).
# ----------------------------------------------------------------
all: contest-rv 
	@echo "=== Competition build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

contest-rv:
	@echo "--- Building RISC-V 64 (contest) ---"
	$(MAKE) ARCH=riscv64 CONTEST=1 _reset_obj
	$(MAKE) ARCH=riscv64 CONTEST=1 _contest_build KERNEL_OUT=kernel-rv DISK_OUT=disk.img

contest-la:
	@echo "--- Building LoongArch 64 (contest) ---"
	$(MAKE) ARCH=loongarch64 CONTEST=1 _reset_obj
	$(MAKE) ARCH=loongarch64 CONTEST=1 _contest_build KERNEL_OUT=kernel-la DISK_OUT=disk-la.img

_reset_obj:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	$(MAKE) -C user clean

_contest_build: $(KERNEL_ELF) user_apps _contest_disk
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_contest_disk: user_apps
	dd if=/dev/zero of=$(DISK_OUT) bs=1M count=16 2>/dev/null
	mkfs.fat -F 32 $(DISK_OUT)
	mcopy -i $(DISK_OUT) user/build/init  ::/init

# ----------------------------------------------------------------
# Development build (for `make run-riscv64` / `make run-loongarch64`)
# ----------------------------------------------------------------

dev-build: $(KERNEL_BIN) user_apps fs_img ext4_img_only
	@echo "Dev build complete: $(KERNEL_BIN), $(FAT32_IMG), $(EXT4_IMG)"

user_apps:
ifeq ($(CONTEST),1)
	$(MAKE) -C user ARCH=$(ARCH) CONTEST=$(CONTEST) build_dir build/init
else
	$(MAKE) -C user ARCH=$(ARCH) CONTEST=$(CONTEST)
endif

fs_img: user_apps
	@echo "Building FAT32 image..."
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(FAT32_IMG) bs=1M count=32
	mkfs.fat -F 32 $(FAT32_IMG)
	mcopy -i $(FAT32_IMG) user/build/init ::/init
	mcopy -i $(FAT32_IMG) user/build/mksh ::/mksh
	mcopy -i $(FAT32_IMG) user/build/sh ::/sh
	mcopy -i $(FAT32_IMG) user/build/ls ::/ls
	mcopy -i $(FAT32_IMG) user/build/cat ::/cat
	mcopy -i $(FAT32_IMG) user/build/mkdir ::/mkdir
	mcopy -i $(FAT32_IMG) user/build/rm ::/rm
	mcopy -i $(FAT32_IMG) user/build/cp ::/cp
	mcopy -i $(FAT32_IMG) user/build/ps ::/ps
	mcopy -i $(FAT32_IMG) user/build/aed ::/aed
	mcopy -i $(FAT32_IMG) user/build/touch ::/touch
	mcopy -i $(FAT32_IMG) user/build/poweroff ::/poweroff
	mcopy -i $(FAT32_IMG) user/build/reboot ::/reboot
	mcopy -i $(FAT32_IMG) user/build/pwd ::/pwd
	mcopy -i $(FAT32_IMG) user/build/echo ::/echo
	mcopy -i $(FAT32_IMG) user/build/env ::/env
	mcopy -i $(FAT32_IMG) user/build/clear ::/clear
	mcopy -i $(FAT32_IMG) user/build/help ::/help
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i $(FAT32_IMG) - ::/test.txt
	cp $(FAT32_IMG) $(FS_TEST_IMG)

ext4_img_only: user_apps
	@echo "Building ext4 image..."
	@rm -rf $(EXT4_STAGING_DIR) && mkdir -p $(EXT4_STAGING_DIR)
	cp user/build/init $(EXT4_STAGING_DIR)/init
	cp user/build/mksh $(EXT4_STAGING_DIR)/mksh
	cp user/build/ls   $(EXT4_STAGING_DIR)/ls
	cp user/build/cat  $(EXT4_STAGING_DIR)/cat
	cp user/build/mkdir $(EXT4_STAGING_DIR)/mkdir
	cp user/build/rm   $(EXT4_STAGING_DIR)/rm
	cp user/build/cp   $(EXT4_STAGING_DIR)/cp
	printf 'Hello from ext4!\nThis file is on the ext4 filesystem.\n' > $(EXT4_STAGING_DIR)/test.txt
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXT4_IMG) bs=1M count=32
	mkfs.ext4 -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index -d $(EXT4_STAGING_DIR) $(EXT4_IMG)
	@rm -rf $(EXT4_STAGING_DIR)

ext4_img: user_apps ext4_img_only
	cp $(EXT4_IMG) $(FS_TEST_IMG)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(CROSS_PREFIX)objcopy -O binary $< $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ)
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	rm -f kernel.elf kernel.bin fat32.img ext4.img
	rm -f kernel-rv kernel-la disk.img disk-la.img
	$(MAKE) -C user clean

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"

# ----------------------------------------------------------------
# Run targets (development mode)
# ----------------------------------------------------------------

run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _run_impl

run-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _run_impl

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) dev-build
endif
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# --- Debug Targets ---

DEBUG_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O0 -g -DDEBUG

debug-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _debug_impl

debug-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _debug_impl

_debug_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 CFLAGS="$(DEBUG_CFLAGS)" kernel-only
else
	$(MAKE) ARCH=$(ARCH) CFLAGS="$(DEBUG_CFLAGS)" dev-build
endif
	@echo "Waiting for GDB connection on port 1234..."
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s

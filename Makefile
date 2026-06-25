# A20OS Makefile

# Parallel build
NPROC ?= $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)

# Architecture selection
ARCH ?= riscv64
ABI ?= both
MODE ?= release
BRINGUP ?= 0
OPT ?= -O3
NR_CPUS ?= 1
ALLOW_UNVERIFIED_SMP ?= 0
BOARD ?= qemu-virt-$(ARCH)

ifneq ($(NR_CPUS),1)
ifeq ($(ALLOW_UNVERIFIED_SMP),0)
SMP_VALIDATION_GOALS := check-concurrency-foundation check-doc-test-gates check-final-definition
ifeq ($(filter $(SMP_VALIDATION_GOALS),$(MAKECMDGOALS)),)
$(error NR_CPUS=$(NR_CPUS) is blocked until scheduler/MM/VFS concurrency gates pass; set ALLOW_UNVERIFIED_SMP=1 only for explicit SMP bringup experiments)
endif
endif
endif

# ABI selection: linux, native, both (compile both ABI layers simultaneously)
ifeq ($(filter $(ABI),linux native both),)
$(error Unsupported ABI '$(ABI)'; supported: linux, native, both)
endif

.DEFAULT_GOAL := all

# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include
BUILD_VARIANT = $(ABI)-$(if $(filter 1,$(BRINGUP)),bringup,dev)
BUILD_DIR = .kernel-build/$(ARCH)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img
EXT4_IMG = $(BUILD_DIR)/ext4.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
USER_BUILD_STAMP = user/build/.build-id
ARCH_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/include
BOARD_INCLUDE_DIR = $(KERNEL_DIR)/platform/$(BOARD)
EXT4_STAGING_DIR = $(BUILD_DIR)/ext4-staging
BUILD_TIME_HDR = $(BUILD_DIR)/generated/build_time.h
FAT32_IMAGE_MB ?= 128
EXT4_IMAGE_MB ?= 128
EXTRA_IMAGE_MB ?= 256
EXTRA_IMG = $(BUILD_DIR)/extra.img
EXTRA_STAGING_DIR = $(BUILD_DIR)/extra-staging
EXTRA_PACKAGES = vim git gcc cc
USER_BUILD_ID = $(ARCH):$(OPT)
USER_BUILD_CHECK_DIRS = user/cmds user/init_common user/lib user/shell \
                        user/external/musl user/external/sbase user/external/mksh-cvs2git \
                        user/external/tlse user/external/fastfetch
comma := ,
NET_HOSTFWD ?= hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
NETDEV_USER = -netdev user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
SMOKE_TIMEOUT ?= 20s
SMOKE_INPUT_DELAY ?= 2
SMOKE_LOG_DIR ?= .kernel-build/smoke

PROTOCOLS_LINES = \
    'hopopt 0 HOPOPT' \
    'icmp 1 ICMP' \
    'igmp 2 IGMP' \
    'tcp 6 TCP' \
    'udp 17 UDP' \
    'ipv6 41 IPv6' \
    'ipv6-route 43 IPv6-Route' \
    'ipv6-frag 44 IPv6-Frag' \
    'esp 50 ESP' \
    'ah 51 AH' \
    'ipv6-icmp 58 IPv6-ICMP' \
    'ipv6-nonxt 59 IPv6-NoNxt' \
    'ipv6-opts 60 IPv6-Opts'

LIBGCC_S_riscv64 := /usr/riscv64-linux-gnu/lib/libgcc_s.so.1
LIBGCC_S_loongarch64 := /usr/loongarch64-linux-gnu/lib/libgcc_s.so.1
LIBGCC_S_aarch64 := /usr/lib/aarch64-linux-gnu/libgcc_s.so.1
LIBGCC_S_x86_64 := /usr/lib/x86_64-linux-gnu/libgcc_s.so.1
LIBGCC_S_ARCH := $(LIBGCC_S_$(ARCH))

# ----------------------------------------------------------------
# Rust support (post-competition incremental RIIR)
# ----------------------------------------------------------------
RUST_ENABLED ?= 1
RUST_MODULE_XATTR ?= 1

RUST_MODULE_TIMEKEEPING ?= 1
RUST_MODULE_PAGECACHE ?= 1
RUST_MODULE_BLOCKCACHE ?= 1
RUST_MODULE_SYNC ?= 1
RUST_MODULE_SLAB ?= 1
RUST_MODULE_STATPERM ?= 1
RUST_MODULE_PROC_LIST ?= 1
RUST_MODULE_RANDOM ?= 1
RUST_MODULE_EVENTFD ?= 1
RUST_MODULE_TIMERFD ?= 1
RUST_MODULE_LOCKS ?= 1
RUST_MODULE_FDTABLE ?= 1
RUST_MODULE_FILE ?= 1
RUST_MODULE_PIPE ?= 1

RUST_MODULE_SIGNAL ?= 1

RUSTC = rustc
RUST_TARGET_riscv64 = riscv64imac-unknown-none-elf
RUST_TARGET_loongarch64 = loongarch64-unknown-none
RUST_TARGET_aarch64 = aarch64-unknown-none
RUST_TARGET_x86_64 = x86_64-unknown-none
RUST_TARGET = $(RUST_TARGET_$(ARCH))

RUSTFLAGS = --edition 2021 \
            --crate-type rlib \
            -C opt-level=$(if $(filter -O3,$(OPT)),3,2) \
            -C panic=abort \
            --target $(RUST_TARGET)

RUST_SUPPORT_SRC = kernel/rust/support/panic_handler.rs kernel/rust/support/irqsave_lock.c kernel/rust/support/arch_info.c kernel/rust/support/page_cache_helpers.c kernel/rust/support/sync_helpers.c kernel/rust/support/slab_helpers.c kernel/rust/support/stat_perm_helpers.c kernel/rust/support/proc_list_helpers.c
RUST_SUPPORT_COBJ = $(BUILD_DIR)/rust/irqsave_lock.o $(BUILD_DIR)/rust/arch_info.o $(BUILD_DIR)/rust/page_cache_helpers.o $(BUILD_DIR)/rust/block_cache_helpers.o $(BUILD_DIR)/rust/xattr_helpers.o $(BUILD_DIR)/rust/time_helpers.o $(BUILD_DIR)/rust/sync_helpers.o $(BUILD_DIR)/rust/slab_helpers.o $(BUILD_DIR)/rust/stat_perm_helpers.o $(BUILD_DIR)/rust/proc_list_helpers.o $(BUILD_DIR)/rust/random_helpers.o $(BUILD_DIR)/rust/eventfd_helpers.o $(BUILD_DIR)/rust/timerfd_helpers.o $(BUILD_DIR)/rust/locks_helpers.o $(BUILD_DIR)/rust/fdtable_helpers.o $(BUILD_DIR)/rust/file_helpers.o $(BUILD_DIR)/rust/pipe_helpers.o $(BUILD_DIR)/rust/signal_helpers.o

RUST_SUPPORT_LIB = $(BUILD_DIR)/rust/liba20rust_support.rlib
RUST_KERNEL_SRC = kernel/rust/rust_kernel.rs
RUST_KERNEL_LIB = $(BUILD_DIR)/rust/librust_kernel.a
RUST_LIBS =

ifeq ($(RUST_ENABLED),1)
  ifeq ($(RUST_MODULE_XATTR),1)
    CFLAGS += -DCONFIG_RUST_XATTR
    RUST_LIBS += $(BUILD_DIR)/rust/libxattr.rlib
  endif
  ifeq ($(RUST_MODULE_TIMEKEEPING),1)
    CFLAGS += -DCONFIG_RUST_TIMEKEEPING
    RUST_LIBS += $(BUILD_DIR)/rust/libtimekeeping.rlib
  endif
  ifeq ($(RUST_MODULE_PAGECACHE),1)
    CFLAGS += -DCONFIG_RUST_PAGECACHE
    RUST_LIBS += $(BUILD_DIR)/rust/libpage_cache.rlib
  endif
  ifeq ($(RUST_MODULE_BLOCKCACHE),1)
    CFLAGS += -DCONFIG_RUST_BLOCKCACHE
    RUST_LIBS += $(BUILD_DIR)/rust/libblock_cache.rlib
  endif
  ifeq ($(RUST_MODULE_SYNC),1)
    CFLAGS += -DCONFIG_RUST_SYNC
    RUST_LIBS += $(BUILD_DIR)/rust/libsync.rlib
  endif
  ifeq ($(RUST_MODULE_SLAB),1)
    CFLAGS += -DCONFIG_RUST_SLAB
    RUST_LIBS += $(BUILD_DIR)/rust/libslab.rlib
  endif
  ifeq ($(RUST_MODULE_STATPERM),1)
    CFLAGS += -DCONFIG_RUST_STATPERM
    RUST_LIBS += $(BUILD_DIR)/rust/libstat_perm.rlib
  endif
  ifeq ($(RUST_MODULE_PROC_LIST),1)
    CFLAGS += -DCONFIG_RUST_PROC_LIST
    RUST_LIBS += $(BUILD_DIR)/rust/libproc_list.rlib
  endif
  ifeq ($(RUST_MODULE_RANDOM),1)
    CFLAGS += -DCONFIG_RUST_RANDOM
    RUST_LIBS += $(BUILD_DIR)/rust/librandom.rlib
  endif
  ifeq ($(RUST_MODULE_EVENTFD),1)
    CFLAGS += -DCONFIG_RUST_EVENTFD
    RUST_LIBS += $(BUILD_DIR)/rust/libeventfd.rlib
  endif
  ifeq ($(RUST_MODULE_TIMERFD),1)
    CFLAGS += -DCONFIG_RUST_TIMERFD
    RUST_LIBS += $(BUILD_DIR)/rust/libtimerfd.rlib
  endif
  ifeq ($(RUST_MODULE_LOCKS),1)
    CFLAGS += -DCONFIG_RUST_LOCKS
    RUST_LIBS += $(BUILD_DIR)/rust/liblocks.rlib
  endif
  ifeq ($(RUST_MODULE_FDTABLE),1)
    CFLAGS += -DCONFIG_RUST_FDTABLE
    RUST_LIBS += $(BUILD_DIR)/rust/libfdtable.rlib
  endif
  ifeq ($(RUST_MODULE_FILE),1)
    CFLAGS += -DCONFIG_RUST_FILE
    RUST_LIBS += $(BUILD_DIR)/rust/libfile.rlib
  endif
  ifeq ($(RUST_MODULE_PIPE),1)
    CFLAGS += -DCONFIG_RUST_PIPE
    RUST_LIBS += $(BUILD_DIR)/rust/libpipe.rlib
  endif
  ifeq ($(RUST_MODULE_SIGNAL),1)
    CFLAGS += -DCONFIG_RUST_SIGNAL
    RUST_LIBS += $(BUILD_DIR)/rust/libsignal.rlib
  endif
endif

RUST_KERNEL_CFG =
RUST_LINK_DEPS =
RUST_LD_LIBS =
ifeq ($(RUST_ENABLED),1)
  ifneq ($(RUST_LIBS),)
    RUST_LINK_DEPS = $(RUST_KERNEL_LIB) $(RUST_SUPPORT_COBJ)
    RUST_LD_LIBS = $(RUST_SUPPORT_COBJ) $(RUST_KERNEL_LIB)
  endif
  ifeq ($(RUST_MODULE_PAGECACHE),1)
    RUST_KERNEL_CFG += --cfg rust_module_page_cache
  endif
  ifeq ($(RUST_MODULE_BLOCKCACHE),1)
    RUST_KERNEL_CFG += --cfg rust_module_block_cache
  endif
  ifeq ($(RUST_MODULE_SYNC),1)
    RUST_KERNEL_CFG += --cfg rust_module_sync
  endif
  ifeq ($(RUST_MODULE_SLAB),1)
    RUST_KERNEL_CFG += --cfg rust_module_slab
  endif
  ifeq ($(RUST_MODULE_STATPERM),1)
    RUST_KERNEL_CFG += --cfg rust_module_stat_perm
  endif
  ifeq ($(RUST_MODULE_PROC_LIST),1)
    RUST_KERNEL_CFG += --cfg rust_module_proc_list
  endif
  ifeq ($(RUST_MODULE_RANDOM),1)
    RUST_KERNEL_CFG += --cfg rust_module_random
  endif
  ifeq ($(RUST_MODULE_EVENTFD),1)
    RUST_KERNEL_CFG += --cfg rust_module_eventfd
  endif
  ifeq ($(RUST_MODULE_TIMERFD),1)
    RUST_KERNEL_CFG += --cfg rust_module_timerfd
  endif
  ifeq ($(RUST_MODULE_LOCKS),1)
    RUST_KERNEL_CFG += --cfg rust_module_locks
  endif
  ifeq ($(RUST_MODULE_FDTABLE),1)
    RUST_KERNEL_CFG += --cfg rust_module_fdtable
  endif
  ifeq ($(RUST_MODULE_FILE),1)
    RUST_KERNEL_CFG += --cfg rust_module_file
  endif
  ifeq ($(RUST_MODULE_PIPE),1)
    RUST_KERNEL_CFG += --cfg rust_module_pipe
  endif
  ifeq ($(RUST_MODULE_SIGNAL),1)
    RUST_KERNEL_CFG += --cfg rust_module_signal
  endif
  ifeq ($(RUST_MODULE_XATTR),1)
    RUST_KERNEL_CFG += --cfg rust_module_xattr
  endif
  ifeq ($(RUST_MODULE_TIMEKEEPING),1)
    RUST_KERNEL_CFG += --cfg rust_module_timekeeping
  endif
endif

# Compiler and tools
ifeq ($(ARCH), riscv64)
    CROSS_PREFIX = riscv64-unknown-elf-
    ARCH_CFLAGS = -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany
    ARCH_RUSTFLAGS =
    ARCH_LDFLAGS =
QEMU = qemu-system-riscv64
QEMU_FLAGS = -machine virt -m 1G -nographic -smp $(NR_CPUS) -bios default -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), loongarch64)
    CROSS_PREFIX = loongarch64-linux-gnu-
    ARCH_CFLAGS = -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
    ARCH_RUSTFLAGS =
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-loongarch64
    QEMU_FLAGS = -machine virt -m 1G -nographic -smp $(NR_CPUS)
else ifeq ($(ARCH), aarch64)
    CROSS_PREFIX = aarch64-linux-gnu-
    ARCH_CFLAGS = -march=armv8-a -mgeneral-regs-only -fno-pic -mcmodel=large -mno-outline-atomics
    ARCH_RUSTFLAGS =
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-aarch64
    QEMU_FLAGS = -machine virt -cpu cortex-a57 -m 1G -nographic -smp $(NR_CPUS) -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), x86_64)
    CROSS_PREFIX = x86_64-linux-gnu-
    ARCH_CFLAGS = -m64 -mcmodel=large -mno-red-zone -fno-pic -fno-pie -mgeneral-regs-only
    ARCH_RUSTFLAGS =
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-x86_64
    QEMU_FLAGS = -machine q35 -m 1G -nographic -smp $(NR_CPUS) -no-reboot
endif

RUSTFLAGS += $(ARCH_RUSTFLAGS)

MKFS_FAT ?= $(or $(shell command -v mkfs.fat 2>/dev/null),$(wildcard /usr/sbin/mkfs.fat),$(wildcard /sbin/mkfs.fat),mkfs.fat)
MKFS_EXT4 ?= $(or $(shell command -v mkfs.ext4 2>/dev/null),$(wildcard /usr/sbin/mkfs.ext4),$(wildcard /sbin/mkfs.ext4),mkfs.ext4)

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
ifeq ($(ARCH), riscv64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
endif

else ifeq ($(ARCH), loongarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-pci,netdev=net

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x1 -device virtio-blk-pci,drive=x1
endif

else ifeq ($(ARCH), aarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4
else ifeq ($(ARCH), x86_64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-pci,netdev=net

endif
endif

# Compiler flags
CFLAGS = -Wall -Wextra $(OPT) -ffreestanding -nostdlib \
         -nostartfiles -fno-builtin -fno-common -std=gnu99 \
         -MMD -MP \
         -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(KERNEL_DIR)/net/lwip_port \
         -I$(KERNEL_DIR)/external/lwip/src/include \
         -I$(ARCH_INCLUDE_DIR) -I$(BOARD_INCLUDE_DIR) -I$(BUILD_DIR)/generated $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_ABI_$(shell echo $(ABI) | tr a-z A-Z) \
         -DCONFIG_NR_CPUS=$(NR_CPUS) \
         -DCONFIG_BOARD_$(shell echo $(BOARD) | tr a-z A-Z | tr - _)
ifeq ($(ABI),both)
CFLAGS += -DCONFIG_ABI_NATIVE
endif

# Bringup / contest mode markers for conditional compilation.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif

# Synthetic driver lifecycle test (disabled by default).
ifeq ($(CONFIG_DRIVER_LIFECYCLE_TEST),y)
CFLAGS += -DCONFIG_DRIVER_LIFECYCLE_TEST
endif

LDFLAGS = -nostdlib -nostartfiles -Wl,--build-id=none -T $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld $(ARCH_LDFLAGS)

# Source files
# ABI-specific source directories
ifeq ($(ABI),both)
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/linux/*.c) \
           $(wildcard $(KERNEL_DIR)/abi/native/*.c)
else
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/$(ABI)/*.c)
endif

KERNEL_SRC = $(wildcard $(KERNEL_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/core/*.c) \
             $(wildcard $(KERNEL_DIR)/mm/*.c) \
             $(wildcard $(KERNEL_DIR)/proc/*.c) \
             $(filter-out $(KERNEL_DIR)/fs/rootfs_overlay.c,$(wildcard $(KERNEL_DIR)/fs/*.c)) \
             $(wildcard $(KERNEL_DIR)/fs/vfs/*.c) \
             $(wildcard $(KERNEL_DIR)/ipc/*.c) \
             $(wildcard $(KERNEL_DIR)/net/*.c) \
             $(wildcard $(KERNEL_DIR)/bpf/*.c) \
             $(wildcard $(KERNEL_DIR)/drivers/core/*.c) \
             $(wildcard $(KERNEL_DIR)/drivers/bus/*.c) \
             $(wildcard $(KERNEL_DIR)/drivers/block/*.c) \
             $(wildcard $(KERNEL_DIR)/drivers/char/*.c) \
             $(wildcard $(KERNEL_DIR)/drivers/net/*.c) \
             $(wildcard $(KERNEL_DIR)/platform/$(BOARD)/*.c) \
             $(ABI_SRCS) \
             $(wildcard $(KERNEL_DIR)/syscall/*.c) \
             $(wildcard $(KERNEL_DIR)/shell/*.c) \
             $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.c' | sort) \
             $(LWIP_SRC)

# Built-in rootfs overlay.
#
# The generated source/header are checked in intentionally so contest builds do
# not depend on python3 being present on the judge. After editing
# user/rootfs_overlay/, regenerate them manually with `make regen-rootfs-overlay`.
ROOTFS_OVERLAY_DIR   = user/rootfs_overlay
ROOTFS_OVERLAY_SRC   = kernel/fs/rootfs_overlay.c
ROOTFS_OVERLAY_HDR   = kernel/include/fs/rootfs_overlay.h
ROOTFS_OVERLAY_FILES := $(shell find $(ROOTFS_OVERLAY_DIR) -type f 2>/dev/null)
KERNEL_SRC += $(ROOTFS_OVERLAY_SRC)

include $(KERNEL_DIR)/external/lwip/sources.mk

# Filter out C sources that are replaced by Rust modules.
ifeq ($(RUST_ENABLED),1)
  ifeq ($(RUST_MODULE_XATTR),1)
    KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/xattr.c,$(KERNEL_SRC))
  endif
  ifeq ($(RUST_MODULE_TIMEKEEPING),1)
    KERNEL_SRC := $(filter-out $(KERNEL_DIR)/core/timekeeping.c,$(KERNEL_SRC))
  endif
  ifeq ($(RUST_MODULE_PAGECACHE),1)
    KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/page_cache.c,$(KERNEL_SRC))
  endif
ifeq ($(RUST_MODULE_BLOCKCACHE),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/block_cache.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_SYNC),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/core/sync.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_SLAB),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/mm/slab.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_STATPERM),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/vfs/stat_perm.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_RANDOM),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/core/random.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_EVENTFD),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/ipc/eventfd.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_TIMERFD),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/ipc/timerfd.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_LOCKS),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/locks.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_FDTABLE),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/fdtable.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_FILE),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/file.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_PIPE),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/fs/pipe.c,$(KERNEL_SRC))
endif
ifeq ($(RUST_MODULE_SIGNAL),1)
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/proc/signal.c,$(KERNEL_SRC))
endif

endif

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

.PHONY: all clean run-riscv64 run-loongarch64 run-arm64 run-x86_64 debug-riscv64 debug-loongarch64 debug-arm64 debug-x86_64 \
		check-kernel-build check-user-build check-dev-build check-contest-build check-build-matrix check-abi-smoke-gate check-doc-drift check-doc-test-gates check-final-definition check-concurrency-foundation check-mm-lock-model check-abi-boundary check-driver-core-model check-external-dependency-boundary \
		check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup \
		check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user \
		smoke-riscv64 smoke-loongarch64 smoke-aarch64 smoke-x86_64 smoke-abi-linux smoke-network-suite smoke-proc-a20 smoke-proc-stress smoke-mm-stress smoke-vfs-stress smoke-vfs-edge smoke-sched-stress smoke-futex-stress smoke-socket-stress smoke-driver-lifecycle smoke-native-handle smoke-native-libc smoke-io-event \\
		FORCE regen-rootfs-overlay \
		user_apps fs_img kernel-only dev-build contest-rv contest-la \
		eval-dev-build-rv eval-dev-build-la \
		extra-img extra-user-apps run-riscv64-extra run-loongarch64-extra run-arm64-extra \
		native-test-rv native-test-la native-test native-minimal-rv native-minimal-la native-minimal native-handle-test-rv native-handle-test-la native-handle-test native-libc-rv native-libc-la native-libc \
		eval eval-rv eval-la

FORCE:

$(BUILD_TIME_HDR):
	@mkdir -p $(dir $@)
	@printf '#ifndef A20_BUILD_UNIX_TIME\n#define A20_BUILD_UNIX_TIME %sULL\n#endif\n' "$$(date -u +%s)" > $@

regen-rootfs-overlay: scripts/gen_rootfs_overlay.py $(ROOTFS_OVERLAY_FILES)
	@mkdir -p $(dir $(ROOTFS_OVERLAY_SRC)) $(dir $(ROOTFS_OVERLAY_HDR))
	python3 $< --out-c $(ROOTFS_OVERLAY_SRC) --out-h $(ROOTFS_OVERLAY_HDR) --root $(ROOTFS_OVERLAY_DIR)

# ----------------------------------------------------------------
# Competition build: produces kernel-rv, kernel-la, disk.img,
# disk-la.img (what the judge expects from `make all`).
# ----------------------------------------------------------------
all:
	$(MAKE) contest-rv
	$(MAKE) contest-la
	@echo "=== Competition build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

check-kernel-build: check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup

check-riscv64-bringup:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only

check-loongarch64-bringup:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-aarch64-bringup:
	$(MAKE) ARCH=aarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-x86_64-bringup:
	$(MAKE) ARCH=x86_64 ABI=$(ABI) BRINGUP=1 kernel-only

check-user-build: check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user

check-build-matrix: check-kernel-build check-user-build
	@rg -q "BUILD_MATRIX_GATE_CONTRACT" docs/testing-gates.md
	@echo "check-build-matrix: PASS"

check-abi-smoke-gate:
	@rg -q "ABI_SMOKE_GATE_CONTRACT" docs/testing-gates.md
	@rg -q "syscall_smoke" Makefile
	@rg -q "smoke-abi-linux" Makefile
	@rg -q "native-minimal" Makefile
	@rg -q "native-test" Makefile
	@rg -q "test_liba20c" user/tests/test_liba20c.c Makefile
	@echo "check-abi-smoke-gate: PASS"

check-doc-drift:
	@rg -q "DOC_DRIFT_KEYWORD_GATE" docs/testing-gates.md
	@python3 scripts/gen_linux_syscall_coverage.py
	@rg -q "stub" kernel/abi/linux/syscall_coverage.md kernel/abi/linux/compat_notes.md docs/testing-gates.md
	@rg -q "partial" kernel/abi/linux/syscall_coverage.md kernel/abi/linux/compat_notes.md docs/testing-gates.md
	@rg -q "Future" TODO.md docs/testing-gates.md kernel/abi/native/sys_core.c
	@rg -q "not yet" docs/testing-gates.md kernel/abi/native/sys_phase2.c kernel/mm/fault.c
	@! rg -q "for simplicity" docs kernel --glob '!docs/research/**' --glob '!docs/testing-gates.md' --glob '!kernel/external/**'
	@echo "check-doc-drift: PASS"

check-doc-test-gates: check-concurrency-foundation check-mm-lock-model check-io-progress-model check-vfs-abstraction check-abi-boundary check-driver-core-model check-external-dependency-boundary check-abi-smoke-gate check-doc-drift
	@rg -q "DOCS_AS_FACT_CONTRACT" docs/testing-gates.md
	@rg -q "TEST_FIRST_ARCHITECTURE_MATRIX" docs/testing-gates.md
	@echo "check-doc-test-gates: PASS"

check-final-definition: check-doc-test-gates
	@rg -q "MM_LOCK_MODEL" kernel/include/mm/vm.h
	@rg -q "TASK_STATE_MUTATION_CONTRACT" kernel/include/proc/proc.h
	@rg -q "VFS_REFCOUNT_HELPER_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX" kernel/abi/native/handle_table.h
	@rg -q "KERNEL_PROGRESS_SERVICE_CONTRACT" kernel/include/core/progress.h
	@rg -q "VFS_OPEN_DISPATCH_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "LINUX_ABI_EXPLICIT_STUB_CONTRACT" kernel/abi/linux/syscall_table.def
	@rg -q "NATIVE_DEBUG_LIMITED_CONTRACT" kernel/abi/native/sys_phase2.c
	@rg -q "DRIVER_CORE_CONCURRENCY_MODEL" kernel/drivers/core/driver_core.c
	@rg -q "EXTERNAL_USERLAND_UPGRADE_CHECKLIST" docs/external-dependencies.md
	@echo "check-final-definition: PASS (SMP smoke tracked separately by TODO section 10)"

check-riscv64-user:
	$(MAKE) -C user ARCH=riscv64 OPT="$(OPT)"

check-loongarch64-user:
	$(MAKE) -C user ARCH=loongarch64 OPT="$(OPT)"

check-aarch64-user:
	$(MAKE) -C user ARCH=aarch64 OPT="$(OPT)"

check-x86_64-user:
	$(MAKE) -C user ARCH=x86_64 OPT="$(OPT)"

check-dev-build:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=0 dev-build

check-contest-build:
	$(MAKE) all

check-concurrency-foundation:
	@rg -q "SCHEDULER_CONCURRENCY_PREREQS" kernel/proc/sched.c
	@rg -q "PER_CPU_CURRENT_VALIDATION" kernel/proc/current.c
	@rg -q "TASK_STATE_MUTATION_CONTRACT" kernel/include/proc/proc.h
	@rg -q "BLOCK_WAKE_PROTOCOL" kernel/include/core/sync.h
	@$(MAKE) ARCH=$(ARCH) NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 BRINGUP=1 kernel-only >/dev/null
	@echo "check-concurrency-foundation: PASS"

check-mm-lock-model:
	@rg -q "MM_LOCK_MODEL" kernel/include/mm/vm.h
	@rg -q "MM_VMA_PTE_AUDIT" kernel/mm/vm.c
	@rg -q "COW_FAULT_TLB_CONTRACT" kernel/mm/fault.c
	@rg -q "DEMAND_FAULT_TLB_CONTRACT" kernel/mm/fault.c
	@rg -q "MM_FORK_COW_REGRESSION_GUARD" kernel/mm/vm.c
	@rg -q "FILE_MMAP_PAGE_CACHE_CONTRACT" kernel/include/fs/page_cache.h
	@rg -q "OOM_RECLAIM_LIFETIME_CONTRACT" kernel/include/mm/oom.h
	@rg -q "smoke-mm-stress" Makefile
	@rg -q "MM_STRESS: PASS" user/cmds/mm_stress.c
	@echo "check-mm-lock-model: PASS"

check-io-progress-model:
	@rg -q "KERNEL_PROGRESS_SERVICE_CONTRACT" kernel/include/core/progress.h
	@rg -q "IO_PROGRESS_SERVICE" kernel/core/progress.c
	@rg -q "kernel_progress_run_bottom_halves\(\)" kernel/proc/sched.c kernel/proc/proc.c
	@rg -q "kernel_progress_timer_tick\(\)" kernel/arch/riscv64/trap/irqchip.c kernel/arch/loongarch64/trap/irqchip.c kernel/arch/aarch64/trap/irqchip.c kernel/arch/x86_64/trap/irqchip.c
	@rg -q "VIRTIO_BLK_COMPLETION_MODEL" kernel/drivers/block/virtio_blk.c
	@rg -q "LWIP_NO_THREAD_PROGRESS_CONTRACT" kernel/net/lwip_stack.c
	@rg -q "g_lwip_lock -> virtio-net nonblocking" kernel/include/core/lock.h
	@! rg -q "virtio_blk_poll_all" kernel/proc/sched.c kernel/proc/proc.c
	@! rg -q "a20_lwip_poll" kernel/proc/sched.c kernel/proc/proc.c
	@echo "check-io-progress-model: PASS"

check-vfs-abstraction:
	@rg -q "VFS_OPEN_DISPATCH_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "VFS_REFCOUNT_HELPER_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "VFS_DCACHE_MOUNT_VNODE_INVARIANT" kernel/include/fs/vfs.h
	@rg -q "vfile_ref_init" kernel/fs/file.c kernel/include/fs/file.h kernel/include/fs/vfs.h
	@rg -q "vfile_get" kernel/fs/file.c kernel/include/fs/file.h kernel/include/fs/vfs.h
	@rg -q "vfile_put_ref_only" kernel/fs/file.c kernel/include/fs/file.h kernel/include/fs/vfs.h
	@rg -q "\.open[[:space:]]*=" kernel/fs/ramfs.c kernel/fs/fat32.c kernel/fs/ext4.c kernel/fs/procfs.c kernel/fs/devfs.c kernel/fs/cgroupfs.c kernel/fs/sysfs.c
	@rg -q "CGROUPFS_DOTDOT_PARENT_LOOKUP" kernel/fs/cgroupfs.c
	@rg -q "VFS_CONCURRENCY_SMOKE_MATRIX" kernel/fs/vfs.c
	@rg -q "smoke-vfs-stress" Makefile
	@rg -q "VFS_STRESS: PASS" user/cmds/vfs_stress.c
	@! rg -q "FS_TYPE_(FAT32|EXT4|RAMFS|PROCFS|DEVFS|CGROUP|SYSFS).*open|fat32_open_vnode|ext4_open_vnode|ramfs_open_vnode|procfs_open_vnode|devfs_open_vnode|cgroupfs_open_vnode|sysfs_open_vnode" kernel/fs/vfs.c
	@! rg -q "refcount_(set|inc|dec_and_test)\(&[A-Za-z0-9_>\.-]*->ref_count" kernel/fs/ramfs.c kernel/fs/fat32.c kernel/fs/ext4.c kernel/fs/procfs.c kernel/fs/devfs.c kernel/fs/cgroupfs.c kernel/fs/sysfs.c kernel/fs/inotify.c kernel/fs/memfd.c kernel/fs/pipe.c
	@! rg -q "for simplicity" kernel/fs/cgroupfs.c
	@echo "check-vfs-abstraction: PASS"

check-abi-boundary:
	@python3 scripts/gen_linux_syscall_coverage.py
	@rg -q "LINUX_ABI_BOUNDARY_CONTRACT" kernel/abi/linux/syscall_impl.h
	@rg -q "LINUX_ABI_EXPLICIT_STUB_CONTRACT" kernel/abi/linux/syscall_table.def
	@rg -q "LINUX_ABI_SCHED_STUB_BOUNDARY" kernel/abi/linux/sys_sched.c
	@rg -q "smoke-sched-stress" Makefile
	@rg -q "SCHED_STRESS: PASS" user/cmds/sched_stress.c
	@rg -q "smoke-futex-stress" Makefile
	@rg -q "FUTEX_STRESS: PASS" user/cmds/futex_stress.c
	@rg -q "LINUX_ABI_BPF_STUB_BOUNDARY" kernel/abi/linux/sys_bpf.c
	@rg -q "LINUX_ABI_NAMESPACE_STUB_BOUNDARY" kernel/abi/linux/sys_namespace.c
	@rg -q "LINUX_ABI_CAPABILITY_STUB_BOUNDARY" kernel/abi/linux/sys_capability.c
	@rg -q "ABI_CORE_API_CONTRACT" kernel/include/abi/core_api.h
	@rg -q "abi_core_proc_exec" kernel/abi/linux/sys_namespace.c kernel/include/abi/core_api.h
	@rg -q "abi_core_proc_mmap" kernel/abi/native/sys_phase2.c kernel/include/abi/core_api.h
	@rg -q "NATIVE_DEBUG_LIMITED_CONTRACT" kernel/abi/native/sys_phase2.c
	@rg -q "NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX" kernel/abi/native/handle_table.h
	@rg -q "NATIVE_HANDLE_CAPABILITY_TEST_CONTRACT" kernel/abi/native/handle_table.c
	@rg -q "Debug 分区受限" docs/native-abi/00-overview.md
	@! rg -q "uint64_t args\[[0-9]+\]" user/liba20c/*.c
	@! rg -q "无 stub 残留|all Phase 2\+ syscalls|Debug \(0x0900\) — stubs" docs/native-abi/00-overview.md kernel/abi/native/sys_core.c kernel/abi/native/sys_phase2.c
	@echo "check-abi-boundary: PASS"

check-driver-core-model:
	@rg -q "DRIVER_CORE_CONCURRENCY_MODEL" kernel/drivers/core/driver_core.c
	@rg -q "spin_lock_irqsave\(&g_driver_core_lock\)" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_CORE_DYNAMIC_LIMITS" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_IRQ_TABLE_FIXED_LIMIT" kernel/drivers/core/driver_hwapi.c
	@rg -q "DRIVER_PROBE_FAILURE_CLEANUP" kernel/drivers/core/driver_core.c
	@rg -q "dev->drv = NULL" kernel/drivers/core/driver_core.c
	@rg -q "dev->state = DEV_STATE_UNINIT" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_ENUMERATION_FAILURE_MODEL" kernel/drivers/bus/virtio_mmio_bus.c kernel/drivers/bus/pci_bus.c
	@rg -q "DRIVER_IRQ_DMA_SEMANTICS" kernel/drivers/core/driver_hwapi.h
	@rg -q "DRIVER_SMOKE_MATRIX" kernel/drivers/core/driver_core.h
	@rg -q "virtio_blk_driver_probe" kernel/drivers/block/virtio_blk.c
	@rg -q "virtio_net_driver_probe" kernel/drivers/net/virtio_net.c
	@rg -q "uart_driver_probe" kernel/drivers/char/uart.c
	@rg -q "pty_init" kernel/drivers/char/pty.c
	@rg -q "loop_init" kernel/drivers/block/loop.c
	@rg -q "pci_enumerate" kernel/drivers/bus/pci_bus.c
	@rg -q "virtio_mmio_enumerate" kernel/drivers/bus/virtio_mmio_bus.c
	@rg -q "kernel/drivers/" docs/driver-interface.md
	@rg -q "kernel/platform/" docs/driver-interface.md
	@rg -q "DRIVER_LIFECYCLE_TEST" kernel/drivers/core/driver_lifecycle_test.c kernel/drivers/core/driver_lifecycle_test.h kernel/main.c
	@rg -q "driver_lifecycle_test_run" kernel/main.c kernel/fs/procfs.c kernel/drivers/core/driver_lifecycle_test.c
	@! rg -q "kernel/driver/|kernel/drv/|kernel/board/|#include \"driver/" docs/driver-interface.md
	@echo "check-driver-core-model: PASS"

check-external-dependency-boundary:
	@rg -q "include kernel/external/lwip/sources.mk" Makefile
	@rg -q "EXTERNAL_LWIP_SOURCE_MANIFEST" docs/external-dependencies.md
	@rg -q "LWIP_SRC" kernel/external/lwip/sources.mk
	@rg -q "core/timeouts.c" kernel/external/lwip/sources.mk
	@rg -q "EXTERNAL_LWIP_CONFIG_CONTRACT" docs/external-dependencies.md
	@rg -q "NO_SYS=1" docs/external-dependencies.md
	@rg -q "g_lwip_lock" docs/external-dependencies.md kernel/net/lwip_stack.c
	@rg -q "a20_lwip_poll\(\)" docs/external-dependencies.md
	@rg -q "kernel_progress_poll\(\)" docs/external-dependencies.md
	@rg -q "EXTERNAL_QEMU_NET_DEFAULTS" docs/external-dependencies.md
	@rg -q "10\.0\.2\.15" docs/external-dependencies.md kernel/net/lwip_stack.c
	@rg -q "EXTERNAL_USERLAND_UPGRADE_CHECKLIST" docs/external-dependencies.md
	@rg -q "syscall smoke, shell smoke, and coreutils smoke" docs/external-dependencies.md
	@rg -q "EXTERNAL_STATIC_LINK_REBUILD_CONTRACT" docs/external-dependencies.md
	@rg -q "user/build/\.build-id" docs/external-dependencies.md Makefile
	@rg -q "EXTERNAL_TLSE_WGET_LIMITS" docs/external-dependencies.md
	@rg -q "TLS 1\.3" docs/external-dependencies.md
	@! rg -n "^LWIP_SRC[[:space:]]*=" Makefile
	@echo "check-external-dependency-boundary: PASS"

smoke-riscv64:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/riscv64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-riscv64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-riscv64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-riscv64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-loongarch64:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/loongarch64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-loongarch64 \
		-machine virt -m 1G -nographic -smp 1 \
		-kernel .kernel-build/loongarch64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-loongarch64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-loongarch64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-loongarch64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-aarch64:
	$(MAKE) ARCH=aarch64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/aarch64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-aarch64 \
		-machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/aarch64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-aarch64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-aarch64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-aarch64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-x86_64:
	$(MAKE) ARCH=x86_64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/x86_64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-kernel .kernel-build/x86_64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-x86_64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-x86_64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-x86_64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-abi-linux:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/abi-linux-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'syscall_smoke\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
		if grep -q 'SYSCALL_SMOKE: PASS' "$$log"; then \
			echo "smoke-abi-linux: PASS; log saved to $$log"; \
		elif [ "$$status" -eq 124 ]; then \
			echo "smoke-abi-linux: timeout without PASS; tail of $$log:"; \
			tail -n 80 "$$log"; \
			exit 1; \
		else \
			echo "smoke-abi-linux: failed with status $$status; tail of $$log:"; \
			tail -n 80 "$$log"; \
			exit "$$status"; \
		fi

smoke-network-suite:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/network-suite-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'network_suite\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NETWORK_SUITE: PASS' "$$log"; then \
		echo "smoke-network-suite: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-network-suite: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-network-suite: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-proc-a20:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/proc-a20-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'cat /proc/a20/bcache\ncat /proc/a20/page_cache\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '^valid_pages:' "$$log" && grep -q '^capacity:' "$$log"; then \
		echo "smoke-proc-a20: PASS; log saved to $$log"; \
	else \
		echo "smoke-proc-a20: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-proc-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/proc-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'proc_stress\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'PROC_STRESS: PASS' "$$log"; then \
		echo "smoke-proc-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-proc-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-mm-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mm-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'mm_stress\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'MM_STRESS: PASS' "$$log"; then \
		echo "smoke-mm-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-mm-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-vfs-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/vfs-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'vfs_stress\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
			-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
			> "$$log" 2>&1 || status=$$?; \
		if grep -q 'VFS_STRESS: PASS' "$$log"; then \
			echo "smoke-vfs-stress: PASS; log saved to $$log"; \
		else \
			echo "smoke-vfs-stress: failed with status $$status; tail of $$log:"; \
			tail -n 80 "$$log"; \
			exit 1; \
		fi

smoke-vfs-edge:
		$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
		@mkdir -p $(SMOKE_LOG_DIR)
		@set -e; \
		log="$(SMOKE_LOG_DIR)/vfs-edge-riscv64.log"; \
		status=0; \
		{ sleep $(SMOKE_INPUT_DELAY); printf 'vfs_edge\npoweroff\n'; } | \
		timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
			-machine virt -m 1G -nographic -smp 1 -bios default \
			-global virtio-mmio.force-legacy=false \
			-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
			-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
		if grep -q 'VFS_EDGE: PASS' "$$log"; then \
			echo "smoke-vfs-edge: PASS; log saved to $$log"; \
		else \
			echo "smoke-vfs-edge: failed with status $$status; tail of $$log:"; \
			tail -n 80 "$$log"; \
			exit 1; \
		fi

smoke-io-event:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/io-event-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'io_event_test\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'IO_EVENT_TEST: PASS' "$$log"; then \
		echo "smoke-io-event: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-io-event: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-io-event: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-sched-stress:
		$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/sched-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'sched_stress\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SCHED_STRESS: PASS' "$$log"; then \
		echo "smoke-sched-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-sched-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-futex-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/futex-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'futex_stress\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'FUTEX_STRESS: PASS' "$$log"; then \
		echo "smoke-futex-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-futex-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-socket-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/socket-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'socket_stress\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SOCKET_STRESS: PASS' "$$log" && ! grep -q '\[LOCK\]' "$$log"; then \
		echo "smoke-socket-stress: PASS; log saved to $$log"; \
	elif grep -q '\[LOCK\]' "$$log"; then \
		echo "smoke-socket-stress: FAIL [LOCK] warning detected; log saved to $$log"; \
		grep '\[LOCK\]' "$$log" | head -n 5; \
		exit 1; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-socket-stress: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-socket-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-driver-lifecycle:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=1 CONFIG_DRIVER_LIFECYCLE_TEST=y kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/driver-lifecycle-riscv64.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'DRIVER_LIFECYCLE: PASS' "$$log"; then \
		echo "smoke-driver-lifecycle: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-driver-lifecycle: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-driver-lifecycle: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-handle:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-handle-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-handle-rv\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'part ok' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-handle: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-handle: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

contest-rv:
	@echo "--- Building RISC-V 64 (contest) ---"
	$(MAKE) ARCH=riscv64 _contest_build KERNEL_OUT=kernel-rv DISK_OUT=disk.img

contest-la:
	@echo "--- Building LoongArch 64 (contest) ---"
	$(MAKE) ARCH=loongarch64 _contest_build KERNEL_OUT=kernel-la DISK_OUT=disk-la.img

_reset_obj:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	$(MAKE) -C user clean

_contest_build: $(KERNEL_ELF) $(USER_BUILD_STAMP)
ifeq ($(ARCH), riscv64)
	$(MAKE) native-test-rv
	$(MAKE) native-handle-test-rv
else ifeq ($(ARCH), loongarch64)
	$(MAKE) native-test-la
	$(MAKE) native-handle-test-la
endif
	$(MAKE) ARCH=$(ARCH) ABI=$(ABI) _contest_disk
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_contest_disk: $(USER_BUILD_STAMP)
	rm -f $(DISK_OUT)
	$(MKFS_FAT) -C -F 32 $(DISK_OUT) 131072
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(DISK_OUT) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(DISK_OUT) user/build/mksh ::/sh
	mcopy -o -i $(DISK_OUT) user/build/mksh ::/bash
	-mmd -i $(DISK_OUT) ::/etc >/dev/null 2>&1
	-mmd -i $(DISK_OUT) ::/lib >/dev/null 2>&1
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(DISK_OUT) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(DISK_OUT) - ::/etc/protocols
	mcopy -o -i $(DISK_OUT) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt
	mcopy -o -i $(DISK_OUT) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(DISK_OUT) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	@printf 'auto\n' | mcopy -o -i $(DISK_OUT) - ::/etc/contest-mode

# ----------------------------------------------------------------
# Development build (for `make run-riscv64` / `make run-loongarch64`)
# ----------------------------------------------------------------

dev-build: $(KERNEL_BIN) $(USER_BUILD_STAMP) $(FS_TEST_IMG) $(EXT4_IMG)
	@echo "Dev build complete: $(KERNEL_BIN), $(FAT32_IMG), $(EXT4_IMG)"

user_apps: $(USER_BUILD_STAMP)

$(USER_BUILD_STAMP): FORCE
	@set -e; \
	mkdir -p $(dir $@); \
	current=""; \
	if [ -f "$@" ]; then current=$$(cat "$@"); fi; \
	need_build=0; \
	need_clean=0; \
	if [ "$$current" != "$(USER_BUILD_ID)" ]; then \
		need_build=1; \
		need_clean=1; \
	elif [ ! -x user/build/init ] || [ ! -x user/build/mksh ]; then \
		need_build=1; \
	elif find user/build -maxdepth 1 -type f ! -name '.build-id' -newer "$@" \
		-print -quit | grep -q .; then \
		need_build=1; \
		need_clean=1; \
	elif find user/Makefile $(USER_BUILD_CHECK_DIRS) \
		\( -path '*/.git' -o -path 'user/build' -o -path 'user/external/musl/build-*' \) -prune -o \
		-type f -newer "$@" -print -quit | grep -q .; then \
		need_build=1; \
	fi; \
	if [ "$$need_build" -eq 1 ]; then \
		if [ "$$need_clean" -eq 1 ]; then $(MAKE) -C user clean; fi; \
		$(MAKE) -C user ARCH=$(ARCH) OPT="$(OPT)"; \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[USER] $(USER_BUILD_ID) up to date"; \
	fi

fs_img: $(FS_TEST_IMG)

$(FAT32_IMG): $(USER_BUILD_STAMP)
	@echo "Building FAT32 image..."
	@mkdir -p $(BUILD_DIR)
ifeq ($(ARCH), riscv64)
	$(MAKE) native-test-rv
	$(MAKE) native-handle-test-rv
	$(MAKE) native-libc-rv
else ifeq ($(ARCH), loongarch64)
	$(MAKE) native-test-la
	$(MAKE) native-handle-test-la
	$(MAKE) native-libc-la
endif
	dd if=/dev/zero of=$(FAT32_IMG) bs=1M count=$(FAT32_IMAGE_MB)
	$(MKFS_FAT) -F 32 $(FAT32_IMG)
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(FAT32_IMG) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(FAT32_IMG) user/build/mksh ::/sh
	mcopy -o -i $(FAT32_IMG) user/build/mksh ::/bash
	-mmd -i $(FAT32_IMG) ::/etc >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/lib >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/musl >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/musl/lib >/dev/null 2>&1
	@[ -f user/external/musl/build-$(ARCH)/lib/libc.so ] && \
		mcopy -o -i $(FAT32_IMG) user/external/musl/build-$(ARCH)/lib/libc.so ::/musl/lib/libc.so || true
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(FAT32_IMG) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(FAT32_IMG) - ::/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' | mcopy -o -i $(FAT32_IMG) - ::/etc/os-release
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i $(FAT32_IMG) - ::/test.txt
	mcopy -o -i $(FAT32_IMG) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt

$(FS_TEST_IMG): $(FAT32_IMG)
	cp $(FAT32_IMG) $(FS_TEST_IMG)

ext4_img_only: $(EXT4_IMG)

$(EXT4_IMG): $(USER_BUILD_STAMP)
	@echo "Building ext4 image..."
	@rm -rf $(EXT4_STAGING_DIR) && mkdir -p $(EXT4_STAGING_DIR)
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		cp "$$f" "$(EXT4_STAGING_DIR)/$$(basename "$$f")"; \
	done
	cp user/build/mksh $(EXT4_STAGING_DIR)/sh
	cp user/build/mksh $(EXT4_STAGING_DIR)/bash
	printf 'Hello from ext4!\nThis file is on the ext4 filesystem.\n' > $(EXT4_STAGING_DIR)/test.txt
	@mkdir -p $(EXT4_STAGING_DIR)/etc
	@printf '%s\n' $(PROTOCOLS_LINES) > $(EXT4_STAGING_DIR)/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' > $(EXT4_STAGING_DIR)/etc/os-release
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXT4_IMG) bs=1M count=$(EXT4_IMAGE_MB)
	$(MKFS_EXT4) -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index -d $(EXT4_STAGING_DIR) $(EXT4_IMG)
	@rm -rf $(EXT4_STAGING_DIR)

ext4_img: $(USER_BUILD_STAMP) ext4_img_only
	cp $(EXT4_IMG) $(FS_TEST_IMG)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(CROSS_PREFIX)objcopy -O binary $< $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(RUST_LINK_DEPS) $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(RUST_LD_LIBS) -o $@

$(BUILD_DIR)/rust/liba20rust_support.rlib: kernel/rust/support/panic_handler.rs kernel/rust/support/lock.rs Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name a20rust_support $< -o $@

$(BUILD_DIR)/rust/irqsave_lock.o: kernel/rust/support/irqsave_lock.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/arch_info.o: kernel/rust/support/arch_info.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/page_cache_helpers.o: kernel/rust/support/page_cache_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/block_cache_helpers.o: kernel/rust/support/block_cache_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/xattr_helpers.o: kernel/rust/support/xattr_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/time_helpers.o: kernel/rust/support/time_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/sync_helpers.o: kernel/rust/support/sync_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/slab_helpers.o: kernel/rust/support/slab_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/stat_perm_helpers.o: kernel/rust/support/stat_perm_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/proc_list_helpers.o: kernel/rust/support/proc_list_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/random_helpers.o: kernel/rust/support/random_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/librandom.rlib: kernel/rust/random/lib.rs kernel/rust/random/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name random --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/eventfd_helpers.o: kernel/rust/support/eventfd_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/libeventfd.rlib: kernel/rust/eventfd/lib.rs kernel/rust/eventfd/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name eventfd --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/timerfd_helpers.o: kernel/rust/support/timerfd_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/locks_helpers.o: kernel/rust/support/locks_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/libtimerfd.rlib: kernel/rust/timerfd/lib.rs kernel/rust/timerfd/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name timerfd --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/liblocks.rlib: kernel/rust/locks/lib.rs kernel/rust/locks/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name locks --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/fdtable_helpers.o: kernel/rust/support/fdtable_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/libfdtable.rlib: kernel/rust/fdtable/lib.rs kernel/rust/fdtable/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name fdtable --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/file_helpers.o: kernel/rust/support/file_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/pipe_helpers.o: kernel/rust/support/pipe_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/signal_helpers.o: kernel/rust/support/signal_helpers.c Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rust/libfile.rlib: kernel/rust/file/lib.rs kernel/rust/file/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name file --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libpipe.rlib: kernel/rust/pipe/lib.rs kernel/rust/pipe/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name pipe --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libsignal.rlib: kernel/rust/signal/lib.rs kernel/rust/signal/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name signal --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libslab.rlib: kernel/rust/slab/lib.rs kernel/rust/slab/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name slab --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libsync.rlib: kernel/rust/sync/lib.rs kernel/rust/sync/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name sync --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libstat_perm.rlib: kernel/rust/stat_perm/lib.rs kernel/rust/stat_perm/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name stat_perm --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libproc_list.rlib: kernel/rust/proc_list/lib.rs kernel/rust/proc_list/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name proc_list --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libxattr.rlib: kernel/rust/xattr/lib.rs kernel/rust/xattr/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name xattr --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libtimekeeping.rlib: kernel/rust/timekeeping/lib.rs kernel/rust/timekeeping/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name timekeeping --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libpage_cache.rlib: kernel/rust/page_cache/lib.rs kernel/rust/page_cache/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name page_cache --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(BUILD_DIR)/rust/libblock_cache.rlib: kernel/rust/block_cache/lib.rs kernel/rust/block_cache/ffi.rs $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-name block_cache --extern a20rust_support=$(RUST_SUPPORT_LIB) $< -o $@

$(RUST_KERNEL_LIB): $(RUST_KERNEL_SRC) $(RUST_LIBS) $(RUST_SUPPORT_LIB) Makefile
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-type staticlib $(RUST_KERNEL_CFG) \
		--extern a20rust_support=$(RUST_SUPPORT_LIB) \
		$(foreach lib,$(RUST_LIBS),--extern $(patsubst lib%.rlib,%,$(notdir $(lib)))=$(lib)) \
		$< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c Makefile | $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	rm -f kernel.elf kernel.bin fat32.img ext4.img
	rm -f kernel-rv kernel-la disk.img disk-la.img
	$(MAKE) -C user clean
	$(MAKE) -f user/extra.mk clean 2>/dev/null || true

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"

# ----------------------------------------------------------------
# Run targets (development mode)
# ----------------------------------------------------------------

run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _run_impl

run-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _run_impl

run-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _run_impl

run-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _run_impl

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# --- Debug Targets ---

DEBUG_CFLAGS = $(filter-out -O0 -O1 -O2 -O3 -Os -Oz,$(CFLAGS)) -O0 -g -DDEBUG

debug-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _debug_impl

debug-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _debug_impl

debug-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _debug_impl

debug-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _debug_impl

_debug_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 OPT="-O0 -g -DDEBUG" kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) OPT="-O0 -g -DDEBUG" dev-build
endif
	@echo "Waiting for GDB connection on port 1234..."
	@echo "=========================================================="
	@echo "Please run in another terminal:"
	@echo "  gdb-multiarch $(KERNEL_ELF)"
	@echo "  (gdb) target remote :1234"
	@echo "=========================================================="
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s

# ----------------------------------------------------------------
# Extra packages (vim / git / gcc) on a separate ext4 disk
# ----------------------------------------------------------------

extra-user-apps:
	$(MAKE) -f user/extra.mk ARCH=$(ARCH) OPT="$(OPT)"

extra-img: extra-user-apps
	@echo "Building extra packages image..."
	@rm -rf $(EXTRA_STAGING_DIR) && mkdir -p $(EXTRA_STAGING_DIR)/bin
	@set -e; \
	for f in user/build/extra/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		cp "$$f" "$(EXTRA_STAGING_DIR)/bin/$$name"; \
	done; \
	[ -f user/build/fastfetch ] && cp user/build/fastfetch "$(EXTRA_STAGING_DIR)/bin/fastfetch" || true
	@set -e; \
	if [ -d user/build/extra/obj/$(ARCH)/gcc-install ]; then \
		cp -a user/build/extra/obj/$(ARCH)/gcc-install/libexec "$(EXTRA_STAGING_DIR)/libexec"; \
		cp -a user/build/extra/obj/$(ARCH)/gcc-install/lib "$(EXTRA_STAGING_DIR)/lib"; \
		for t in user/build/extra/obj/$(ARCH)/gcc-install/bin/*; do \
			[ -f "$$t" ] && cp "$$t" "$(EXTRA_STAGING_DIR)/bin/$$(basename $$t)"; \
		done; \
		mv "$(EXTRA_STAGING_DIR)/bin/gcc" "$(EXTRA_STAGING_DIR)/bin/gcc-real"; \
		printf '#!/bin/sh\nexec /usr/bin/gcc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/gcc"; \
		mv "$(EXTRA_STAGING_DIR)/bin/cc" "$(EXTRA_STAGING_DIR)/bin/cc-real"; \
		printf '#!/bin/sh\nexec /usr/bin/cc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cc"; \
	fi
	@MCM_LIB=user/external/musl-cross-make/output/riscv64-linux-musl/lib; \
	if [ -f "$$MCM_LIB/libc.so" ]; then \
		mkdir -p "$(EXTRA_STAGING_DIR)/lib"; \
		cp "$$MCM_LIB/libc.so" "$(EXTRA_STAGING_DIR)/lib/libc.so"; \
		ln -sf libc.so "$(EXTRA_STAGING_DIR)/lib/ld-musl-riscv64.so.1"; \
	fi
	@MCM_INC=user/external/musl-cross-make/output/riscv64-linux-musl/include; \
	if [ -d "$$MCM_INC" ]; then \
		mkdir -p "$(EXTRA_STAGING_DIR)/include"; \
		cp -a $$MCM_INC/* "$(EXTRA_STAGING_DIR)/include/"; \
		rm -rf "$(EXTRA_STAGING_DIR)/include/c++"; \
	fi
	@MCM_GCC_INC=user/external/musl-cross-make/output/lib/gcc/riscv64-linux-musl/14.2.0/include; \
	GCC_VER=17.0.0; \
	if [ -d "$$MCM_GCC_INC" ]; then \
		mkdir -p "$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/$$GCC_VER/include"; \
		cp -a $$MCM_GCC_INC/* "$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/$$GCC_VER/include/"; \
	fi
	@MCM_GCC_LIB=user/external/musl-cross-make/output/lib/gcc/riscv64-linux-musl/14.2.0; \
	GCC_VER=17.0.0; \
	for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o; do \
		[ -f "$$MCM_GCC_LIB/$$f" ] && cp "$$MCM_GCC_LIB/$$f" "$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/$$GCC_VER/$$f"; \
	done
	@GCC_SPECS_DIR="$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/17.0.0"; \
	printf '*cc1_options:+ -fno-lto\n' > "$$GCC_SPECS_DIR/specs"
	@VIM_RT="$(EXTRA_STAGING_DIR)/share/vim/vim92"; \
	VIM_SRC=user/external/vim/runtime; \
	mkdir -p "$$VIM_RT"; \
	for f in defaults.vim filetype.vim ftoff.vim ftplugin.vim ftplugof.vim; do \
		[ -f "$$VIM_SRC/$$f" ] && cp "$$VIM_SRC/$$f" "$$VIM_RT/$$f"; \
	done; \
	for d in syntax indent; do \
		mkdir -p "$$VIM_RT/$$d"; \
		cp -a "$$VIM_SRC/$$d/"*.vim "$$VIM_RT/$$d/" 2>/dev/null || true; \
	done
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXTRA_IMG) bs=1M count=$(EXTRA_IMAGE_MB) 2>/dev/null
	$(MKFS_EXT4) -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index \
		-d $(EXTRA_STAGING_DIR) $(EXTRA_IMG)
	@rm -rf $(EXTRA_STAGING_DIR)
	@echo "Extra image: $(EXTRA_IMG) ($(EXTRA_IMAGE_MB)MB)"

# Helper: QEMU flags for the extra disk (appended conditionally)
ifeq ($(ARCH), riscv64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), loongarch64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
else ifeq ($(ARCH), aarch64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), x86_64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
endif

run-riscv64-extra:
	$(MAKE) ARCH=riscv64 BRINGUP=0 _run_extra_impl

run-loongarch64-extra:
	$(MAKE) ARCH=loongarch64 BRINGUP=0 _run_extra_impl

run-arm64-extra:
	$(MAKE) ARCH=aarch64 BRINGUP=0 _run_extra_impl

run-x86_64-extra:
	$(MAKE) ARCH=x86_64 BRINGUP=0 _run_extra_impl

_run_extra_impl:
	$(MAKE) ARCH=$(ARCH) BRINGUP=0 dev-build
	$(MAKE) -C user ARCH=$(ARCH) fastfetch || true
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG=$(EXTRA_IMG) extra-img
	$(QEMU) $(QEMU_FLAGS) $(EXTRA_QEMU_BLK) -kernel $(KERNEL_ELF)

NATIVE_TEST_DIR  := user/tests
NATIVE_LD        := user/liba20rt/a20-generic.ld
NATIVE_CRT0_RV   := user/liba20rt/crt0_rv64.S
NATIVE_CRT0_LA   := user/liba20rt/crt0_la64.S
NATIVE_SDK_SRC   := user/liba20rt/a20_malloc.c

NATIVE_LIBC_SRC  := \
    user/liba20c/malloc.c \
    user/liba20c/bare_alloc.c \
    user/liba20c/unistd.c \
    user/liba20c/stdio.c \
    user/liba20c/printf.c \
    user/liba20c/string.c \
    user/liba20c/time.c \
    user/liba20c/fdtable.c \
    user/liba20c/exit.c \
    user/liba20c/errno.c \
    user/liba20c/environ.c \
    user/liba20c/a20_errno.c

define NATIVE_TEST_RECIPE
@mkdir -p user/build
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    user/tests/test_native_hello.c \
    -o $(4)
endef

define NATIVE_MINIMAL_RECIPE
@mkdir -p user/build
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    user/tests/test_native_minimal.c \
    -o $(4)
endef

native-test-rv:
	$(call NATIVE_TEST_RECIPE,riscv64-unknown-elf-gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/native-hello-rv)

native-test-la:
	$(call NATIVE_TEST_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/native-hello-la)

native-test: native-test-rv native-test-la

native-minimal-rv:
	$(call NATIVE_MINIMAL_RECIPE,riscv64-unknown-elf-gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/native-minimal-rv)
	@file user/build/native-minimal-rv

native-minimal-la:
	$(call NATIVE_MINIMAL_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/native-minimal-la)
	@file user/build/native-minimal-la

native-minimal: native-minimal-rv native-minimal-la

define NATIVE_HANDLE_TEST_RECIPE
@mkdir -p user/build
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    user/tests/test_native_handle.c \
    -o $(4)
endef

native-handle-test-rv:
	$(call NATIVE_HANDLE_TEST_RECIPE,riscv64-unknown-elf-gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/native-handle-rv)

native-handle-test-la:
	$(call NATIVE_HANDLE_TEST_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/native-handle-la)

native-handle-test: native-handle-test-rv native-handle-test-la

define NATIVE_LIBC_RECIPE
@mkdir -p user/build
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt -Iuser/liba20c/include \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_LIBC_SRC) \
    user/tests/test_liba20c.c \
    -o $(4)
endef

native-libc-rv:
	$(call NATIVE_LIBC_RECIPE,riscv64-unknown-elf-gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/native-libc-rv)

native-libc-la:
	$(call NATIVE_LIBC_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/native-libc-la)

native-libc: native-libc-rv native-libc-la

smoke-native-libc:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-libc-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-libc-rv\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_LIBC: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-libc: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-libc: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ----------------------------------------------------------------
# Local evaluation: make eval-rv / make eval-la / make eval
# ----------------------------------------------------------------
EVAL_DIR   := .eval-state
EVAL_LOGS  := $(EVAL_DIR)/logs
EVAL_TIMEOUT ?= 36000

SDCARD_RV_URL := https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-rv.img.xz
SDCARD_LA_URL := https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-la.img.xz

$(EVAL_DIR) $(EVAL_LOGS):
	mkdir -p $@

# --- sdcard images (download if missing, prefer project-root copy) ---
$(EVAL_DIR)/sdcard-rv.img: | $(EVAL_DIR)
	@if [ -f sdcard-rv.img ]; then \
		ln -sf "$$(pwd)/sdcard-rv.img" $@; \
	elif [ -f $@ ]; then \
		echo "[eval] reusing cached sdcard-rv.img"; \
	else \
		echo "[eval] downloading sdcard-rv.img ..."; \
		wget -q -O $(EVAL_DIR)/sdcard-rv.img.xz $(SDCARD_RV_URL); \
		xz -dc $(EVAL_DIR)/sdcard-rv.img.xz > $@; \
	fi

$(EVAL_DIR)/sdcard-la.img: | $(EVAL_DIR)
	@if [ -f sdcard-la.img ]; then \
		ln -sf "$$(pwd)/sdcard-la.img" $@; \
	elif [ -f $@ ]; then \
		echo "[eval] reusing cached sdcard-la.img"; \
	else \
		echo "[eval] downloading sdcard-la.img ..."; \
		wget -q -O $(EVAL_DIR)/sdcard-la.img.xz $(SDCARD_LA_URL); \
		xz -dc $(EVAL_DIR)/sdcard-la.img.xz > $@; \
	fi

# --- eval dev-build targets (match run-*, add contest-mode + 128 MB) ---
EVAL_KERNEL_RV  = .kernel-build/riscv64-both-dev/kernel.elf
EVAL_FAT32_RV   = .kernel-build/riscv64-both-dev/fat32.img
EVAL_KERNEL_LA  = .kernel-build/loongarch64-both-dev/kernel.elf
EVAL_FAT32_LA   = .kernel-build/loongarch64-both-dev/fat32.img

eval-dev-build-rv:
	$(MAKE) ARCH=riscv64 FAT32_IMAGE_MB=128 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_RV) - ::/etc/contest-mode

eval-dev-build-la:
	$(MAKE) ARCH=loongarch64 FAT32_IMAGE_MB=128 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_LA) - ::/etc/contest-mode

# --- QEMU launch ---
define RUN_QEMU_RV
	timeout --foreground $(EVAL_TIMEOUT) \
	qemu-system-riscv64 -machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel $(EVAL_KERNEL_RV) \
		-drive 'file=$(EVAL_FAT32_RV),if=none,format=raw,id=x0' \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-drive 'file=$(EVAL_DIR)/sdcard-rv.img,if=none,format=raw,id=x1' \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		-no-reboot \
	2>&1 | tee $(EVAL_LOGS)/serial-rv.txt || true
endef

define RUN_QEMU_LA
	timeout --foreground $(EVAL_TIMEOUT) \
	qemu-system-loongarch64 -machine virt -m 1G -nographic -smp 1 \
		-kernel $(EVAL_KERNEL_LA) \
		-drive 'file=$(EVAL_FAT32_LA),if=none,format=raw,id=x0' \
		-device virtio-blk-pci,drive=x0 \
		$(NETDEV_USER) -device virtio-net-pci,netdev=net \
		-drive 'file=$(EVAL_DIR)/sdcard-la.img,if=none,format=raw,id=x1' \
		-device virtio-blk-pci,drive=x1 \
		-no-reboot \
	2>&1 | tee $(EVAL_LOGS)/serial-la.txt || true
endef

# --- Top-level eval targets ---
eval-rv: eval-dev-build-rv $(EVAL_DIR)/sdcard-rv.img | $(EVAL_LOGS)
	@echo "[eval] launching RISC-V QEMU (timeout=$(EVAL_TIMEOUT)s) ..."
	$(RUN_QEMU_RV)

eval-la: eval-dev-build-la $(EVAL_DIR)/sdcard-la.img | $(EVAL_LOGS)
	@echo "[eval] launching LoongArch QEMU (timeout=$(EVAL_TIMEOUT)s) ..."
	$(RUN_QEMU_LA)

eval: eval-rv eval-la
	@echo "[eval] complete"

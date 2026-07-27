# A20OS Makefile

# Parallel build
NPROC ?= $(or $(shell getconf _NPROCESSORS_ONLN 2>/dev/null),$(shell sysctl -n hw.logicalcpu 2>/dev/null),4)
PYTHON ?= python3
TIMEOUT ?= $(PYTHON) scripts/run_with_timeout.py
HOST_OS ?= $(shell uname -s 2>/dev/null)

ifeq ($(HOST_OS),Darwin)
DEFAULT_CONTEST_TARGETS := contest-rv
DEFAULT_KERNEL_CHECK_TARGETS := check-riscv64-bringup check-stm32f103
DEFAULT_USER_CHECK_TARGETS := check-riscv64-user
DEFAULT_NATIVE_TEST_TARGETS := native-test-rv
DEFAULT_NATIVE_HANDLE_TARGETS := native-handle-test-rv
DEFAULT_NATIVE_LIBC_TARGETS := native-libc-rv
DEFAULT_EVAL_TARGETS := eval-rv
else
DEFAULT_CONTEST_TARGETS := contest-rv contest-la
DEFAULT_KERNEL_CHECK_TARGETS := check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup check-arm32-bringup check-riscv32-bringup check-ppc64le-bringup
DEFAULT_USER_CHECK_TARGETS := check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user check-arm32-user check-riscv32-user check-ppc64le-user
DEFAULT_NATIVE_TEST_TARGETS := native-test-rv native-test-la native-test-aarch64 native-test-x86_64 native-test-arm32 native-test-rv32 native-test-ppc64le
DEFAULT_NATIVE_HANDLE_TARGETS := native-handle-test-rv native-handle-test-la native-handle-test-aarch64 native-handle-test-x86_64 native-handle-test-arm32 native-handle-test-rv32 native-handle-test-ppc64le
DEFAULT_NATIVE_LIBC_TARGETS := native-libc-rv native-libc-la native-libc-aarch64 native-libc-x86_64 native-libc-arm32 native-libc-rv32 native-libc-ppc64le
DEFAULT_EVAL_TARGETS := eval-rv eval-la
endif

# Architecture selection
ARCH ?= riscv64
ABI ?= both
MODE ?= release
BRINGUP ?= 0
OPT ?= -O3
USER_OPT ?= $(OPT)
NR_CPUS ?= 1
COOPERATIVE_BOOT ?= 0
ALLOW_UNVERIFIED_SMP ?= 0
SMP_VERIFIED_QEMU_ARCHES := riscv64 aarch64 loongarch64 x86_64
PROFILE ?= full
CONFIG_SWAP ?= n
STM32_OPENOCD_INTERFACE ?= interface/cmsis-dap.cfg
STM32_OPENOCD_TRANSPORT ?= swd
STM32_OPENOCD_ADAPTER_KHZ ?= 1000
STM32_CMSIS_DAP_SERIAL ?=
STM32_BT_NAME ?= KasaneTeto
STM32_BT_PIN ?= 2233
STM32_BT_UUID ?= 1101
STM32_BT_BAUD ?= 38400
STM32_QEMU ?= 0
STM32_XUANWU_BUILD_DIR = .kernel-build/armv7m-both-bringup-nommu-stm32f103-f512k-r64k
STM32_XUANWU_ELF = $(STM32_XUANWU_BUILD_DIR)/kernel.elf
STM32_WIFI_SSID ?=
STM32_WIFI_PASSWORD ?=
ifeq ($(ARCH),armv7m)
BOARD ?= stm32f103
PROFILE := mcu
NOMMU := 1
BRINGUP := 1
STM32_FLASH_KB ?= 64
STM32_RAM_KB ?= 20
STM32_XUANWU ?= 0
else
BOARD ?= qemu-virt-$(ARCH)
endif
NOMMU ?= 0
ifeq ($(NOMMU),1)
CONFIG_SWAP := n
endif
NOMMU_SUPPORTED_ARCHES := riscv64 riscv32 aarch64 arm32 armv7m

ifeq ($(NOMMU),1)
ifeq ($(filter $(ARCH),$(NOMMU_SUPPORTED_ARCHES)),)
$(error NOMMU is unsupported for ARCH=$(ARCH); supported architectures: $(NOMMU_SUPPORTED_ARCHES))
endif
endif

ifneq ($(NR_CPUS),1)
ifeq ($(ALLOW_UNVERIFIED_SMP),0)
SMP_PLATFORM_VERIFIED := $(and $(filter $(ARCH),$(SMP_VERIFIED_QEMU_ARCHES)),$(filter $(BOARD),qemu-virt-$(ARCH)))
ifeq ($(SMP_PLATFORM_VERIFIED),)
SMP_VALIDATION_GOALS := check-concurrency-foundation check-doc-test-gates check-final-definition
ifeq ($(filter $(SMP_VALIDATION_GOALS),$(MAKECMDGOALS)),)
$(error NR_CPUS=$(NR_CPUS) is unverified for ARCH=$(ARCH) BOARD=$(BOARD); set ALLOW_UNVERIFIED_SMP=1 only for explicit SMP bringup experiments)
endif
endif
endif
endif

# ABI selection: linux, native, both (compile both ABI layers simultaneously)
ifeq ($(filter $(ABI),linux native both),)
$(error Unsupported ABI '$(ABI)'; supported: linux, native, both)
endif

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include
BUILD_VARIANT = $(ABI)-$(if $(filter 1,$(BRINGUP)),bringup,dev)$(if $(filter 1,$(NOMMU)),-nommu,)$(if $(filter-out 1,$(NR_CPUS)),-smp$(NR_CPUS),)$(if $(filter y,$(CONFIG_DRIVER_LIFECYCLE_TEST)),-driver-lifecycle,)$(if $(filter y,$(CONFIG_HDA_SMOKE_TEST)),-hda-smoke,)$(if $(filter y,$(CONFIG_NVME_SMOKE_TEST)),-nvme-smoke,)
ifeq ($(ARCH),armv7m)
BUILD_VARIANT := $(BUILD_VARIANT)-$(BOARD)-f$(STM32_FLASH_KB)k-r$(STM32_RAM_KB)k
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(filter 1,$(STM32_QEMU)),-qemu,)
endif
BUILD_DIR = .kernel-build/$(ARCH)-$(BOARD)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img
GUI_FAT32_IMG = $(BUILD_DIR)/gui-fat32.img
EXT4_IMG = $(BUILD_DIR)/ext4.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
USER_VARIANT = $(ARCH)$(if $(filter 1,$(NOMMU)),-nommu,)
USER_BUILD_DIR = user/build/$(USER_VARIANT)
USER_BUILD_STAMP = $(USER_BUILD_DIR)/.build-id
ARCH_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/include
BOARD_INCLUDE_DIR = $(KERNEL_DIR)/platform/$(BOARD)
BOARD_DRIVER_DIR = $(KERNEL_DIR)/drivers/$(if $(filter stm32f103,$(BOARD)),stm32f1,)
EXT4_STAGING_DIR = $(BUILD_DIR)/ext4-staging
BUILD_TIME_HDR = $(BUILD_DIR)/generated/build_time.h
STM32_BT_CONFIG_HDR = $(BUILD_DIR)/generated/stm32_bluetooth_config.h
STM32_WIFI_CONFIG_HDR = $(BUILD_DIR)/generated/stm32_wifi_config.h
FAT32_IMAGE_MB ?= 128
EXT4_IMAGE_MB ?= 128
EXTRA_IMAGE_MB ?= 1024
EXTRA_IMG = $(BUILD_DIR)/extra.img
EXTRA_STAGING_DIR = $(BUILD_DIR)/extra-staging
EXTRA_IMAGE_STAMP = $(BUILD_DIR)/.extra-image-id
EXTRA_PACKAGES ?= vim git gcc rust
RISCV_GNU_CC ?= riscv64-linux-gnu-gcc
RISCV_GLIBC_SYSROOT ?= $(shell $(RISCV_GNU_CC) -print-sysroot 2>/dev/null)
RISCV_GLIBC_LIB_CANDIDATES := $(RISCV_GLIBC_SYSROOT)/lib \
                              $(RISCV_GLIBC_SYSROOT)/lib64 \
                              /usr/riscv64-linux-gnu/lib
RISCV_GLIBC_LIB_DIR ?= $(patsubst %/ld-linux-riscv64-lp64d.so.1,%,$(firstword \
                         $(wildcard $(addsuffix /ld-linux-riscv64-lp64d.so.1,$(RISCV_GLIBC_LIB_CANDIDATES)))))
RISCV_GLIBC_LOCAL_ROOT ?= user/external/riscv64-glibc-sysroot
RISCV_GLIBC_LOCAL_LIB_DIR = $(RISCV_GLIBC_LOCAL_ROOT)/lib
FEDORA_RISCV_RELEASE ?=
USER_BUILD_ID = $(ARCH):$(NOMMU):$(USER_OPT):$(PROFILE)
USER_BUILD_DESKTOP = $(if $(filter benchmark,$(PROFILE)),0,1)
USER_BUILD_CHECK_DIRS = user/init.c user/cmds user/init_common user/desktop user/external/lvgl \
                        user/external/musl user/external/sbase user/external/mksh-cvs2git \
                        user/external/tlse user/external/fastfetch
NATIVE_TAG_riscv64     := rv
NATIVE_TAG_loongarch64 := la
NATIVE_TAG_aarch64     := aarch64
NATIVE_TAG_x86_64      := x86_64
NATIVE_TAG_arm32       := arm32
NATIVE_TAG_armv7m      := armv7m
NATIVE_TAG_riscv32     := rv32
NATIVE_TAG_ppc64le     := ppc64le
NATIVE_TAG             := $(NATIVE_TAG_$(ARCH))
NATIVE_BUILD_DIR       := $(USER_BUILD_DIR)
NATIVE_HELLO_BIN       := $(NATIVE_BUILD_DIR)/native-hello-$(NATIVE_TAG)
NATIVE_HANDLE_BIN      := $(NATIVE_BUILD_DIR)/native-handle-$(NATIVE_TAG)
NATIVE_LIBC_BIN        := $(NATIVE_BUILD_DIR)/native-libc-$(NATIVE_TAG)
NATIVE_OUTPUTS         := $(NATIVE_HELLO_BIN) $(NATIVE_HANDLE_BIN) $(NATIVE_LIBC_BIN)
NATIVE_BUILD_STAMP     := $(NATIVE_BUILD_DIR)/.native-build-id
comma := ,
NET_HOSTFWD ?= hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
NETDEV_USER = -netdev user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
SMOKE_TIMEOUT ?= 20s
SMOKE_INPUT_DELAY ?= 2
SMOKE_LOG_DIR ?= .kernel-build/smoke
STEP35_TIMEOUT ?= 300s
STEP35_INPUT_DELAY ?= 3
STEP35_LOG_DIR ?= .eval-state/2026/logs
WAIT_TIMER_HEAP_MAX ?=
REQUIRE_TIMEOUT_CAPACITY ?= 0
REQUIRE_SMP_RUNQUEUE ?= 0
REQUIRE_LOCK_SPLIT ?= 0
QEMU_MEMORY ?= 1G

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

RISCV_ELF_PREFIX ?= $(if $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null),riscv64-unknown-elf-,$(if $(shell command -v riscv64-elf-gcc 2>/dev/null),riscv64-elf-,riscv64-unknown-elf-))
RISCV_ELF_RV32_MULTIDIR := $(shell if command -v $(RISCV_ELF_PREFIX)gcc >/dev/null 2>&1; then \
	$(RISCV_ELF_PREFIX)gcc -march=rv32imafdc -mabi=ilp32d -print-multi-directory 2>/dev/null; \
fi)
CROSS_PREFIX_riscv64     := $(RISCV_ELF_PREFIX)
CROSS_PREFIX_loongarch64 := loongarch64-linux-gnu-
CROSS_PREFIX_aarch64     := aarch64-linux-gnu-
CROSS_PREFIX_x86_64      := x86_64-linux-gnu-
CROSS_PREFIX_arm32       := $(if $(shell command -v arm-linux-gnueabihf-gcc 2>/dev/null),arm-linux-gnueabihf-,$(if $(shell command -v arm-none-eabi-gcc 2>/dev/null),arm-none-eabi-,arm-linux-gnueabihf-))
CROSS_PREFIX_armv7m      := $(if $(shell command -v arm-none-eabi-gcc 2>/dev/null),arm-none-eabi-,)
CROSS_PREFIX_riscv32     := $(if $(filter-out .,$(RISCV_ELF_RV32_MULTIDIR)),$(RISCV_ELF_PREFIX),$(if $(shell command -v riscv32-linux-gnu-gcc 2>/dev/null),riscv32-linux-gnu-,riscv64-linux-gnu-))
CROSS_PREFIX_ppc64le     := powerpc64le-linux-gnu-

ARCH_CFLAGS_riscv64     := -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany
ARCH_CFLAGS_loongarch64 := -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
ARCH_CFLAGS_aarch64     := -march=armv8-a -mgeneral-regs-only -fno-pic -mcmodel=large -mno-outline-atomics
ARCH_CFLAGS_x86_64      := -m64 -mcmodel=large -mno-red-zone -fno-pic -fno-pie -mgeneral-regs-only
ARCH_CFLAGS_arm32       := -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -static -mno-unaligned-access
ARCH_CFLAGS_armv7m      := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-pic -static \
                           -ffunction-sections -fdata-sections -fno-unwind-tables \
                           -fno-asynchronous-unwind-tables
ARCH_CFLAGS_riscv32     := -march=rv32imafdc -mabi=ilp32d -mcmodel=medany -fno-pic -static
ARCH_CFLAGS_ppc64le     := -m64 -mcpu=power8 -mtune=power8 -mlong-double-64 -fno-pic -static -mno-vsx -mno-altivec -fno-tree-vectorize

PHYS_BASE_aarch64     := 0x40080000
PHYS_BASE_arm32       := 0x40080000
PHYS_BASE_armv7m      := 0x20000000
PHYS_BASE_ppc64le     := 0x00400000
PHYS_BASE_riscv32     := 0x80200000
PHYS_BASE_riscv64     := 0x80200000
PHYS_BASE_x86_64      := 0x00200000
PHYS_BASE_loongarch64 := 0x9000000000000000

ifeq ($(NOMMU),1)
CFLAGS += -DCONFIG_NOMMU
LDFLAGS_NOMMU := -Wl,--defsym=VIRT_BASE=$(PHYS_BASE_$(ARCH))
ARCH_CFLAGS_aarch64 += -mstrict-align
endif

ARCH_LDFLAGS_riscv64     :=
ARCH_LDFLAGS_loongarch64 := -static -no-pie
ARCH_LDFLAGS_aarch64     := -static -no-pie
ARCH_LDFLAGS_x86_64      := -static -no-pie
ARCH_LDFLAGS_arm32       := -static -no-pie
ARCH_LDFLAGS_armv7m      := -static -Wl,--gc-sections
ARCH_LDFLAGS_riscv32     := -static -no-pie -Wl,-m,elf32lriscv
ARCH_LDFLAGS_ppc64le     := -static -no-pie

ARCH_LIBS_riscv64     :=
ARCH_LIBS_loongarch64 :=
ARCH_LIBS_aarch64     :=
ARCH_LIBS_x86_64      :=
ARCH_LIBS_arm32       := $(shell $(CROSS_PREFIX_arm32)gcc $(ARCH_CFLAGS_arm32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_armv7m      :=
ARCH_LIBS_riscv32     := $(shell $(CROSS_PREFIX_riscv32)gcc $(ARCH_CFLAGS_riscv32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_ppc64le     :=

QEMU_riscv64     := qemu-system-riscv64
QEMU_loongarch64 := qemu-system-loongarch64
QEMU_aarch64     := qemu-system-aarch64
QEMU_x86_64      := qemu-system-x86_64
QEMU_arm32       := qemu-system-arm
QEMU_armv7m      := qemu-system-arm
QEMU_riscv32     := qemu-system-riscv32
QEMU_ppc64le     := qemu-system-ppc64
QEMU_GUI_DISPLAY ?= $(if $(filter Darwin,$(shell uname -s 2>/dev/null)),cocoa,gtk)

QEMU_FLAGS_BASE_riscv64     := -machine virt -bios default -global virtio-mmio.force-legacy=false
QEMU_FLAGS_BASE_loongarch64 := -machine virt
QEMU_FLAGS_BASE_aarch64     := -machine virt -cpu cortex-a57 -global virtio-mmio.force-legacy=false
QEMU_FLAGS_BASE_x86_64      := -machine q35 -no-reboot
QEMU_FLAGS_BASE_arm32       := -machine virt -cpu cortex-a15 -global virtio-mmio.force-legacy=false
QEMU_FLAGS_BASE_armv7m      := -machine stm32vldiscovery
QEMU_FLAGS_BASE_riscv32     := -machine virt -bios default -global virtio-mmio.force-legacy=false
QEMU_FLAGS_BASE_ppc64le     := -machine pseries

QEMU_BLK_riscv64     := virtio-blk-device,bus=virtio-mmio-bus.0
QEMU_BLK_loongarch64 := virtio-blk-pci
QEMU_BLK_aarch64     := virtio-blk-device,bus=virtio-mmio-bus.0
QEMU_BLK_x86_64      := virtio-blk-pci
QEMU_BLK_arm32       := virtio-blk-device,bus=virtio-mmio-bus.0
QEMU_BLK_riscv32     := virtio-blk-device,bus=virtio-mmio-bus.0
QEMU_BLK_ppc64le     := virtio-blk-pci

# A virtio-mmio bus accepts only one device.  Keep an optional second disk off
# the primary disk's bus; PCI transports can continue to use automatic slots.
QEMU_BLK_SECOND_riscv64     := virtio-blk-device,bus=virtio-mmio-bus.1
QEMU_BLK_SECOND_loongarch64 := virtio-blk-pci

QEMU_NET_riscv64     := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_loongarch64 := virtio-net-pci
QEMU_NET_aarch64     := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_x86_64      := virtio-net-pci
QEMU_NET_arm32       := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_riscv32     := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_ppc64le     := virtio-net-pci

QEMU_GUI_DEVICES_aarch64 := -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
                            -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
                            -device virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GUI_DEVICES_riscv64 := -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
                            -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
                            -device virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GUI_DEVICES_arm32 := -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
                          -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
                          -device virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GUI_DEVICES_x86_64 := -vga none \
                           -device virtio-gpu-pci \
                           -device virtio-keyboard-pci \
                           -device virtio-mouse-pci
QEMU_GUI_DEVICES_loongarch64 := -vga none \
                                -device virtio-gpu-pci \
                                -device virtio-keyboard-pci \
                                -device virtio-mouse-pci
QEMU_GUI_DEVICES_DEFAULT := -device virtio-gpu-device \
                            -device virtio-keyboard-device \
                            -device virtio-mouse-device

# Compiler and tools
CROSS_PREFIX := $(CROSS_PREFIX_$(ARCH))
ARCH_CFLAGS  := $(ARCH_CFLAGS_$(ARCH))
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH))
ARCH_LIBS    := $(ARCH_LIBS_$(ARCH))
QEMU         := $(QEMU_$(ARCH))
QEMU_FLAGS   := $(QEMU_FLAGS_BASE_$(ARCH)) -m $(QEMU_MEMORY) -nographic -smp $(NR_CPUS)
ifneq ($(NR_CPUS),1)
QEMU_FLAGS += -accel tcg,thread=multi
endif

QEMU_BLK     := $(QEMU_BLK_$(ARCH))
QEMU_BLK_SECOND := $(QEMU_BLK_SECOND_$(ARCH))
QEMU_NET     := $(QEMU_NET_$(ARCH))
QEMU_GUI_DEVICES := $(if $(QEMU_GUI_DEVICES_$(ARCH)),$(QEMU_GUI_DEVICES_$(ARCH)),$(QEMU_GUI_DEVICES_DEFAULT))

ifeq ($(ARCH),armv7m)
ifeq ($(CROSS_PREFIX),)
CLANG_ARMV7M := $(or $(shell command -v clang-19 2>/dev/null),$(shell command -v clang 2>/dev/null))
LLVM_OBJCOPY_ARMV7M := $(or $(shell command -v llvm-objcopy-19 2>/dev/null),$(shell command -v llvm-objcopy 2>/dev/null))
ifeq ($(CLANG_ARMV7M),)
$(error ARCH=armv7m requires arm-none-eabi-gcc or clang with the arm-none-eabi target)
endif
ifeq ($(LLVM_OBJCOPY_ARMV7M),)
$(error ARCH=armv7m with clang requires llvm-objcopy)
endif
CC := $(CLANG_ARMV7M) --target=arm-none-eabi
OBJCOPY := $(LLVM_OBJCOPY_ARMV7M)
ARCH_LDFLAGS += -fuse-ld=lld
else
CC := $(CROSS_PREFIX)gcc
OBJCOPY := $(CROSS_PREFIX)objcopy
endif
else
CC := $(CROSS_PREFIX)gcc
OBJCOPY := $(CROSS_PREFIX)objcopy
endif

ifeq ($(CC),)
$(error Unsupported ARCH '$(ARCH)')
endif

MKFS_FAT ?= $(or $(shell command -v mkfs.fat 2>/dev/null),$(wildcard /usr/sbin/mkfs.fat),$(wildcard /sbin/mkfs.fat),mkfs.fat)
MKFS_EXT4 ?= $(or $(shell command -v mkfs.ext4 2>/dev/null),$(wildcard /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4),$(wildcard /usr/local/opt/e2fsprogs/sbin/mkfs.ext4),$(wildcard /usr/sbin/mkfs.ext4),$(wildcard /sbin/mkfs.ext4),mkfs.ext4)
LIBGCC_S_ARCH := $(shell $(CC) $(ARCH_CFLAGS) -print-file-name=libgcc_s.so.1 2>/dev/null)
ifeq ($(LIBGCC_S_ARCH),libgcc_s.so.1)
LIBGCC_S_ARCH :=
endif

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device $(QEMU_BLK),drive=x0
QEMU_FLAGS += $(NETDEV_USER) -device $(QEMU_NET),netdev=net
# Extra-package runs need extra.img to be the ext4 filesystem mounted at /test,
# so snapshot the flags before an optional contest sdcard is appended.
QEMU_FLAGS_NO_SDCARD := $(QEMU_FLAGS)
ifeq ($(ARCH),riscv64)
ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x1 -device $(QEMU_BLK_SECOND),drive=x1
endif
endif
ifeq ($(ARCH),loongarch64)
ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x1 -device $(QEMU_BLK_SECOND),drive=x1
endif
endif
endif

# Compiler flags
CFLAGS = -Wall -Wextra $(OPT) -ffreestanding -nostdlib \
         -fno-builtin -fno-common -std=gnu99 \
         -MMD -MP \
         -I$(ARCH_INCLUDE_DIR) -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(KERNEL_DIR)/net/lwip_port \
         -I$(KERNEL_DIR)/external/lwip/src/include \
         -I$(BOARD_INCLUDE_DIR) -I$(BUILD_DIR)/generated $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_ABI_$(shell echo $(ABI) | tr a-z A-Z) \
          -DCONFIG_NR_CPUS=$(NR_CPUS) \
          -DCONFIG_BOARD_$(shell echo $(BOARD) | tr a-z A-Z | tr - _)
ifneq ($(strip $(WAIT_TIMER_HEAP_MAX)),)
CFLAGS += -DCONFIG_WAIT_TIMER_HEAP_MAX=$(WAIT_TIMER_HEAP_MAX)
endif
ifneq ($(NR_CPUS),1)
CFLAGS += -DCONFIG_SMP
endif
ifeq ($(COOPERATIVE_BOOT),1)
CFLAGS += -DCONFIG_AARCH64_COOPERATIVE_BOOT
endif
ifeq ($(BOARD),virtualbox-aarch64)
CFLAGS += -DCONFIG_AARCH64_COOPERATIVE_BOOT
endif
ELF_MACHINE_riscv64     := 243
ELF_MACHINE_loongarch64 := 258
ELF_MACHINE_aarch64     := 183
ELF_MACHINE_x86_64      := 62
ELF_MACHINE_arm32       := 40
ELF_MACHINE_riscv32     := 243
ELF_MACHINE_ppc64le     := 21
ELF_CLASS_riscv64       := 2
ELF_CLASS_loongarch64   := 2
ELF_CLASS_aarch64       := 2
ELF_CLASS_x86_64        := 2
ELF_CLASS_arm32         := 1
ELF_CLASS_riscv32       := 1
ELF_CLASS_ppc64le       := 2
ifneq ($(ARCH),armv7m)
CFLAGS += -DARCH_ELF_MACHINE=$(ELF_MACHINE_$(ARCH)) \
          -DARCH_ELF_CLASS=$(ELF_CLASS_$(ARCH))
endif
ifeq ($(ARCH),armv7m)
CFLAGS += -DSTM32_FLASH_KB=$(STM32_FLASH_KB) -DSTM32_RAM_KB=$(STM32_RAM_KB)
ifneq ($(shell printf '%s' '$(STM32_BT_NAME)' | LC_ALL=C grep -Eq '^[A-Za-z0-9_-]{1,32}$$' && echo yes),yes)
$(error STM32_BT_NAME must contain 1-32 ASCII letters, digits, '_' or '-')
endif
ifneq ($(shell printf '%s' '$(STM32_BT_PIN)' | LC_ALL=C grep -Eq '^[0-9]{4}$$' && echo yes),yes)
$(error STM32_BT_PIN must contain exactly four digits)
endif
ifneq ($(shell printf '%s' '$(STM32_BT_UUID)' | LC_ALL=C grep -Eq '^[0-9A-Fa-f]{4}$$' && echo yes),yes)
$(error STM32_BT_UUID must contain exactly four hexadecimal digits)
endif
ifeq ($(filter $(STM32_BT_BAUD),4800 9600 19200 38400 57600 115200),)
$(error STM32_BT_BAUD must be one of 4800, 9600, 19200, 38400, 57600, 115200)
endif
CFLAGS += -DCONFIG_STM32_RADIO_WIFI -DCONFIG_STM32_RADIO_BLUETOOTH
ifneq ($(strip $(STM32_WIFI_SSID)),)
ifneq ($(shell printf '%s' '$(STM32_WIFI_SSID)' | LC_ALL=C grep -Eq '^[A-Za-z0-9_.@-]{1,32}$$' && echo yes),yes)
$(error STM32_WIFI_SSID must be empty or contain 1-32 ASCII letters, digits, '.', '_', '@' or '-')
endif
endif
ifneq ($(strip $(STM32_WIFI_PASSWORD)),)
ifneq ($(shell printf '%s' '$(STM32_WIFI_PASSWORD)' | LC_ALL=C grep -Eq '^[A-Za-z0-9_.@-]{8,63}$$' && echo yes),yes)
$(error STM32_WIFI_PASSWORD must be empty or contain 8-63 ASCII letters, digits, '.', '_', '@' or '-')
endif
endif
ifeq ($(STM32_XUANWU),1)
CFLAGS += -DCONFIG_STM32_XUANWU
endif
ifeq ($(STM32_QEMU),1)
CFLAGS += -DCONFIG_STM32_QEMU
endif
endif
ifeq ($(filter $(ARCH),arm32 armv7m riscv32),)
CFLAGS += -DCONFIG_64BIT
else
CFLAGS += -DCONFIG_32BIT
endif
ifeq ($(ARCH),arm32)
# ARM32 supplies its own short-descriptor page-table backend.
else
CFLAGS += -DARCH_HAS_PGTABLE_OPS
endif
ifeq ($(ABI),both)
CFLAGS += -DCONFIG_ABI_NATIVE
endif

# Bringup / contest mode markers for conditional compilation.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif

ifeq ($(NOMMU),1)
CFLAGS += -DCONFIG_NOMMU
endif

# Synthetic driver lifecycle test (disabled by default).
ifeq ($(CONFIG_DRIVER_LIFECYCLE_TEST),y)
CFLAGS += -DCONFIG_DRIVER_LIFECYCLE_TEST
endif

ifeq ($(CONFIG_HDA_SMOKE_TEST),y)
CFLAGS += -DCONFIG_HDA_SMOKE_TEST
endif

ifeq ($(CONFIG_NVME_SMOKE_TEST),y)
CFLAGS += -DCONFIG_NVME_SMOKE_TEST
endif

ifeq ($(CONFIG_SWAP),y)
CFLAGS += -DCONFIG_SWAP
endif

BOARD_LDSCRIPT = $(KERNEL_DIR)/platform/$(BOARD)/ldscript.ld
ifeq ($(wildcard $(BOARD_LDSCRIPT)),)
LDSCRIPT = $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld
else
LDSCRIPT = $(BOARD_LDSCRIPT)
endif

LDFLAGS = -nostdlib -nostartfiles -Wl,--build-id=none -T $(LDSCRIPT) $(ARCH_LDFLAGS) $(LDFLAGS_NOMMU)
ifeq ($(ARCH),armv7m)
LDFLAGS += -Wl,--defsym=FLASH_LENGTH=$(STM32_FLASH_KB)K \
           -Wl,--defsym=RAM_LENGTH=$(STM32_RAM_KB)K
endif

# Source files
# ABI-specific source directories
ifeq ($(ABI),both)
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/linux/*.c) \
           $(wildcard $(KERNEL_DIR)/abi/native/*.c)
else
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/$(ABI)/*.c)
endif

ifeq ($(PROFILE),mcu)
CFLAGS += -DCONFIG_MCU -DCONFIG_KLOG_BUF_SIZE=256
KERNEL_SRC = $(KERNEL_DIR)/mcu/main.c \
             $(KERNEL_DIR)/mcu/uart.c \
             $(KERNEL_DIR)/mcu/heap.c \
             $(KERNEL_DIR)/mcu/mcu_stubs.c \
             $(KERNEL_DIR)/core/printf.c \
             $(KERNEL_DIR)/core/string.c \
             $(KERNEL_DIR)/core/panic.c \
             $(KERNEL_DIR)/core/sync.c \
             $(KERNEL_DIR)/core/klog.c \
             $(KERNEL_DIR)/core/timekeeping.c \
             $(KERNEL_DIR)/proc/sched.c \
             $(KERNEL_DIR)/proc/current.c \
             $(KERNEL_DIR)/proc/pid.c \
             $(KERNEL_DIR)/proc/proc.c \
             $(KERNEL_DIR)/proc/task.c \
             $(KERNEL_DIR)/proc/exit.c \
             $(KERNEL_DIR)/proc/signal.c \
             $(KERNEL_DIR)/proc/cg_cpu.c \
              $(KERNEL_DIR)/mm/nommu.c \
              $(KERNEL_DIR)/fs/fdtable.c \
	             $(KERNEL_DIR)/fs/fat32lite.c \
	             $(KERNEL_DIR)/fs/vfs.c \
	             $(wildcard $(BOARD_DRIVER_DIR)/*.c) \
	             $(wildcard $(KERNEL_DIR)/platform/$(BOARD)/*.c) \
             $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.c' | sort)
else
KERNEL_SRC = $(wildcard $(KERNEL_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/core/*.c) \
             $(filter-out $(KERNEL_DIR)/mm/nommu.c,$(wildcard $(KERNEL_DIR)/mm/*.c)) \
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
             $(wildcard $(KERNEL_DIR)/drivers/gpu/*.c) \
	             $(wildcard $(KERNEL_DIR)/drivers/audio/*.c) \
	             $(wildcard $(KERNEL_DIR)/drivers/input/*.c) \
	             $(wildcard $(BOARD_DRIVER_DIR)/*.c) \
	             $(wildcard $(KERNEL_DIR)/platform/$(BOARD)/*.c) \
             $(ABI_SRCS) \
             $(wildcard $(KERNEL_DIR)/syscall/*.c) \
             $(wildcard $(KERNEL_DIR)/shell/*.c) \
             $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.c' | sort) \
             $(LWIP_SRC)

ifeq ($(NOMMU),1)
KERNEL_SRC += $(KERNEL_DIR)/mm/nommu.c
endif

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
VBOX_AARCH64_EFI = $(BUILD_DIR)/BOOTAA64.EFI
VBOX_AARCH64_IMG = $(BUILD_DIR)/a20os-vbox-aarch64.img
VBOX_AARCH64_TEXT_IMG = $(BUILD_DIR)/a20os-vbox-aarch64-text.img
VBOX_AARCH64_LOAD_ADDRESS ?= 0x08080000ULL

# ================================================================
# Targets
# ================================================================

.PHONY: all all-architectures clean run-riscv64 run-gui-riscv64 run-gui-rv run-loongarch64 run-gui-loongarch64 run-gui-la run-arm64 run-gui-arm64 run-gui-aarch64 run-x86_64 run-gui-x86_64 vbox-iso-x86_64 _vbox_iso_x86_64_impl vbox-image-aarch64 _vbox_image_aarch64_impl vbox-text-image-aarch64 _vbox_text_image_aarch64_impl run-arm32 run-gui-arm32 run-riscv32 run-ppc64le debug-riscv64 debug-loongarch64 debug-arm64 debug-x86_64 debug-arm32 debug-riscv32 debug-ppc64le \
		run-gui-nommu-arm32 run-nommu-gui-arm32 \
		stm32f103-bringup stm32f103-xuanwu flash-stm32f103-xuanwu run-stm32f103-qemu \
		check-stm32f103 \
		check-kernel-build check-kernel-build-all check-user-build check-user-build-all check-dev-build check-contest-build check-contest-build-all check-build-matrix check-build-matrix-all check-abi-smoke-gate check-doc-drift check-doc-test-gates check-final-definition check-concurrency-foundation check-mm-lock-model check-abi-boundary check-driver-core-model check-external-dependency-boundary \
		check-arch-boundary check-task-state-boundary \
		check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup check-arm32-bringup check-riscv32-bringup check-ppc64le-bringup \
		check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user check-arm32-user check-riscv32-user check-ppc64le-user \
		smoke-riscv64 smoke-loongarch64 smoke-aarch64 smoke-x86_64 smoke-qemu-gui-x86_64 smoke-qemu-gui-riscv64 smoke-qemu-gui-aarch64 smoke-qemu-gui-arm32 smoke-qemu-gui-loongarch64 smoke-arm32 smoke-riscv32 smoke-ppc64le smoke-abi-linux smoke-network-suite smoke-proc-a20 smoke-proc-stress smoke-mm-stress smoke-vfs-stress smoke-vfs-edge smoke-sched-stress smoke-futex-stress smoke-socket-stress smoke-driver-lifecycle smoke-hda smoke-pci-portability smoke-native-handle smoke-native-libc smoke-io-event \
		smoke-arch-mmu-matrix \
		FORCE regen-rootfs-overlay \
		user_apps fs_img kernel-only dev-build contest-rv contest-la \
		eval-dev-build-rv eval-dev-build-la \
		qemu-disk-rv qemu-disk-la \
		extra-img _extra-img extra-user-apps prepare-riscv64-glibc-sysroot force_extra_image_stamp run-riscv64-extra run-loongarch64-extra run-arm64-extra run-x86_64-extra run-arm32-extra run-riscv32-extra run-ppc64le-extra \
		native-test-arch native-handle-test-arch native-libc-arch native-programs \
		native-test-rv native-test-la native-test-aarch64 native-test-x86_64 native-test-arm32 native-test-rv32 native-test-ppc64le native-test native-test-all \
		native-minimal-rv native-minimal-la native-minimal \
		native-handle-test-rv native-handle-test-la native-handle-test-aarch64 native-handle-test-x86_64 native-handle-test-arm32 native-handle-test-rv32 native-handle-test-ppc64le native-handle-test native-handle-test-all \
		native-libc-rv native-libc-la native-libc-aarch64 native-libc-x86_64 native-libc-arm32 native-libc-rv32 native-libc-ppc64le native-libc native-libc-all \
		eval eval-all eval-rv eval-la \
		final-eval-rv-cagent final-eval-la-cagent \
		final-eval-rv-buildstorm final-eval-la-buildstorm

FORCE:

$(BUILD_TIME_HDR):
	@mkdir -p $(dir $@)
	@printf '#ifndef A20_BUILD_UNIX_TIME\n#define A20_BUILD_UNIX_TIME %sULL\n#endif\n' "$$(date -u +%s)" > $@

$(STM32_BT_CONFIG_HDR): FORCE
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#ifndef GENERATED_STM32_BLUETOOTH_CONFIG_H'; \
		printf '%s\n' '#define GENERATED_STM32_BLUETOOTH_CONFIG_H'; \
		printf '#define STM32_BLUETOOTH_DEVICE_NAME "%s"\n' '$(STM32_BT_NAME)'; \
		printf '#define STM32_BLUETOOTH_PIN "%s"\n' '$(STM32_BT_PIN)'; \
		printf '#define STM32_BLUETOOTH_SERVICE_UUID 0x%sU\n' '$(STM32_BT_UUID)'; \
		printf '#define STM32_BLUETOOTH_SERVICE_UUID_TEXT "%s"\n' '$(STM32_BT_UUID)'; \
		printf '#define STM32_BLUETOOTH_BAUD_RATE %sU\n' '$(STM32_BT_BAUD)'; \
		printf '#define STM32_BLUETOOTH_BAUD_RATE_TEXT "%s"\n' '$(STM32_BT_BAUD)'; \
		printf '%s\n' '#endif'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(STM32_WIFI_CONFIG_HDR): FORCE
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#ifndef GENERATED_STM32_WIFI_CONFIG_H'; \
		printf '%s\n' '#define GENERATED_STM32_WIFI_CONFIG_H'; \
		printf '#define STM32_WIFI_SSID "%s"\n' '$(STM32_WIFI_SSID)'; \
		printf '#define STM32_WIFI_PASSWORD "%s"\n' '$(STM32_WIFI_PASSWORD)'; \
		printf '%s\n' '#endif'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

regen-rootfs-overlay: scripts/gen_rootfs_overlay.py $(ROOTFS_OVERLAY_FILES)
	@mkdir -p $(dir $(ROOTFS_OVERLAY_SRC)) $(dir $(ROOTFS_OVERLAY_HDR))
	$(PYTHON) $< --out-c $(ROOTFS_OVERLAY_SRC) --out-h $(ROOTFS_OVERLAY_HDR) --root $(ROOTFS_OVERLAY_DIR)

$(ROOTFS_OVERLAY_SRC) $(ROOTFS_OVERLAY_HDR): scripts/gen_rootfs_overlay.py $(ROOTFS_OVERLAY_FILES)
	$(PYTHON) $< --out-c $(ROOTFS_OVERLAY_SRC) --out-h $(ROOTFS_OVERLAY_HDR) --root $(ROOTFS_OVERLAY_DIR)

# ----------------------------------------------------------------
# Competition build. Linux produces both contest architectures; macOS builds
# the supported RISC-V artifacts. Use all-architectures for the judge matrix.
# ----------------------------------------------------------------
all:
	@set -e; for target in $(DEFAULT_CONTEST_TARGETS); do $(MAKE) $$target; done
	@echo "=== Competition build complete ==="
	@echo "  built: $(DEFAULT_CONTEST_TARGETS)"

all-architectures:
	$(MAKE) contest-rv
	$(MAKE) contest-la
	@echo "=== Full competition build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

check-kernel-build: $(DEFAULT_KERNEL_CHECK_TARGETS)

check-kernel-build-all: check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup check-arm32-bringup check-riscv32-bringup check-ppc64le-bringup

check-riscv64-bringup:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only

check-loongarch64-bringup:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-aarch64-bringup:
	$(MAKE) ARCH=aarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-x86_64-bringup:
	$(MAKE) ARCH=x86_64 ABI=$(ABI) BRINGUP=1 kernel-only

check-arm32-bringup:
	$(MAKE) ARCH=arm32 ABI=$(ABI) BRINGUP=1 kernel-only

check-riscv32-bringup:
	$(MAKE) ARCH=riscv32 ABI=$(ABI) BRINGUP=1 kernel-only

check-ppc64le-bringup:
	$(MAKE) ARCH=ppc64le ABI=$(ABI) BRINGUP=1 kernel-only

check-user-build: $(DEFAULT_USER_CHECK_TARGETS)

check-user-build-all: check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user check-arm32-user check-riscv32-user check-ppc64le-user

check-build-matrix: check-kernel-build check-user-build
	@rg -q "BUILD_MATRIX_GATE_CONTRACT" docs/testing/testing-gates.md
	@echo "check-build-matrix: PASS"

check-build-matrix-all: check-kernel-build-all check-user-build-all
	@rg -q "BUILD_MATRIX_GATE_CONTRACT" docs/testing/testing-gates.md
	@echo "check-build-matrix-all: PASS"

check-arch-boundary:
	@! rg -n '#if(n?def)?[[:space:]]+(CONFIG_|__)(AARCH64|ARM|RISCV|LOONG|X86|PPC)|CONFIG_ARM32|CONFIG_AARCH64|__aarch64__|__arm__' \
		kernel --glob '!kernel/arch/**' --glob '!kernel/platform/**' \
		--glob '!kernel/external/**' --glob '!kernel/include/core/arch.h'
	@rg -q "ARCH_MMU_RUNTIME_MATRIX_CONTRACT" docs/testing/testing-gates.md
	@rg -q "smoke-arch-mmu-matrix" Makefile docs/OS-Design.md
	@for arch in loongarch64 x86_64 ppc64le; do \
		if $(MAKE) -s ARCH=$$arch NOMMU=1 kernel-only >/dev/null 2>&1; then \
			echo "check-arch-boundary: unsupported NOMMU build accepted for $$arch"; \
			exit 1; \
		fi; \
	done
	@echo "check-arch-boundary: PASS"

check-task-state-boundary:
	@! rg -n --pcre2 --glob '*.c' --glob '!kernel/external/**' \
		--glob '!kernel/proc/park.c' --glob '!kernel/proc/sched.c' \
		--glob '!kernel/proc/exit.c' --glob '!kernel/proc/task.c' \
		-- '->state[[:space:]]*=[[:space:]]*PROC_' kernel
	@! rg -n --pcre2 --glob '*.c' --glob '!kernel/external/**' \
		--glob '!kernel/proc/sched.c' --glob '!kernel/proc/current.c' \
		--glob '!kernel/proc/task.c' \
		-- '->(on_rq|dispatching|on_cpu|owner_cpu|rq_next|rq_prev)[[:space:]]*=' kernel
	@! rg -n 'proc_runq_(enqueue|remove)_locked[[:space:]]*\(' kernel \
		--glob '*.c' --glob '!kernel/proc/park.c' \
		--glob '!kernel/proc/current.c' \
		--glob '!kernel/proc/sched.c' --glob '!kernel/proc/exit.c' \
		--glob '!kernel/proc/task.c'
	@! rg -n --pcre2 'task_t[[:space:]]*\*[[:space:]]*(waiter|rx_waiter)\b' \
		kernel --glob '*.[ch]' --glob '!kernel/external/**'
	@! rg -n --pcre2 '\bproc_find[[:space:]]*\(' kernel \
		--glob '*.[ch]' --glob '!kernel/external/**'
	@rg -q 'A20_PARK_WAKE_PROTOCOL' kernel/include/proc/park.h
	@rg -q 'WAIT_QUEUE_PARK_PROTOCOL' kernel/include/core/sync.h
	@rg -q 'TASK_REFERENCE_LIFETIME' kernel/include/proc/proc.h
	@rg -q 'proc_get\(token\.task\)' kernel/core/sync.c
	@rg -q '_Static_assert\(offsetof\(task_t, kstack\) == 0' kernel/include/proc/proc.h
	@rg -q '_Static_assert\(offsetof\(task_t, kstack_base\) == sizeof\(uintptr_t\)' kernel/include/proc/proc.h
	@echo "check-task-state-boundary: PASS"

check-smp-platform-boundary:
	@rg -q "typedef struct smp_platform_ops" kernel/include/core/smp.h
	@rg -q "const smp_platform_ops_t \*smp" kernel/drivers/core/driver_core.h
	@files="kernel/arch/riscv64/platform/smp.c kernel/arch/aarch64/platform/smp.c kernel/arch/x86_64/platform/smp.c kernel/arch/loongarch64/platform/smp.c"; \
		test -r kernel/arch/riscv64/platform/smp.c && \
		test -r kernel/arch/aarch64/platform/smp.c && \
		test -r kernel/arch/x86_64/platform/smp.c && \
		test -r kernel/arch/loongarch64/platform/smp.c && \
		if rg -n "CONFIG_BOARD|firmware_cpu_on|sbi_hart_start|firmware_acpi_apic_ids|IOCSR_MBUF" $$files; then exit 1; fi
	@! rg -n "void smp_(init|boot_secondaries|send_reschedule|secondary_init)\\(" kernel/arch
	@for board in qemu-virt-riscv64 qemu-virt-aarch64 qemu-virt-loongarch64 qemu-virt-x86_64; do \
		rg -q "\\.smp[[:space:]]*=" "kernel/platform/$$board/board.c" || exit 1; \
	done
	@echo "check-smp-platform-boundary: PASS"

check-abi-smoke-gate:
	@rg -q "ABI_SMOKE_GATE_CONTRACT" docs/testing/testing-gates.md
	@rg -q "syscall_smoke" Makefile
	@rg -q "smoke-abi-linux" Makefile
	@rg -q "native-minimal" Makefile
	@rg -q "native-test" Makefile
	@rg -q "test_liba20c" user/tests/test_liba20c.c Makefile
	@echo "check-abi-smoke-gate: PASS"

check-doc-drift:
	@rg -q "DOC_DRIFT_KEYWORD_GATE" docs/testing/testing-gates.md
	@$(PYTHON) scripts/gen_linux_syscall_coverage.py
	@rg -q "stub" kernel/abi/linux/syscall_coverage.md kernel/abi/linux/compat_notes.md docs/testing/testing-gates.md
	@rg -q "partial" kernel/abi/linux/syscall_coverage.md kernel/abi/linux/compat_notes.md docs/testing/testing-gates.md
	@rg -q "Future" docs/testing/testing-gates.md kernel/abi/native/sys_core.c
	@rg -q "not yet" docs/testing/testing-gates.md kernel/abi/native/sys_phase2.c kernel/mm/fault.c
	@! rg -q "for simplicity" docs kernel --glob '!docs/research/**' --glob '!docs/testing/testing-gates.md' --glob '!kernel/external/**'
	@echo "check-doc-drift: PASS"

check-task-lifetime-boundary:
	@rg -q "STEP35_TASK_LIFETIME_DIAGNOSTICS" kernel/include/proc/lifetime.h
	@rg -q "proc_lifetime_note_task_init" kernel/proc/task.c
	@rg -q "proc_lifetime_note_pid_add" kernel/proc/pid.c
	@rg -q "proc_lifetime_note_wait_to_wake" kernel/core/sync.c
	@rg -q "proc_lifetime_note_wake_remove" kernel/proc/park.c
	@rg -q "proc_wait_timer_count_locked" kernel/proc/sched.c
	@rg -q "proc_current_lifetime_violations_locked" kernel/proc/current.c
	@rg -q "PF_A20_TASK_LIFETIME" kernel/fs/procfs.c
	@rg -q "TASK_LIFETIME_OWNERSHIP_AUDIT" docs/testing/task-lifetime-audit.md
	@! rg -n --pcre2 '\bproc_find[[:space:]]*\(' kernel \
		--glob '*.[ch]' --glob '!kernel/external/**'
	@echo "check-task-lifetime-boundary: PASS"

check-blocking-point-boundary:
	@rg -q "BLOCKING_POINT_PROTOCOL_AUDIT" docs/testing/blocking-point-audit.md
	@rg -q "PROC_BLOCKED_ALLOCATION_WHITELIST" kernel/proc/task.c
	@rg -q "FUTEX_WAIT_RECHECK_PROTOCOL" kernel/abi/linux/sys_futex.c
	@rg -q "proc_try_wake_locked\(t, t->wait_seq, PROC_WAKE_EVENT\)" kernel/proc/sched.c
	@! rg -n --pcre2 '\bproc_block_until[[:space:]]*\(' kernel \
		--glob '*.[ch]' --glob '!kernel/external/**'
	@bad=$$(rg -n --pcre2 '(?:->|\.)state[[:space:]]*=[[:space:]]*PROC_BLOCKED' \
		kernel --glob '*.[ch]' --glob '!kernel/external/**' | \
		rg -v '^kernel/proc/(task|park)\.c:' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@bad=$$(rg -n --pcre2 '(?:->|\.)(?:on_rq|cpu_id|rq_next|rq_prev)[[:space:]]*=' \
		kernel --glob '*.[ch]' --glob '!kernel/external/**' | \
		rg -v '^kernel/proc/(task|sched)\.c:' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@bad=$$(rg -n --pcre2 '\bproc_make_ready[[:space:]]*\(' kernel \
		--glob '*.c' --glob '!kernel/external/**' | \
		rg -v '^kernel/(proc/(fork|proc|sched|cg_cpu)\.c|abi/native/sys_core\.c):' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@rg -Uq 'typedef struct wait_queue_entry[^{]*\{[^}]*task[^}]*wait_seq' kernel/include/core/sync.h
	@rg -Uq 'typedef struct futex_waiter[^{]*\{[^}]*task[^}]*wait_seq' kernel/abi/linux/sys_futex.c
	@rg -Uq 'typedef struct wait_timer[^{]*\{[^}]*task[^}]*wait_seq' kernel/proc/sched.c
	@rg -Uq 'typedef struct proc_wake_q_item[^{]*\{[^}]*task[^}]*seq' kernel/include/proc/park.h
	@rg -q "FUTEX_STRESS: unrelated-wake-isolation PASS" user/cmds/futex_stress.c
	@rg -q "PROC_STRESS: vfork-auto-reap PASS" user/cmds/proc_stress.c
	@echo "check-blocking-point-boundary: PASS"

check-signal-exit-boundary:
	@rg -q "SIGNAL_EXIT_PROTOCOL_AUDIT" docs/testing/signal-exit-audit.md
	@rg -q "SIGNAL_STATE_LOCK_CONTRACT" kernel/include/proc/signal.h
	@rg -q "PARK_SIGNAL_MODE_PROTOCOL" kernel/proc/park.c
	@rg -q "SIGNAL_MASK_PARK_PROTOCOL" kernel/abi/linux/sys_signal.c
	@rg -q "REMOTE_EXIT_SAFE_BOUNDARY" kernel/proc/exit.c
	@rg -q "PROC_WAKE_FATAL_SIGNAL" kernel/include/proc/park.h kernel/proc/park.c
	@rg -q "PROC_WAKE_TASK_EXIT" kernel/include/proc/park.h kernel/proc/exit.c
	@! rg -n --pcre2 '\bproc_make_ready[[:space:]]*\(' \
		kernel/proc/signal.c kernel/proc/exit.c
	@! rg -n --pcre2 'reason[[:space:]]*==[[:space:]]*PROC_WAKE_SIGNAL' \
		kernel --glob '*.c' --glob '!kernel/external/**' \
		--glob '!kernel/proc/park.c'
	@bad=$$(rg -n --pcre2 \
		'(?:->|\.)(?:sig_blocked|thread_pending|sigsuspend_old_blocked|sigsuspend_active|sigwait_mask|sigwait_active)\b|(?:ss|signal_state)->(?:pending|pending_has_info|pending_info|actions)\b' \
		kernel --glob '*.c' --glob '!kernel/external/**' | \
		rg -v '^kernel/(proc/(signal|task)\.c|abi/linux/sys_signal\.c):' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@rg -Uq 'eventfd_read[\s\S]*proc_park_prepare\(PROC_WAIT_INTERRUPTIBLE' \
		kernel/ipc/eventfd.c
	@rg -Uq 'timerfd_read[\s\S]*proc_park_prepare\(PROC_WAIT_INTERRUPTIBLE' \
		kernel/ipc/timerfd.c
	@rg -q "PROC_STRESS: signal-stop-exit PASS" user/cmds/proc_stress.c
	@rg -q "PROC_STRESS: signal-mask-park PASS" user/cmds/proc_stress.c
	@echo "check-signal-exit-boundary: PASS"

check-timeout-ownership-boundary:
	@rg -q "TIMEOUT_OWNERSHIP_AUDIT" docs/testing/timeout-ownership-audit.md
	@rg -Uq 'typedef struct wait_timer[^{]*\{[^}]*deadline[^}]*task[^}]*wait_seq' \
		kernel/proc/sched.c
	@rg -q "PROC_PARK_PREPARE_TIMEOUT_CAPACITY" \
		kernel/include/proc/park.h kernel/proc/park.c kernel/proc/sched.c
	@rg -q "wait_timer_duplicate_rejections" kernel/proc/sched.c
	@rg -Uq 'proc_try_wake_locked\(timer\.task,[[:space:]]*timer\.wait_seq' \
		kernel/proc/sched.c
	@rg -q "timeout_heap_violations" \
		kernel/include/proc/lifetime.h kernel/proc/lifetime.c
	@rg -Fq "timeout-capacity+1 PASS" user/cmds/lifetime_stress.c
	@rg -q "FUTEX_STRESS: stale-timeout-isolation PASS" \
		user/cmds/futex_stress.c
	@! rg -U --pcre2 \
		'wait_timer_count[\s\S]{0,500}PROC_READY' kernel/proc/sched.c
	@echo "check-timeout-ownership-boundary: PASS"

check-smp-runqueue-boundary:
	@rg -q "SMP_RUNQUEUE_PREEMPT_AUDIT" \
		docs/testing/smp-runqueue-audit.md
	@rg -q "SMP_RUNQUEUE_PREEMPT_PROTOCOL" kernel/include/proc/proc.h
	@rg -q "SMP_RUNQUEUE_MIGRATION_PROTOCOL" kernel/proc/sched.c
	@rg -q "need_resched" kernel/proc/sched.c
	@rg -Uq 'first = src_cpu < dst_cpu[\s\S]*RUNQ_LOCK_IRQ\(first\)[\s\S]*RUNQ_LOCK_IRQ\(second\)' \
		kernel/proc/sched.c
	@rg -q "proc_sched_safe_point" kernel/core/trap.c
	@rg -q "proc_sched_tick" \
		kernel/arch/riscv64/trap/irqchip.c \
		kernel/arch/loongarch64/trap/irqchip.c
	@rg -q "proc_sched_handle_reschedule_ipi" \
		kernel/arch/riscv64/trap/irqchip.c \
		kernel/arch/loongarch64/platform/smp.c
	@! rg -n 'proc_yield' \
		kernel/arch/riscv64/trap/irqchip.c \
		kernel/arch/loongarch64/platform/smp.c \
		kernel/arch/aarch64/trap/irqchip.c \
		kernel/arch/x86_64/trap/irqchip.c
	@rg -q "scheduler_violations" \
		kernel/include/proc/lifetime.h kernel/proc/lifetime.c
	@rg -q "SCHED_STRESS: smp-runqueue PASS" user/cmds/sched_stress.c
	@echo "check-smp-runqueue-boundary: PASS"

check-process-lock-split-boundary:
	@rg -q "PROCESS_LOCK_SPLIT_AUDIT" \
		docs/testing/process-lock-split-audit.md
	@rg -q "SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN" kernel/proc/sched.c
	@rg -Uq 'task_t \*next = proc_runq_pick_local\(\);[[:space:]]*uint64_t flags = spin_lock_irqsave\(&proc_lock\)' \
		kernel/proc/sched.c
	@! rg -U --pcre2 \
		'SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN(?:(?!SCHED_LOCAL_PICK_LOCK_SPLIT_END)[\s\S])*spin_lock_irqsave\(&proc_lock\)' \
		kernel/proc/sched.c
	@! rg -n 'proc_runq_pick_locked' kernel/proc kernel/include/proc
	@rg -q "runqueue_parallel_pick_peak" \
		kernel/include/proc/lifetime.h kernel/proc/lifetime.c
	@rg -q "SCHED_STRESS: lock-split PASS" user/cmds/sched_stress.c
	@echo "check-process-lock-split-boundary: PASS"

check-doc-test-gates: check-concurrency-foundation check-smp-platform-boundary check-task-state-boundary check-task-lifetime-boundary check-blocking-point-boundary check-signal-exit-boundary check-timeout-ownership-boundary check-smp-runqueue-boundary check-process-lock-split-boundary check-mm-lock-model check-io-progress-model check-vfs-abstraction check-abi-boundary check-driver-core-model check-external-dependency-boundary check-abi-smoke-gate check-doc-drift
	@rg -q "DOCS_AS_FACT_CONTRACT" docs/testing/testing-gates.md
	@rg -q "TEST_FIRST_ARCHITECTURE_MATRIX" docs/testing/testing-gates.md
	@echo "check-doc-test-gates: PASS"

check-final-definition: check-doc-test-gates
	@rg -q "MM_LOCK_MODEL" kernel/include/mm/vm.h
	@rg -q "TASK_STATE_MUTATION_CONTRACT" kernel/include/proc/proc.h
	@rg -q "TASK_REFERENCE_LIFETIME" kernel/include/proc/proc.h
	@rg -q "VFS_REFCOUNT_HELPER_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX" kernel/abi/native/handle_table.h
	@rg -q "KERNEL_PROGRESS_SERVICE_CONTRACT" kernel/include/core/progress.h
	@rg -q "VFS_OPEN_DISPATCH_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "LINUX_ABI_EXPLICIT_STUB_CONTRACT" kernel/abi/linux/syscall_table.def
	@rg -q "NATIVE_DEBUG_LIMITED_CONTRACT" kernel/abi/native/sys_phase2.c
	@rg -q "DRIVER_CORE_CONCURRENCY_MODEL" kernel/drivers/core/driver_core.c
	@rg -q "EXTERNAL_USERLAND_UPGRADE_CHECKLIST" docs/project/external-dependencies.md
	@echo "check-final-definition: PASS (SMP smoke tracked separately by TODO section 10)"

check-riscv64-user:
	$(MAKE) -C user ARCH=riscv64 OPT="$(USER_OPT)"

check-loongarch64-user:
	$(MAKE) -C user ARCH=loongarch64 OPT="$(USER_OPT)"

check-aarch64-user:
	$(MAKE) -C user ARCH=aarch64 OPT="$(USER_OPT)"

check-x86_64-user:
	$(MAKE) -C user ARCH=x86_64 OPT="$(USER_OPT)"

check-arm32-user:
	$(MAKE) -C user ARCH=arm32 OPT="$(USER_OPT)"

check-riscv32-user:
	$(MAKE) -C user ARCH=riscv32 OPT="$(USER_OPT)"

check-ppc64le-user:
	$(MAKE) -C user ARCH=ppc64le OPT="$(USER_OPT)"

check-dev-build:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=0 dev-build

check-contest-build:
	$(MAKE) all

check-contest-build-all:
	$(MAKE) all-architectures

check-concurrency-foundation:
	@rg -q "SCHEDULER_CONCURRENCY_PREREQS" kernel/proc/sched.c
	@rg -q "SCHEDULER_CPU_OWNERSHIP" kernel/proc/sched.c
	@rg -q "PER_CPU_CURRENT_VALIDATION" kernel/proc/current.c
	@rg -q "TASK_STATE_MUTATION_CONTRACT" kernel/include/proc/proc.h
	@rg -q "A20_PARK_WAKE_PROTOCOL" kernel/include/proc/park.h
	@rg -q "WAIT_QUEUE_PARK_PROTOCOL" kernel/include/core/sync.h
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
	@$(PYTHON) scripts/gen_linux_syscall_coverage.py
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
	@rg -q "mutex_lock\(&g_driver_core_ops\)" kernel/drivers/core/driver_core.c
	@rg -q -- "return -EEXIST" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_CORE_DYNAMIC_LIMITS" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_IRQ_TABLE_FIXED_LIMIT" kernel/drivers/core/driver_hwapi.c
	@rg -q "irq_table_lock" kernel/drivers/core/driver_hwapi.c
	@rg -q "irq_active" kernel/drivers/core/driver_hwapi.c
	@rg -q "DRIVER_PROBE_FAILURE_CLEANUP" kernel/drivers/core/driver_core.c
	@rg -q "dev->drv = NULL" kernel/drivers/core/driver_core.c
	@rg -q "dev->state = DEV_STATE_UNINIT" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_ENUMERATION_FAILURE_MODEL" kernel/drivers/bus/virtio_mmio_bus.c kernel/drivers/bus/pci_bus.c
	@rg -q "DRIVER_IRQ_DMA_SEMANTICS" kernel/drivers/core/driver_hwapi.h
	@rg -q "DRIVER_SMOKE_MATRIX" kernel/drivers/core/driver_core.h
	@rg -q "class_device_publish" kernel/drivers/core/driver_core.c kernel/drivers/core/driver_class.c
	@rg -q "class_device_unpublish" kernel/drivers/core/driver_core.c kernel/drivers/core/driver_class.c
	@rg -q "DEV_STATE_REMOVING" kernel/drivers/core/driver_core.h kernel/drivers/core/driver_core.c
	@rg -q "platform_device_register" kernel/drivers/bus/platform_bus.c kernel/platform/qemu-virt-x86_64/board.c
	@rg -q "DEV_CLASS_AUDIO" kernel/drivers/core/driver_core.h kernel/drivers/audio/pc_speaker.c
	@rg -Fq "pci_class_code(dev) != 0x040300" kernel/drivers/audio/hda.c
	@rg -Fq "pci_class_code(dev) != 0x010802" kernel/drivers/block/nvme.c
	@rg -Fq "if (!size && bar_lo == 0)" kernel/drivers/bus/pci_bus.c
	@rg -q "\.match = hda_match" kernel/drivers/audio/hda.c
	@rg -q "\.match = nvme_match" kernel/drivers/block/nvme.c
	@rg -q "NVME_IO_SMOKE: PASS" kernel/drivers/block/nvme.c Makefile
	@! rg -q "CONFIG_X86_64" kernel/drivers/audio/hda.c kernel/drivers/block/nvme.c
	@rg -q "CONFIG_X86_64" kernel/drivers/audio/pc_speaker.c
	@rg -q "virtio_blk_driver_probe" kernel/drivers/block/virtio_blk.c
	@rg -q "virtio_net_driver_probe" kernel/drivers/net/virtio_net.c
	@rg -q "uart_driver_probe" kernel/drivers/char/uart.c
	@rg -q "pty_init" kernel/drivers/char/pty.c
	@rg -q "loop_init" kernel/drivers/block/loop.c
	@rg -q "pci_enumerate" kernel/drivers/bus/pci_bus.c
	@rg -q "virtio_mmio_enumerate" kernel/drivers/bus/virtio_mmio_bus.c
	@rg -q "kernel/drivers/" docs/drivers/README.md
	@rg -q "kernel/platform/" docs/drivers/README.md
	@rg -q "DRIVER_LIFECYCLE_TEST" kernel/drivers/core/driver_lifecycle_test.c kernel/drivers/core/driver_lifecycle_test.h kernel/main.c
	@rg -q "driver_lifecycle_test_run" kernel/main.c kernel/fs/procfs.c kernel/drivers/core/driver_lifecycle_test.c
	@rg -q "duplicate driver registration" kernel/drivers/core/driver_lifecycle_test.c
	@! rg -q "virtio_gpu_init\(\)|virtio_input_init\(\)" kernel/main.c
	@! rg -q "virtio_(blk|net)_init\(\)" kernel/main.c kernel/net/socket.c
	@! rg -q "arch_virtio_(gpu|input)_probe" kernel
	@rg -q "唯一枚举所有权" docs/drivers/core-model.md
	@! rg -q "kernel/driver/|kernel/drv/|kernel/board/|#include \"driver/" docs/drivers/*.md
	@echo "check-driver-core-model: PASS"

check-external-dependency-boundary:
	@rg -q "include kernel/external/lwip/sources.mk" Makefile
	@rg -q "EXTERNAL_LWIP_SOURCE_MANIFEST" docs/project/external-dependencies.md
	@rg -q "LWIP_SRC" kernel/external/lwip/sources.mk
	@rg -q "core/timeouts.c" kernel/external/lwip/sources.mk
	@rg -q "EXTERNAL_LWIP_CONFIG_CONTRACT" docs/project/external-dependencies.md
	@rg -q "NO_SYS=1" docs/project/external-dependencies.md
	@rg -q "g_lwip_lock" docs/project/external-dependencies.md kernel/net/lwip_stack.c
	@rg -q "a20_lwip_poll\(\)" docs/project/external-dependencies.md
	@rg -q "kernel_progress_poll\(\)" docs/project/external-dependencies.md
	@rg -q "EXTERNAL_QEMU_NET_DEFAULTS" docs/project/external-dependencies.md
	@rg -q "10\.0\.2\.15" docs/project/external-dependencies.md kernel/net/lwip_stack.c
	@rg -q "EXTERNAL_USERLAND_UPGRADE_CHECKLIST" docs/project/external-dependencies.md
	@rg -q "syscall smoke, shell smoke, and coreutils smoke" docs/project/external-dependencies.md
	@rg -q "EXTERNAL_STATIC_LINK_REBUILD_CONTRACT" docs/project/external-dependencies.md
	@rg -q "user/build/<arch>\\[-nommu\\]/\\.build-id" docs/project/external-dependencies.md
	@rg -q "EXTERNAL_TLSE_WGET_LIMITS" docs/project/external-dependencies.md
	@rg -q "TLS 1\.3" docs/project/external-dependencies.md
	@! rg -n "^LWIP_SRC[[:space:]]*=" Makefile
	@echo "check-external-dependency-boundary: PASS"

smoke-riscv64:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/riscv64-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-bringup/kernel.elf \
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
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/loongarch64-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-loongarch64 \
		-machine virt -m 1G -nographic -smp 1 \
		-kernel .kernel-build/loongarch64-qemu-virt-loongarch64-linux-bringup/kernel.elf \
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
	$(MAKE) ARCH=aarch64 ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/aarch64-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-aarch64 \
		-machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/aarch64-qemu-virt-aarch64-linux-bringup/kernel.elf \
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
	$(MAKE) ARCH=x86_64 ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/x86_64-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-linux-bringup/kernel.elf \
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

# Headless behavioral gate for the QEMU GUI path. QMP injects a real keyboard
# event and screendump reads the emulated scanout, so this catches regressions
# that a kernel build or serial-only bring-up cannot observe.
smoke-qemu-gui-x86_64:
	$(MAKE) ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both BRINGUP=0 \
		.kernel-build/x86_64-qemu-virt-x86_64-both-dev/gui-fat32.img
	$(PYTHON) scripts/smoke_qemu_gui.py \
		--arch x86_64 \
		--qemu qemu-system-x86_64 \
		--kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf \
		--disk .kernel-build/x86_64-qemu-virt-x86_64-both-dev/gui-fat32.img \
		--timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-riscv64:
	$(MAKE) ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=both BRINGUP=0 \
		.kernel-build/riscv64-qemu-virt-riscv64-both-dev/gui-fat32.img
	$(PYTHON) scripts/smoke_qemu_gui.py \
		--arch riscv64 \
		--qemu qemu-system-riscv64 \
		--kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		--disk .kernel-build/riscv64-qemu-virt-riscv64-both-dev/gui-fat32.img \
		--artifacts .kernel-build/smoke/qemu-gui-riscv64 \
		--timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-aarch64:
	$(MAKE) ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=both BRINGUP=0 \
		.kernel-build/aarch64-qemu-virt-aarch64-both-dev/gui-fat32.img
	$(PYTHON) scripts/smoke_qemu_gui.py --arch aarch64 --qemu qemu-system-aarch64 \
		--kernel .kernel-build/aarch64-qemu-virt-aarch64-both-dev/kernel.elf \
		--disk .kernel-build/aarch64-qemu-virt-aarch64-both-dev/gui-fat32.img \
		--artifacts .kernel-build/smoke/qemu-gui-aarch64 --timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-arm32:
	$(MAKE) ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 \
		.kernel-build/arm32-qemu-virt-arm32-both-dev/gui-fat32.img
	$(PYTHON) scripts/smoke_qemu_gui.py --arch arm32 --qemu qemu-system-arm \
		--kernel .kernel-build/arm32-qemu-virt-arm32-both-dev/kernel.elf \
		--disk .kernel-build/arm32-qemu-virt-arm32-both-dev/gui-fat32.img \
		--artifacts .kernel-build/smoke/qemu-gui-arm32 --timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-loongarch64:
	$(MAKE) ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=both BRINGUP=0 \
		.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/gui-fat32.img
	$(PYTHON) scripts/smoke_qemu_gui.py --arch loongarch64 --qemu qemu-system-loongarch64 \
		--kernel .kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/kernel.elf \
		--disk .kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/gui-fat32.img \
		--artifacts .kernel-build/smoke/qemu-gui-loongarch64 --timeout $(SMOKE_TIMEOUT)

smoke-arm32:
	$(MAKE) ARCH=arm32 ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/arm32-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-arm \
		-machine virt -cpu cortex-a15 -m 1G -nographic -smp 1 \
		-kernel .kernel-build/arm32-qemu-virt-arm32-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'part ok' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-arm32: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-arm32: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-arm32: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-riscv32:
	$(MAKE) ARCH=riscv32 ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/riscv32-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv32 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv32-qemu-virt-riscv32-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'part ok' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-riscv32: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-riscv32: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-riscv32: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-arch-mmu-matrix:
	@set -e; \
	for spec in arm32:0 aarch64:0 riscv64:0 riscv32:0 \
		    arm32:1 aarch64:1 riscv64:1 riscv32:1; do \
		status=0; \
		arch=$${spec%%:*}; nommu=$${spec##*:}; \
		variant="$$arch"; [ "$$nommu" = 1 ] && variant="$$variant-nommu"; \
		build=".kernel-build/$$arch-both-dev"; \
		[ "$$nommu" = 1 ] && build="$$build-nommu"; \
		log=".kernel-build/smoke/$$variant-shell.log"; \
		mkdir -p .kernel-build/smoke; \
		echo "=== smoke-arch-mmu-matrix: $$variant ==="; \
		$(MAKE) ARCH=$$arch ABI=both BRINGUP=0 NOMMU=$$nommu dev-build >/dev/null; \
		case "$$arch" in \
		arm32) qemu=qemu-system-arm; base="-machine virt -cpu cortex-a15" ;; \
		aarch64) qemu=qemu-system-aarch64; base="-machine virt -cpu cortex-a57 -global virtio-mmio.force-legacy=false" ;; \
		riscv64) qemu=qemu-system-riscv64; base="-machine virt -bios default -global virtio-mmio.force-legacy=false" ;; \
		riscv32) qemu=qemu-system-riscv32; base="-machine virt -bios default -global virtio-mmio.force-legacy=false" ;; \
		esac; \
		{ sleep $(SMOKE_INPUT_DELAY); printf 'echo A20_MATRIX_%s_OK\n/bin/echo A20_EXTERNAL_OK\npoweroff\n' "$$variant"; } | \
		$(TIMEOUT) $(SMOKE_TIMEOUT) $$qemu $$base -m 1G -nographic -smp 1 \
			-drive file="$$build/fat32.img",if=none,format=raw,id=x0 \
			-device virtio-blk-device,bus=virtio-mmio-bus.0,drive=x0 \
			-netdev user,id=net \
			-device virtio-net-device,bus=virtio-mmio-bus.4,netdev=net \
			-kernel "$$build/kernel.elf" >"$$log" 2>&1 || status=$$?; \
		if ! grep -q "A20_MATRIX_$${variant}_OK" "$$log" || \
		   ! grep -q "A20_EXTERNAL_OK" "$$log" || \
		   ! grep -q "System is going down for power-off NOW" "$$log"; then \
			echo "smoke-arch-mmu-matrix: $$variant FAIL (status=$${status:-0})"; \
			tail -n 100 "$$log"; exit 1; \
		fi; \
		echo "smoke-arch-mmu-matrix: $$variant PASS"; \
	done

smoke-ppc64le:
	$(MAKE) ARCH=ppc64le ABI=linux BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/ppc64le-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-ppc64 \
		-machine pseries -m 1G -nographic -smp 1 \
		-kernel .kernel-build/ppc64le-qemu-virt-ppc64le-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'part ok' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-ppc64le: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-ppc64le: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-ppc64le: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-abi-linux:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/abi-linux-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'syscall_smoke\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'PROC_STRESS: PASS' "$$log" && \
	   grep -q 'PROC_STRESS: signal-stop-exit PASS' "$$log" && \
	   grep -q 'PROC_STRESS: signal-mask-park PASS' "$$log"; then \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
			-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
		$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
			-machine virt -m 1G -nographic -smp 1 -bios default \
			-global virtio-mmio.force-legacy=false \
			-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
			-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'FUTEX_STRESS: PASS' "$$log"; then \
		echo "smoke-futex-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-futex-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

.PHONY: check-task-lifetime-boundary check-blocking-point-boundary \
	check-signal-exit-boundary check-timeout-ownership-boundary \
	check-smp-runqueue-boundary check-process-lock-split-boundary \
	check-proc-step35 \
	check-proc-step4 check-proc-step4-local \
	check-proc-step5 check-proc-step5-local \
	check-proc-step6 check-proc-step6-local \
	check-proc-step7 check-proc-step7-local \
	check-proc-step8 check-proc-step8-local \
	check-proc-step35-local step35-rv-debug-1c step35-la-debug-1c \
	step35-rv-release-8c step35-la-release-8c \
	step6-rv-debug-1c step6-la-debug-1c \
	step6-rv-release-8c step6-la-release-8c \
	step7-rv-debug-1c step7-la-debug-1c \
	step7-rv-release-8c step7-la-release-8c \
	step8-rv-debug-1c step8-la-debug-1c \
	step8-rv-release-8c step8-la-release-8c _step35_smoke

step35-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		BUILD_DIR=.kernel-build/step35/riscv64-debug-1c \
		STEP35_LABEL=riscv64-debug-1c _step35_smoke

step35-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		BUILD_DIR=.kernel-build/step35/loongarch64-debug-1c \
		STEP35_LABEL=loongarch64-debug-1c _step35_smoke

step35-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		BUILD_DIR=.kernel-build/step35/riscv64-release-8c \
		STEP35_LABEL=riscv64-release-8c _step35_smoke

step35-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		BUILD_DIR=.kernel-build/step35/loongarch64-release-8c \
		STEP35_LABEL=loongarch64-release-8c _step35_smoke

step6-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/riscv64-debug-1c \
		STEP35_LABEL=step6-riscv64-debug-1c _step35_smoke

step6-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/loongarch64-debug-1c \
		STEP35_LABEL=step6-loongarch64-debug-1c _step35_smoke

step6-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/riscv64-release-8c \
		STEP35_LABEL=step6-riscv64-release-8c _step35_smoke

step6-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/loongarch64-release-8c \
		STEP35_LABEL=step6-loongarch64-release-8c _step35_smoke

step7-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/riscv64-debug-1c \
		STEP35_LABEL=step7-riscv64-debug-1c _step35_smoke

step7-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/loongarch64-debug-1c \
		STEP35_LABEL=step7-loongarch64-debug-1c _step35_smoke

step7-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/riscv64-release-8c \
		STEP35_LABEL=step7-riscv64-release-8c _step35_smoke

step7-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/loongarch64-release-8c \
		STEP35_LABEL=step7-loongarch64-release-8c _step35_smoke

step8-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/riscv64-debug-1c \
		STEP35_LABEL=step8-riscv64-debug-1c _step35_smoke

step8-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/loongarch64-debug-1c \
		STEP35_LABEL=step8-loongarch64-debug-1c _step35_smoke

step8-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/riscv64-release-8c \
		STEP35_LABEL=step8-riscv64-release-8c _step35_smoke

step8-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/loongarch64-release-8c \
		STEP35_LABEL=step8-loongarch64-release-8c _step35_smoke

_step35_smoke: dev-build
	@mkdir -p $(STEP35_LOG_DIR)
	@set -e; \
	stamp=$$(date -u +%Y%m%dT%H%M%SZ); \
	log="$(STEP35_LOG_DIR)/step35-$(STEP35_LABEL)-$$stamp.log"; \
	status=0; \
	{ sleep $(STEP35_INPUT_DELAY); \
	  printf 'lifetime_stress\ncat /proc/a20/task_lifetime\npoweroff\n'; } | \
	$(TIMEOUT) $(STEP35_TIMEOUT) $(QEMU) $(QEMU_FLAGS_NO_SDCARD) \
		-kernel $(KERNEL_ELF) \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -ne 0 ]; then \
		echo "_step35_smoke: QEMU failed status=$$status log=$$log"; \
		tail -n 120 "$$log"; \
		exit 1; \
	fi; \
	for marker in \
		'SCHED_STRESS: PASS' \
		'FUTEX_STRESS: PASS' \
		'FUTEX_STRESS: unrelated-wake-isolation PASS' \
		'FUTEX_STRESS: stale-timeout-isolation PASS' \
		'PROC_STRESS: PASS' \
		'PROC_STRESS: vfork-auto-reap PASS' \
		'PROC_STRESS: signal-stop-exit PASS' \
		'PROC_STRESS: signal-mask-park PASS' \
		'IO_EVENT_TEST: PASS' \
		'VFS_STRESS: PASS' \
		'SOCKET_STRESS: PASS' \
		'LIFETIME_STRESS: PASS' \
		'lifetime_errors: 0' \
		'System is going down for power-off NOW.'; do \
		if ! grep -q "$$marker" "$$log"; then \
			echo "_step35_smoke: missing '$$marker' log=$$log"; \
			tail -n 120 "$$log"; \
			exit 1; \
		fi; \
	done; \
	if [ "$(REQUIRE_TIMEOUT_CAPACITY)" = "1" ]; then \
		for marker in \
			'LIFETIME_STRESS: timeout-capacity-1 PASS' \
			'LIFETIME_STRESS: timeout-capacity PASS entries=' \
			'LIFETIME_STRESS: timeout-capacity+1 PASS' \
			'LIFETIME_STRESS: timeout-capacity PASS capacity='; do \
			if ! grep -q "$$marker" "$$log"; then \
				echo "_step35_smoke: missing '$$marker' log=$$log"; \
				tail -n 120 "$$log"; \
				exit 1; \
			fi; \
		done; \
	fi; \
	if [ "$(REQUIRE_SMP_RUNQUEUE)" = "1" ]; then \
		for marker in \
			'SCHED_STRESS: smp-runqueue PASS' \
			'scheduler_violations: 0'; do \
			if ! grep -q "$$marker" "$$log"; then \
				echo "_step35_smoke: missing '$$marker' log=$$log"; \
				tail -n 120 "$$log"; \
				exit 1; \
			fi; \
		done; \
	fi; \
	if [ "$(REQUIRE_LOCK_SPLIT)" = "1" ]; then \
		for marker in \
			'SCHED_STRESS: lock-split PASS' \
			'runqueue_local_picks:' \
			'runqueue_lock_acquires:' \
			'runqueue_parallel_pick_peak:' \
			'scheduler_violations: 0'; do \
			if ! grep -q "$$marker" "$$log"; then \
				echo "_step35_smoke: missing '$$marker' log=$$log"; \
				tail -n 120 "$$log"; \
				exit 1; \
			fi; \
		done; \
	fi; \
	if [ "$(NR_CPUS)" -gt 1 ] && \
	   ! grep -Fq '[SMP] $(NR_CPUS)/$(NR_CPUS) configured CPUs online' "$$log"; then \
		echo "_step35_smoke: not all $(NR_CPUS) CPUs came online log=$$log"; \
		tail -n 120 "$$log"; \
		exit 1; \
	fi; \
	if grep -Eq 'PANIC|sched invariant|reference underflow|use-after-free|\[LOCK\]' "$$log"; then \
		echo "_step35_smoke: lifecycle diagnostic failure log=$$log"; \
		grep -E 'PANIC|sched invariant|reference underflow|use-after-free|\[LOCK\]' "$$log" | head -n 20; \
		exit 1; \
	fi; \
	echo "_step35_smoke: PASS ($(STEP35_LABEL)); log saved to $$log"

check-proc-step35-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	step35-rv-debug-1c step35-la-debug-1c \
	step35-rv-release-8c step35-la-release-8c
	@git diff --check
	@echo "check-proc-step35-local: PASS"

check-proc-step6-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary \
	step6-rv-debug-1c step6-la-debug-1c \
	step6-rv-release-8c step6-la-release-8c
	@git diff --check
	@echo "check-proc-step6-local: PASS"

check-proc-step7-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary check-smp-runqueue-boundary \
	step7-rv-debug-1c step7-la-debug-1c \
	step7-rv-release-8c step7-la-release-8c
	@git diff --check
	@echo "check-proc-step7-local: PASS"

check-proc-step8-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary check-smp-runqueue-boundary \
	check-process-lock-split-boundary \
	step8-rv-debug-1c step8-la-debug-1c \
	step8-rv-release-8c step8-la-release-8c
	@git diff --check
	@echo "check-proc-step8-local: PASS"

check-proc-step35: check-proc-step35-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step35: PASS"

check-proc-step6: check-proc-step6-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step6: PASS"

check-proc-step7: check-proc-step7-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step7: PASS"

check-proc-step8: check-proc-step8-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step8: PASS"

check-proc-step4-local: check-blocking-point-boundary check-proc-step35-local
	@git diff --check
	@echo "check-proc-step4-local: PASS"

check-proc-step4: check-proc-step4-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step4: PASS"

check-proc-step5-local: check-signal-exit-boundary check-proc-step4-local
	@git diff --check
	@echo "check-proc-step5-local: PASS"

check-proc-step5: check-proc-step5-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step5: PASS"

smoke-socket-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/socket-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'socket_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
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
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-bringup-driver-lifecycle/kernel.elf \
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

smoke-hda:
	$(MAKE) ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=linux BRINGUP=1 CONFIG_HDA_SMOKE_TEST=y kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/hda-x86_64.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-audiodev driver=none,id=audio0 \
		-device intel-hda -device hda-duplex,audiodev=audio0 \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-linux-bringup-hda-smoke/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'HDA_STREAM_SMOKE: PASS' "$$log" && \
	   grep -q "bound to driver 'hda'" "$$log" && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-hda: PASS; log saved to $$log"; \
	else \
		echo "smoke-hda: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-pci-portability:
	$(MAKE) ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=linux BRINGUP=1 CONFIG_HDA_SMOKE_TEST=y CONFIG_NVME_SMOKE_TEST=y kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/pci-portability-loongarch64.log"; \
	image="$(SMOKE_LOG_DIR)/pci-portability-nvme.img"; \
	truncate -s 128M "$$image"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-loongarch64 \
		-machine virt -m 1G -nographic -smp 1 -no-reboot -snapshot \
		-audiodev driver=none,id=audio0 \
		-device intel-hda -device hda-duplex,audiodev=audio0 \
		-drive file="$$image",if=none,format=raw,id=nvme0 \
		-device nvme,drive=nvme0,serial=A20NVME \
		-kernel .kernel-build/loongarch64-qemu-virt-loongarch64-linux-bringup-hda-smoke-nvme-smoke/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'HDA_STREAM_SMOKE: PASS' "$$log" && \
	   grep -q 'NVME_IO_SMOKE: PASS' "$$log" && \
	   grep -q "bound to driver 'hda'" "$$log" && \
	   grep -q "bound to driver 'nvme'" "$$log" && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-pci-portability: PASS; log saved to $$log"; \
	else \
		echo "smoke-pci-portability: failed with status $$status; tail of $$log:"; \
		tail -n 100 "$$log"; \
		exit 1; \
	fi

smoke-native-handle:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-handle-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-handle-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
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

_contest_build: $(KERNEL_ELF) $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	$(MAKE) ARCH=$(ARCH) ABI=$(ABI) _contest_disk
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_contest_disk: $(USER_BUILD_STAMP) \
		user/contest_init/contest.sh \
		user/contest_init/run_ltp_resume.sh \
		user/contest_init/ltp_blacklist.txt
	rm -f $(DISK_OUT)
	$(MKFS_FAT) -C -F 32 $(DISK_OUT) 131072
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(DISK_OUT) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(DISK_OUT) $(USER_BUILD_DIR)/mksh ::/sh
	mcopy -o -i $(DISK_OUT) $(USER_BUILD_DIR)/mksh ::/bash
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

.PHONY: user_apps

.PHONY: force_user_build force_vbox_rootfs_verify
force_user_build:
	@:

force_vbox_rootfs_verify:
	@:

.PHONY: force_native_build
force_native_build:
	@:

$(USER_BUILD_STAMP): user/Makefile force_user_build | $(USER_BUILD_CHECK_DIRS)
	@set -e; \
	mkdir -p $(dir $@); \
	current=""; \
	if [ -f "$@" ]; then current=$$(cat "$@"); fi; \
	need_build=0; \
	need_clean=0; \
	if [ "$$current" != "$(USER_BUILD_ID)" ]; then \
		need_build=1; \
		need_clean=1; \
	elif [ ! -x "$(USER_BUILD_DIR)/init" ] || [ ! -x "$(USER_BUILD_DIR)/mksh" ]; then \
		need_build=1; \
	elif find user/Makefile $(USER_BUILD_CHECK_DIRS) \
		\( -path '*/.git' -o -path 'user/build' -o -path 'user/external/musl/build-*' \) -prune -o \
		-type f -newer "$@" -print -quit | grep -q .; then \
		need_build=1; \
	fi; \
	if [ "$$need_build" -eq 1 ]; then \
		if [ "$$need_clean" -eq 1 ]; then \
			$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" \
				PROFILE=$(PROFILE) BUILD_DIR=build/$(USER_VARIANT) clean; \
		fi; \
		$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" PROFILE=$(PROFILE) BUILD_DESKTOP=$(USER_BUILD_DESKTOP) \
			BUILD_DIR=build/$(USER_VARIANT); \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[USER] $(USER_BUILD_ID) up to date"; \
	fi

$(NATIVE_BUILD_STAMP): $(USER_BUILD_STAMP) force_native_build
	@set -e; \
	need_build=0; \
	current=""; \
	if [ -f "$@" ]; then current=$$(cat "$@"); fi; \
	if [ "$$current" != "$(USER_BUILD_ID)" ]; then \
		need_build=1; \
	elif [ ! -x "$(NATIVE_HELLO_BIN)" ] || [ ! -x "$(NATIVE_HANDLE_BIN)" ] || \
	     [ ! -x "$(NATIVE_LIBC_BIN)" ]; then \
		need_build=1; \
	elif find user/liba20rt user/liba20c user/tests -type f -newer "$@" \
		-print -quit | grep -q .; then \
		need_build=1; \
	fi; \
	if [ "$$need_build" -eq 1 ]; then \
		$(MAKE) ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(OPT)" native-programs; \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[NATIVE] $(USER_BUILD_ID) up to date"; \
	fi

fs_img: $(FS_TEST_IMG)

$(FAT32_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP) \
		user/contest_init/contest.sh \
		user/contest_init/final_contest.sh \
		user/contest_init/run_ltp_resume.sh \
		user/contest_init/ltp_blacklist.txt
	@echo "Building FAT32 image..."
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(FAT32_IMG) bs=1048576 count=$(FAT32_IMAGE_MB)
	$(MKFS_FAT) -F 32 $(FAT32_IMG)
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(FAT32_IMG) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(FAT32_IMG) $(USER_BUILD_DIR)/mksh ::/sh
	mcopy -o -i $(FAT32_IMG) $(USER_BUILD_DIR)/mksh ::/bash
	-mmd -i $(FAT32_IMG) ::/etc >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/lib >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/musl >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/musl/lib >/dev/null 2>&1
	@[ -f user/external/musl/build-$(USER_VARIANT)/lib/libc.so ] && \
		mcopy -o -i $(FAT32_IMG) user/external/musl/build-$(USER_VARIANT)/lib/libc.so ::/musl/lib/libc.so || true
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(FAT32_IMG) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(FAT32_IMG) - ::/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' | mcopy -o -i $(FAT32_IMG) - ::/etc/os-release
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i $(FAT32_IMG) - ::/test.txt
	mcopy -o -i $(FAT32_IMG) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/final_contest.sh ::/final_contest.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt

# Keep GUI state out of fat32.img so a later text-mode run does not inherit it.
# init uses this marker to replace the serial shell with the LVGL desktop.
$(GUI_FAT32_IMG): $(FAT32_IMG)
	cp $(FAT32_IMG) $(GUI_FAT32_IMG)
	@printf '1\n' | mcopy -o -i $(GUI_FAT32_IMG) - ::/etc/a20-gui
	@printf '1\n' | mcopy -o -i $(GUI_FAT32_IMG) - ::/a20-gui

$(FS_TEST_IMG): $(FAT32_IMG)
	cp $(FAT32_IMG) $(FS_TEST_IMG)

ext4_img_only: $(EXT4_IMG)

$(EXT4_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building ext4 image..."
	@rm -rf $(EXT4_STAGING_DIR) && mkdir -p $(EXT4_STAGING_DIR)
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		cp "$$f" "$(EXT4_STAGING_DIR)/$$(basename "$$f")"; \
	done
	cp $(USER_BUILD_DIR)/mksh $(EXT4_STAGING_DIR)/sh
	cp $(USER_BUILD_DIR)/mksh $(EXT4_STAGING_DIR)/bash
	printf 'Hello from ext4!\nThis file is on the ext4 filesystem.\n' > $(EXT4_STAGING_DIR)/test.txt
	@mkdir -p $(EXT4_STAGING_DIR)/etc
	@printf '%s\n' $(PROTOCOLS_LINES) > $(EXT4_STAGING_DIR)/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' > $(EXT4_STAGING_DIR)/etc/os-release
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXT4_IMG) bs=1048576 count=$(EXT4_IMAGE_MB)
	$(MKFS_EXT4) -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index -d $(EXT4_STAGING_DIR) $(EXT4_IMG)
	@rm -rf $(EXT4_STAGING_DIR)

ext4_img: $(USER_BUILD_STAMP) ext4_img_only
	cp $(EXT4_IMG) $(FS_TEST_IMG)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(VBOX_AARCH64_EFI): $(KERNEL_BIN) kernel/boot/uefi/aarch64_loader.c kernel/boot/uefi/aarch64_kernel_blob.S
	@mkdir -p $(dir $@)
	$(CC) -march=armv8-a -fpic -fshort-wchar -ffreestanding -fno-stack-protector \
		-fno-builtin -fvisibility=hidden -mno-outline-atomics \
		-DKERNEL_LOAD_ADDRESS=$(VBOX_AARCH64_LOAD_ADDRESS) \
		-c kernel/boot/uefi/aarch64_loader.c \
		-o $(BUILD_DIR)/uefi-loader.o
	$(CC) -march=armv8-a -fpic -ffreestanding \
		-DKERNEL_BIN_PATH='"$(abspath $(KERNEL_BIN))"' \
		-c kernel/boot/uefi/aarch64_kernel_blob.S -o $(BUILD_DIR)/uefi-kernel.o
	$(CC) -nostdlib -shared -Wl,-Bsymbolic -Wl,-e,efi_main \
		-Wl,-T,kernel/boot/uefi/aarch64_efi.lds \
		-o $(BUILD_DIR)/uefi-loader.so \
		$(BUILD_DIR)/uefi-loader.o $(BUILD_DIR)/uefi-kernel.o
	$(OBJCOPY) -j .text -j .reloc -j .dynamic -j .data -j .kernel \
		-j .rela -j .rela.* -j .rodata -j .dynsym -j .dynstr \
		-O pei-aarch64-little --subsystem efi-app \
		$(BUILD_DIR)/uefi-loader.so $@

# The user build stamp is intentionally refreshed by a recipe, so a user binary
# can become newer than an already-created FAT image during the same checkout.
# Verify the staged /init byte-for-byte every time a VBox image is requested;
# otherwise make's timestamp graph can leave a bootable but stale userspace in
# place after interrupted or manually-invoked sub-builds.
$(BUILD_DIR)/.vbox-rootfs-verified: force_vbox_rootfs_verify $(GUI_FAT32_IMG) $(USER_BUILD_STAMP)
	@set -e; \
	tmp=$$(mktemp); \
	trap 'rm -f "$$tmp"' EXIT HUP INT TERM; \
	mcopy -i $(GUI_FAT32_IMG) ::/init "$$tmp"; \
	cmp -s "$$tmp" "$(USER_BUILD_DIR)/init" || { \
		echo "[VBOX] stale /init detected; rebuilding GUI root filesystem"; \
		rm -f $(FAT32_IMG) $(GUI_FAT32_IMG); \
		$(MAKE) ARCH=$(ARCH) BOARD=$(BOARD) ABI=$(ABI) BRINGUP=$(BRINGUP) \
			NOMMU=$(NOMMU) OPT="$(OPT)" $(GUI_FAT32_IMG); \
		mcopy -i $(GUI_FAT32_IMG) ::/init "$$tmp"; \
		cmp -s "$$tmp" "$(USER_BUILD_DIR)/init"; \
	}; \
	touch $@

$(VBOX_AARCH64_IMG): $(VBOX_AARCH64_EFI) $(BUILD_DIR)/.vbox-rootfs-verified scripts/mk_uefi_fat_image.sh
	scripts/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $@ $(GUI_FAT32_IMG)

$(VBOX_AARCH64_TEXT_IMG): $(VBOX_AARCH64_EFI) $(FAT32_IMG) scripts/mk_uefi_fat_image.sh
	scripts/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $@ $(FAT32_IMG)

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(ARCH_LIBS) -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/stm32f1/bluetooth.o: $(STM32_BT_CONFIG_HDR)
$(BUILD_DIR)/drivers/stm32f1/wifi.o: $(STM32_WIFI_CONFIG_HDR)

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S Makefile | $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

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

stm32f103-bringup:
	$(MAKE) ARCH=armv7m BOARD=stm32f103 PROFILE=mcu STM32_FLASH_KB=64 STM32_RAM_KB=20 kernel-only

stm32f103-xuanwu:
	$(MAKE) ARCH=armv7m BOARD=stm32f103 PROFILE=mcu STM32_FLASH_KB=512 STM32_RAM_KB=64 STM32_XUANWU=1 kernel-only

check-stm32f103:
	@! rg -n '0x400[0-9A-Fa-f]{5}|0xE000E[0-9A-Fa-f]{3}' \
		$(KERNEL_DIR)/platform/stm32f103 --glob '*.[ch]' \
		--glob '!board.c' --glob '!board_config.h'
	@! rg -n 'CONFIG_ARMV7M' $(KERNEL_DIR) \
		--glob '!kernel/arch/**' --glob '!kernel/platform/**' \
		--glob '!kernel/external/**' --glob '!kernel/include/core/arch.h'
	$(MAKE) stm32f103-xuanwu
	@echo "check-stm32f103: PASS"

flash-stm32f103-xuanwu: stm32f103-xuanwu
	@command -v openocd >/dev/null 2>&1 || { \
		echo "openocd not found; install OpenOCD or use STM32CubeProgrammer"; \
		exit 1; \
	}
	openocd -f $(STM32_OPENOCD_INTERFACE) \
		$(if $(STM32_CMSIS_DAP_SERIAL),-c "adapter serial $(STM32_CMSIS_DAP_SERIAL)") \
		-c "transport select $(STM32_OPENOCD_TRANSPORT)" \
		-c "adapter speed $(STM32_OPENOCD_ADAPTER_KHZ)" \
		-f target/stm32f1x.cfg \
		-c "init" \
		-c "mww 0xE000EDF0 0xA05F0003" \
		-c "sleep 50" \
		-c "flash probe 0" \
		-c "flash write_image erase $(STM32_XUANWU_ELF)" \
		-c "verify_image $(STM32_XUANWU_ELF)" \
		-c "set boot_sp [mrw 0x08000000]" \
		-c "set boot_pc [mrw 0x08000004]" \
		-c "reg msp \$$boot_sp" \
		-c "reg psp 0" \
		-c "reg control 0" \
		-c "reg primask 0" \
		-c "reg basepri 0" \
		-c "reg faultmask 0" \
		-c "reg pc \$$boot_pc" \
		-c "resume" \
		-c "shutdown"

run-stm32f103-qemu:
	$(MAKE) ARCH=armv7m BOARD=stm32f103 PROFILE=mcu STM32_FLASH_KB=128 STM32_RAM_KB=8 STM32_QEMU=1 kernel-only
	qemu-system-arm -machine stm32vldiscovery -nographic \
		-kernel .kernel-build/armv7m-both-bringup-nommu-stm32f103-f128k-r8k-qemu/kernel.bin

# ----------------------------------------------------------------
# Run targets (development mode)
# ----------------------------------------------------------------

run:
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) _run_impl

run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _run_impl

run-gui-riscv64 run-gui-rv:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _run_gui_impl

run-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _run_impl

run-gui-loongarch64 run-gui-la:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _run_gui_impl

run-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _run_impl

run-gui-arm64 run-gui-aarch64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _run_gui_impl

run-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _run_impl

run-gui-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _run_gui_impl

vbox-iso-x86_64:
	$(MAKE) ARCH=x86_64 ABI=both BRINGUP=0 _vbox_iso_x86_64_impl

_vbox_iso_x86_64_impl: dev-build
	scripts/mk_grub_iso.sh $(KERNEL_ELF) $(BUILD_DIR)/a20os-x86_64.iso

vbox-image-aarch64:
	$(MAKE) ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both BRINGUP=0 _vbox_image_aarch64_impl

vbox-text-image-aarch64:
	$(MAKE) ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both BRINGUP=0 _vbox_text_image_aarch64_impl

# The ARM VirtualBox target is a graphical machine by default.  Its kernel
# runs the SVGAv3/VMSVGA driver and the image carries the GUI marker used by
# /bin/init to start the desktop instead of the serial-only shell.
vbox-gui-image-aarch64: vbox-image-aarch64
	$(MAKE) ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both BRINGUP=0 _vbox_gui_image_aarch64_impl

_vbox_image_aarch64_impl: $(VBOX_AARCH64_IMG)
	@echo "VirtualBox ARM64 image ready: $(VBOX_AARCH64_IMG)"

_vbox_text_image_aarch64_impl: $(VBOX_AARCH64_TEXT_IMG)
	@echo "VirtualBox ARM64 text image ready: $(VBOX_AARCH64_TEXT_IMG)"

_vbox_gui_image_aarch64_impl: $(VBOX_AARCH64_EFI) $(GUI_FAT32_IMG) scripts/mk_uefi_fat_image.sh
	scripts/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $(BUILD_DIR)/a20os-vbox-aarch64-gui.img $(GUI_FAT32_IMG)
	@echo "VirtualBox ARM64 GUI image ready: $(BUILD_DIR)/a20os-vbox-aarch64-gui.img"

run-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) _run_impl

run-gui-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) _run_gui_impl

run-riscv32:
	$(MAKE) ARCH=riscv32 BRINGUP=$(BRINGUP) _run_impl

run-ppc64le:
	$(MAKE) ARCH=ppc64le BRINGUP=$(BRINGUP) _run_impl

run-nommu-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

run-nommu-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

run-nommu-aarch64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

run-nommu-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

run-nommu-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

run-nommu-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

run-gui-nommu-arm32 run-nommu-gui-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) NOMMU=1 _run_gui_impl

run-nommu-riscv32:
	$(MAKE) ARCH=riscv32 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	@test -s $(KERNEL_ELF) || (echo "ERROR: kernel ELF missing or empty: $(KERNEL_ELF)" ; exit 1)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

_run_gui_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) $(GUI_FAT32_IMG)
	@test -s $(KERNEL_ELF) || (echo "ERROR: kernel ELF missing or empty: $(KERNEL_ELF)" ; exit 1)
	$(QEMU) $(subst $(FAT32_IMG),$(GUI_FAT32_IMG),$(patsubst -nographic,-display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) -serial stdio,$(QEMU_FLAGS))) -kernel $(KERNEL_ELF)

# --- Debug Targets ---

debug-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _debug_impl

debug-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _debug_impl

debug-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _debug_impl

debug-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _debug_impl

debug-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) _debug_impl

debug-riscv32:
	$(MAKE) ARCH=riscv32 BRINGUP=$(BRINGUP) _debug_impl

debug-ppc64le:
	$(MAKE) ARCH=ppc64le BRINGUP=$(BRINGUP) _debug_impl

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
	$(MAKE) -f user/extra.mk ARCH=$(ARCH) OPT="$(OPT)" PACKAGES="$(EXTRA_PACKAGES)"

# Debian/Ubuntu cross toolchains already provide a complete target sysroot and
# return immediately here.  Fedora's cross GCC omits it, so bootstrap the
# target runtime from Fedora's official RISC-V repository into the project.
prepare-riscv64-glibc-sysroot:
	@if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter rust rustc cargo rustfmt,$(EXTRA_PACKAGES))" ]; then \
		user/extra/prepare-riscv64-glibc-sysroot.sh \
			"$(RISCV_GLIBC_LIB_DIR)" "$(RISCV_GLIBC_LOCAL_ROOT)" "$(FEDORA_RISCV_RELEASE)"; \
	fi

force_extra_image_stamp:
	@:

$(EXTRA_IMAGE_STAMP): force_extra_image_stamp
	@set -e; \
	mkdir -p "$(dir $@)"; \
	tmp="$@.tmp"; \
	{ \
		printf '%s\n' \
			"arch=$(ARCH)" \
			"nommu=$(NOMMU)" \
			"opt=$(OPT)" \
			"profile=$(PROFILE)" \
			"user_variant=$(USER_VARIANT)" \
			"packages=$(sort $(EXTRA_PACKAGES))" \
			"image_mb=$(EXTRA_IMAGE_MB)" \
			"image=$(EXTRA_IMG)" \
			"glibc_dir=$(RISCV_GLIBC_LIB_DIR)" \
			"glibc_local_dir=$(RISCV_GLIBC_LOCAL_LIB_DIR)"; \
		for f in "$(USER_BUILD_DIR)"/*; do \
			[ -f "$$f" ] || continue; \
			name=$$(basename "$$f"); \
			case "$$name" in *.o|*.a|*.so|*.d) continue ;; esac; \
			find -H "$$f" -maxdepth 0 -printf 'user %f %s %T@\n'; \
		done; \
		for f in user/build/extra/$(ARCH)/*; do \
			[ -f "$$f" ] || continue; \
			name=$$(basename "$$f"); \
			case " $(EXTRA_PACKAGES) " in *" $$name "*) \
				find -H "$$f" -maxdepth 0 -printf 'extra %f %s %T@\n' ;; \
			esac; \
		done; \
		for package in $(sort $(EXTRA_PACKAGES)); do \
			case "$$package" in \
				vim) stamp=.vim-built ;; \
				git) stamp=.git-built ;; \
				gcc|cc) stamp=.gcc-built ;; \
				rust|rustc|cargo|rustfmt) stamp=.rust-built ;; \
				*) continue ;; \
			esac; \
			f="user/build/extra/$(ARCH)/stamp/$$stamp"; \
			[ ! -f "$$f" ] || find "$$f" -maxdepth 0 -printf 'stamp %f %s %T@\n'; \
		done; \
		if [ -n "$(filter vim,$(EXTRA_PACKAGES))" ]; then \
			find user/external/vim/runtime -type f -printf 'vim-runtime %P %s %T@\n' 2>/dev/null || true; \
		fi; \
		if [ -n "$(filter git,$(EXTRA_PACKAGES))" ]; then \
			find user/external/git/templates/blt -type f -printf 'git-template %P %s %T@\n' 2>/dev/null || true; \
		fi; \
		if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter rust rustc cargo rustfmt,$(EXTRA_PACKAGES))" ]; then \
			for dir in "$(RISCV_GLIBC_LIB_DIR)" "$(RISCV_GLIBC_LOCAL_LIB_DIR)"; do \
				[ -n "$$dir" ] || continue; \
				for name in ld-linux-riscv64-lp64d.so.1 libc.so.6 libdl.so.2 libm.so.6 \
					libpthread.so.0 librt.so.1 libatomic.so.1 libgcc_s.so.1; do \
					f="$$dir/$$name"; \
					[ ! -f "$$f" ] || find -H "$$f" -maxdepth 0 -printf 'glibc %p %s %T@\n'; \
				done; \
			done; \
		fi; \
		find Makefile user/extra.mk -maxdepth 0 -type f -printf 'recipe %p %s %T@\n'; \
	} | LC_ALL=C sort > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$@" "$$tmp"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
		echo "[EXTRA] image inputs changed"; \
	fi

# Refresh the input manifest after package preparation, then let a second make
# decide from real timestamps whether the expensive image recipe is necessary.
extra-img: extra-user-apps prepare-riscv64-glibc-sysroot
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG="$(EXTRA_IMG)" "$(EXTRA_IMAGE_STAMP)"
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG="$(EXTRA_IMG)" _extra-img

_extra-img: $(EXTRA_IMG)

$(EXTRA_IMG): $(EXTRA_IMAGE_STAMP)
	@echo "Building extra packages image..."
	@rm -rf $(EXTRA_STAGING_DIR) && mkdir -p $(EXTRA_STAGING_DIR)/bin
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		case "$$name" in \
			.build-id) continue ;; \
			*.o|*.a|*.so|*.d) continue ;; \
		esac; \
		cp "$$f" "$(EXTRA_STAGING_DIR)/bin/$$name"; \
	done; \
	for f in user/build/extra/$(ARCH)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		case " $(EXTRA_PACKAGES) " in *" $$name "*) cp "$$f" "$(EXTRA_STAGING_DIR)/bin/$$name" ;; esac; \
	done
	@set -e; \
	if [ -n "$(filter gcc cc,$(EXTRA_PACKAGES))" ] && [ -d user/build/extra/$(ARCH)/obj/gcc-install ]; then \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/libexec "$(EXTRA_STAGING_DIR)/libexec"; \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/lib "$(EXTRA_STAGING_DIR)/lib"; \
		for t in user/build/extra/$(ARCH)/obj/gcc-install/bin/*; do \
			[ -f "$$t" ] && cp "$$t" "$(EXTRA_STAGING_DIR)/bin/$$(basename $$t)"; \
		done; \
		mv "$(EXTRA_STAGING_DIR)/bin/gcc" "$(EXTRA_STAGING_DIR)/bin/gcc-real"; \
		printf '#!/bin/sh\nexec /test/bin/gcc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/gcc"; \
		mv "$(EXTRA_STAGING_DIR)/bin/cc" "$(EXTRA_STAGING_DIR)/bin/cc-real"; \
		printf '#!/bin/sh\nexec /test/bin/cc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cc"; \
	fi
	@if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter rust rustc cargo rustfmt,$(EXTRA_PACKAGES))" ]; then \
		RUST=user/build/extra/$(ARCH)/obj/rust; \
		[ -x "$$RUST/bin/rustc" ] && [ -x "$$RUST/bin/cargo" ] && \
			[ -x "$$RUST/bin/rustfmt" ] && [ -x "$$RUST/bin/cargo-fmt" ] || \
			{ echo "Rust installation incomplete in $$RUST"; exit 1; }; \
		GLIBC="$(RISCV_GLIBC_LIB_DIR)"; \
		REQUIRED_GLIBC="ld-linux-riscv64-lp64d.so.1 libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0 librt.so.1 libatomic.so.1 libgcc_s.so.1"; \
		MISSING_GLIBC=""; \
		for f in $$REQUIRED_GLIBC; do [ -n "$$GLIBC" ] && [ -f "$$GLIBC/$$f" ] || MISSING_GLIBC="$$MISSING_GLIBC $$f"; done; \
		if [ -n "$$MISSING_GLIBC" ]; then \
			GLIBC="$(RISCV_GLIBC_LOCAL_LIB_DIR)"; \
			MISSING_GLIBC=""; \
			for f in $$REQUIRED_GLIBC; do [ -f "$$GLIBC/$$f" ] || MISSING_GLIBC="$$MISSING_GLIBC $$f"; done; \
		fi; \
		[ -z "$$MISSING_GLIBC" ] || { \
			echo "RISC-V glibc runtime incomplete in '$$GLIBC'; missing:$$MISSING_GLIBC"; \
			echo "Install/provide the cross glibc runtime or set RISCV_GLIBC_LIB_DIR to a directory containing all required libraries"; \
			exit 1; \
		}; \
		cp -a "$$RUST" "$(EXTRA_STAGING_DIR)/rust"; \
		printf '#!/bin/sh\nexec /test/rust/bin/rustc --target riscv64gc-unknown-linux-musl -C linker=/test/rust/lib/rustlib/riscv64gc-unknown-linux-gnu/bin/rust-lld -C relocation-model=static -C link-arg=-L/test/rust/a20-sysroot/lib -C link-arg=-static -C link-arg=/test/rust/a20-sysroot/lib/crt1.o -C link-arg=/test/rust/a20-sysroot/lib/crti.o -C link-arg=/test/rust/a20-sysroot/lib/crtn.o "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/rustc"; \
		printf '#!/bin/sh\nexport RUSTC=/test/rust/bin/rustc\nexport CARGO_BUILD_TARGET=riscv64gc-unknown-linux-musl\nexec /test/rust/bin/cargo --config /test/rust/config.toml "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cargo"; \
		printf '#!/bin/sh\nexec /test/rust/bin/rustfmt "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/rustfmt"; \
		printf '#!/bin/sh\nexec /test/rust/bin/cargo-fmt "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cargo-fmt"; \
		printf '[target.riscv64gc-unknown-linux-musl]\nlinker = "/test/rust/lib/rustlib/riscv64gc-unknown-linux-gnu/bin/rust-lld"\nrustflags = ["-C", "relocation-model=static", "-C", "link-arg=-L/test/rust/a20-sysroot/lib", "-C", "link-arg=-static", "-C", "link-arg=/test/rust/a20-sysroot/lib/crt1.o", "-C", "link-arg=/test/rust/a20-sysroot/lib/crti.o", "-C", "link-arg=/test/rust/a20-sysroot/lib/crtn.o"]\n' > "$(EXTRA_STAGING_DIR)/rust/config.toml"; \
		chmod 0755 "$(EXTRA_STAGING_DIR)/bin/rustc" "$(EXTRA_STAGING_DIR)/bin/cargo" \
			"$(EXTRA_STAGING_DIR)/bin/rustfmt" "$(EXTRA_STAGING_DIR)/bin/cargo-fmt"; \
		mkdir -p "$(EXTRA_STAGING_DIR)/glibc/lib"; \
		for f in $$REQUIRED_GLIBC; do \
			cp -aL "$$GLIBC/$$f" "$(EXTRA_STAGING_DIR)/glibc/lib/$$f"; \
		done; \
	fi
	@VIM_RT="$(EXTRA_STAGING_DIR)/share/vim/vim92"; \
	VIM_SRC=user/external/vim/runtime; \
	if [ -z "$(filter vim,$(EXTRA_PACKAGES))" ] || [ ! -d "$$VIM_SRC" ]; then exit 0; fi; \
	mkdir -p "$$VIM_RT"; \
	for f in defaults.vim filetype.vim ftoff.vim ftplugin.vim ftplugof.vim indent.vim indoff.vim; do \
		[ -f "$$VIM_SRC/$$f" ] && cp "$$VIM_SRC/$$f" "$$VIM_RT/$$f"; \
	done; \
	for d in syntax indent ftplugin autoload; do \
		mkdir -p "$$VIM_RT/$$d"; \
		cp -a "$$VIM_SRC/$$d/." "$$VIM_RT/$$d/"; \
	done
	@GIT_TEMPLATE_SRC=user/external/git/templates/blt; \
	GIT_TEMPLATE_DST="$(EXTRA_STAGING_DIR)/share/git-core/templates"; \
	if [ -n "$(filter git,$(EXTRA_PACKAGES))" ] && [ -d "$$GIT_TEMPLATE_SRC" ]; then \
		mkdir -p "$$GIT_TEMPLATE_DST"; \
		cp -a "$$GIT_TEMPLATE_SRC"/. "$$GIT_TEMPLATE_DST"/; \
	fi
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXTRA_IMG) bs=1048576 count=$(EXTRA_IMAGE_MB) 2>/dev/null
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
else ifeq ($(ARCH), arm32)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), riscv32)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), ppc64le)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
endif

run-riscv64-extra:
	$(MAKE) ARCH=riscv64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-loongarch64-extra:
	$(MAKE) ARCH=loongarch64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-arm64-extra:
	$(MAKE) ARCH=aarch64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-x86_64-extra:
	$(MAKE) ARCH=x86_64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-arm32-extra:
	$(MAKE) ARCH=arm32 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-riscv32-extra:
	$(MAKE) ARCH=riscv32 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-ppc64le-extra:
	$(MAKE) ARCH=ppc64le BRINGUP=0 PROFILE=benchmark _run_extra_impl

_run_extra_impl:
	$(MAKE) ARCH=$(ARCH) BRINGUP=0 dev-build
	@if [ -f user/external/fastfetch/src/fastfetch.c ]; then \
		$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" PROFILE=$(PROFILE) \
			BUILD_DIR=build/$(USER_VARIANT) fastfetch; \
	else \
		echo "[EXTRA] fastfetch source unavailable; skipping"; \
	fi
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG=$(EXTRA_IMG) extra-img
	$(QEMU) $(QEMU_FLAGS_NO_SDCARD) $(EXTRA_QEMU_BLK) -kernel $(KERNEL_ELF)

NATIVE_TEST_DIR  := user/tests
NATIVE_LD        := user/liba20rt/a20-generic.ld
NATIVE_CRT0_RV   := user/liba20rt/crt0_rv64.S
NATIVE_CRT0_LA   := user/liba20rt/crt0_la64.S
NATIVE_CRT0_AARCH64 := user/liba20rt/crt0_aarch64.S
NATIVE_CRT0_X86_64  := user/liba20rt/crt0_x86_64.S
NATIVE_CRT0_ARM32   := user/liba20rt/crt0_arm32.S
NATIVE_CRT0_RV32    := user/liba20rt/crt0_rv64.S
NATIVE_CRT0_PPC64LE := user/liba20rt/crt0_ppc64le.S
NATIVE_CC_riscv64     := $(RISCV_ELF_PREFIX)gcc
NATIVE_CC_loongarch64 := loongarch64-linux-gnu-gcc
NATIVE_CC_aarch64     := aarch64-linux-gnu-gcc
NATIVE_CC_x86_64      := x86_64-linux-gnu-gcc
NATIVE_CC_arm32       := arm-linux-gnueabihf-gcc
NATIVE_CC_riscv32     := $(RISCV_ELF_PREFIX)gcc
NATIVE_CC_ppc64le     := powerpc64le-linux-gnu-gcc
NATIVE_CC := $(NATIVE_CC_$(ARCH))
NATIVE_CFLAGS_riscv64     := -march=rv64gc -mabi=lp64d -mcmodel=medany
NATIVE_CFLAGS_loongarch64 := -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic
NATIVE_CFLAGS_aarch64     := -march=armv8-a -fno-pic
NATIVE_CFLAGS_x86_64      := -m64 -mno-red-zone -fno-pic -fno-pie
NATIVE_CFLAGS_arm32       := -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -fno-builtin
NATIVE_CFLAGS_riscv32     := -march=rv32imafdc -mabi=ilp32d -mcmodel=medany -fno-builtin
NATIVE_CFLAGS_ppc64le     := -m64 -mcpu=power8 -mabi=elfv2 -fno-pic -mno-vsx -mno-altivec
NATIVE_CFLAGS := $(NATIVE_CFLAGS_$(ARCH))
NATIVE_LIBS_arm32 := -Wl,--start-group -lgcc -Wl,--end-group
NATIVE_LIBS := $(if $(NATIVE_LIBS_$(ARCH)),$(NATIVE_LIBS_$(ARCH)),-lgcc)
NATIVE_ARCH_SRC_arm32 := user/liba20rt/a20_arm_rt.c
NATIVE_ARCH_SRC := $(NATIVE_ARCH_SRC_$(ARCH))
NATIVE_CRT0_riscv64     := $(NATIVE_CRT0_RV)
NATIVE_CRT0_loongarch64 := $(NATIVE_CRT0_LA)
NATIVE_CRT0_aarch64     := $(NATIVE_CRT0_AARCH64)
NATIVE_CRT0_x86_64      := $(NATIVE_CRT0_X86_64)
NATIVE_CRT0_arm32       := $(NATIVE_CRT0_ARM32)
NATIVE_CRT0_riscv32     := $(NATIVE_CRT0_RV32)
NATIVE_CRT0_ppc64le     := $(NATIVE_CRT0_PPC64LE)
NATIVE_CRT0 := $(NATIVE_CRT0_$(ARCH))
NATIVE_SDK_SRC   := user/liba20rt/a20_malloc.c
NATIVE_COMPILER_RT_SRC := user/liba20rt/a20_compiler_rt.c

NATIVE_LIBC_SRC  := \
    user/liba20c/malloc.c \
    user/liba20c/bare_alloc.c \
    user/liba20c/unistd.c \
    user/liba20c/dirent.c \
    user/liba20c/fcntl.c \
    user/liba20c/stdlib.c \
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
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
	    $(3) \
	    $(NATIVE_SDK_SRC) \
	    $(NATIVE_COMPILER_RT_SRC) \
	    $(NATIVE_ARCH_SRC) \
	    user/tests/test_native_hello.c \
	    $(NATIVE_LIBS) \
	    -o $(4)
endef

define NATIVE_MINIMAL_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    user/tests/test_native_minimal.c \
    -o $(4)
endef

$(NATIVE_HELLO_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_hello.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_TEST_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-test-arch: $(NATIVE_HELLO_BIN)

native-test-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-test-arch
native-test-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-test-arch
native-test-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) native-test-arch
native-test-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) native-test-arch
native-test-arm32:
	$(MAKE) ARCH=arm32 NOMMU=$(NOMMU) native-test-arch
native-test-rv32:
	$(MAKE) ARCH=riscv32 NOMMU=$(NOMMU) native-test-arch
native-test-ppc64le:
	$(MAKE) ARCH=ppc64le NOMMU=$(NOMMU) native-test-arch

native-test: $(DEFAULT_NATIVE_TEST_TARGETS)

native-test-all: native-test-rv native-test-la native-test-aarch64 native-test-x86_64 native-test-arm32 native-test-rv32 native-test-ppc64le

native-minimal-rv:
	$(call NATIVE_MINIMAL_RECIPE,$(RISCV_ELF_PREFIX)gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/riscv64/native-minimal-rv)
	@file user/build/riscv64/native-minimal-rv

native-minimal-la:
	$(call NATIVE_MINIMAL_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/loongarch64/native-minimal-la)
	@file user/build/loongarch64/native-minimal-la

native-minimal: native-minimal-rv native-minimal-la

define NATIVE_HANDLE_TEST_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
	    $(3) \
	    $(NATIVE_SDK_SRC) \
	    $(NATIVE_COMPILER_RT_SRC) \
	    $(NATIVE_ARCH_SRC) \
	    user/tests/test_native_handle.c \
	    $(NATIVE_LIBS) \
	    -o $(4)
endef

$(NATIVE_HANDLE_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_handle.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_HANDLE_TEST_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-handle-test-arch: $(NATIVE_HANDLE_BIN)

native-handle-test-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-arm32:
	$(MAKE) ARCH=arm32 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-rv32:
	$(MAKE) ARCH=riscv32 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-ppc64le:
	$(MAKE) ARCH=ppc64le NOMMU=$(NOMMU) native-handle-test-arch

native-handle-test: $(DEFAULT_NATIVE_HANDLE_TARGETS)

native-handle-test-all: native-handle-test-rv native-handle-test-la native-handle-test-aarch64 native-handle-test-x86_64 native-handle-test-arm32 native-handle-test-rv32 native-handle-test-ppc64le

define NATIVE_LIBC_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt -Iuser/liba20c/include \
    -T$(NATIVE_LD) \
    $(3) \
	    $(NATIVE_SDK_SRC) \
	    $(NATIVE_LIBC_SRC) \
	    $(NATIVE_ARCH_SRC) \
	    user/tests/test_liba20c.c \
	    $(NATIVE_LIBS) \
	    -o $(4)
endef

$(NATIVE_LIBC_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_LIBC_SRC) $(NATIVE_ARCH_SRC) \
		user/tests/test_liba20c.c user/liba20rt/a20-generic.ld \
		user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_LIBC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-libc-arch: $(NATIVE_LIBC_BIN)

native-libc-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-libc-arch
native-libc-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-libc-arch
native-libc-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) native-libc-arch
native-libc-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) native-libc-arch
native-libc-arm32:
	$(MAKE) ARCH=arm32 NOMMU=$(NOMMU) native-libc-arch
native-libc-rv32:
	$(MAKE) ARCH=riscv32 NOMMU=$(NOMMU) native-libc-arch
native-libc-ppc64le:
	$(MAKE) ARCH=ppc64le NOMMU=$(NOMMU) native-libc-arch

native-libc: $(DEFAULT_NATIVE_LIBC_TARGETS)

native-libc-all: native-libc-rv native-libc-la native-libc-aarch64 native-libc-x86_64 native-libc-arm32 native-libc-rv32 native-libc-ppc64le

native-programs: $(NATIVE_OUTPUTS)

smoke-native-libc:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-libc-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-libc-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
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
.PHONY: eval-check eval-check-rv eval-check-la

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
		if command -v curl >/dev/null 2>&1; then \
			curl -fL --retry 3 -o $(EVAL_DIR)/sdcard-rv.img.xz $(SDCARD_RV_URL); \
		elif command -v wget >/dev/null 2>&1; then \
			wget -q -O $(EVAL_DIR)/sdcard-rv.img.xz $(SDCARD_RV_URL); \
		else \
			echo "[eval] curl or wget is required"; exit 1; \
		fi; \
		xz -dc $(EVAL_DIR)/sdcard-rv.img.xz > $@; \
	fi

$(EVAL_DIR)/sdcard-la.img: | $(EVAL_DIR)
	@if [ -f sdcard-la.img ]; then \
		ln -sf "$$(pwd)/sdcard-la.img" $@; \
	elif [ -f $@ ]; then \
		echo "[eval] reusing cached sdcard-la.img"; \
	else \
		echo "[eval] downloading sdcard-la.img ..."; \
		if command -v curl >/dev/null 2>&1; then \
			curl -fL --retry 3 -o $(EVAL_DIR)/sdcard-la.img.xz $(SDCARD_LA_URL); \
		elif command -v wget >/dev/null 2>&1; then \
			wget -q -O $(EVAL_DIR)/sdcard-la.img.xz $(SDCARD_LA_URL); \
		else \
			echo "[eval] curl or wget is required"; exit 1; \
		fi; \
		xz -dc $(EVAL_DIR)/sdcard-la.img.xz > $@; \
	fi

# --- eval dev-build targets (match run-*, add contest-mode + 128 MB) ---
EVAL_KERNEL_RV  = .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf
EVAL_FAT32_RV   = .kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img
EVAL_KERNEL_LA  = .kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/kernel.elf
EVAL_FAT32_LA   = .kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/fat32.img
EVAL_NET_HOSTFWD_RV ?= hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
EVAL_NET_HOSTFWD_LA ?= hostfwd=tcp::5556-:5555,hostfwd=udp::5556-:5555
EVAL_NETDEV_RV = -netdev user,id=net$(if $(strip $(EVAL_NET_HOSTFWD_RV)),$(comma)$(EVAL_NET_HOSTFWD_RV),)
EVAL_NETDEV_LA = -netdev user,id=net$(if $(strip $(EVAL_NET_HOSTFWD_LA)),$(comma)$(EVAL_NET_HOSTFWD_LA),)

eval-dev-build-rv:
	$(MAKE) ARCH=riscv64 FAT32_IMAGE_MB=128 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_RV) - ::/etc/contest-mode

eval-dev-build-la:
	$(MAKE) ARCH=loongarch64 FAT32_IMAGE_MB=128 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_LA) - ::/etc/contest-mode

# --- QEMU launch ---
define RUN_QEMU_RV
	$(TIMEOUT) --foreground $(EVAL_TIMEOUT) \
	qemu-system-riscv64 -machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel $(EVAL_KERNEL_RV) \
		-drive 'file=$(EVAL_FAT32_RV),if=none,format=raw,id=x0' \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(EVAL_NETDEV_RV) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-drive 'file=$(EVAL_DIR)/sdcard-rv.img,if=none,format=raw,id=x1' \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		-no-reboot \
	2>&1 | tee $(EVAL_LOGS)/serial-rv.txt || true
endef

define RUN_QEMU_LA
	$(TIMEOUT) --foreground $(EVAL_TIMEOUT) \
	qemu-system-loongarch64 -machine virt -m 1G -nographic -smp 1 \
		-kernel $(EVAL_KERNEL_LA) \
		-drive 'file=$(EVAL_FAT32_LA),if=none,format=raw,id=x0' \
		-device virtio-blk-pci,drive=x0 \
		$(EVAL_NETDEV_LA) -device virtio-net-pci,netdev=net \
		-drive 'file=$(EVAL_DIR)/sdcard-la.img,if=none,format=raw,id=x1' \
		-device virtio-blk-pci,drive=x1 \
		-no-reboot \
	2>&1 | tee $(EVAL_LOGS)/serial-la.txt || true
endef

# --- Top-level eval targets ---
eval-rv: eval-dev-build-rv $(EVAL_DIR)/sdcard-rv.img | $(EVAL_LOGS)
	@echo "[eval] launching RISC-V QEMU (timeout=$(EVAL_TIMEOUT)s) ..."
	$(RUN_QEMU_RV)
	$(MAKE) eval-check-rv

eval-la: eval-dev-build-la $(EVAL_DIR)/sdcard-la.img | $(EVAL_LOGS)
	@echo "[eval] launching LoongArch QEMU (timeout=$(EVAL_TIMEOUT)s) ..."
	$(RUN_QEMU_LA)
	$(MAKE) eval-check-la

eval:
	@set -e; for target in $(DEFAULT_EVAL_TARGETS); do $(MAKE) $$target; done
	@echo "[eval] complete"

eval-all:
	$(MAKE) eval-rv
	$(MAKE) eval-la
	@echo "[eval] full architecture evaluation complete"

eval-check-rv:
	@log="$(EVAL_LOGS)/serial-rv.txt"; \
	test -s "$$log" || { echo "[eval][rv] missing log: $$log"; exit 1; }; \
	grep -q '\[CONTEST\] Done:' "$$log" || { echo "[eval][rv] incomplete: missing [CONTEST] Done"; exit 1; }; \
	grep -q '#### OS COMP TEST GROUP START ltp-glibc ####' "$$log" || { echo "[eval][rv] missing ltp-glibc group"; exit 1; }; \
	grep -q '#### OS COMP TEST GROUP START ltp-musl ####' "$$log" || { echo "[eval][rv] missing ltp-musl group"; exit 1; }; \
	awk ' \
		/#### OS COMP TEST GROUP START [A-Za-z0-9-]+ ####/ { start[$$7]++; next } \
		/#### OS COMP TEST GROUP END/ { end[$$7]++; next } \
		END { \
			ok = 1; \
			groups = 0; \
			for (g in start) { \
				groups++; \
				if (start[g] != end[g]) { \
					printf("[eval][rv] unmatched group %s: start=%d end=%d\n", g, start[g], end[g]); \
					ok = 0; \
				} \
			} \
			for (g in end) { \
				if (!(g in start)) { \
					printf("[eval][rv] group ended without start %s: end=%d\n", g, end[g]); \
					ok = 0; \
				} \
			} \
			if (groups == 0) { \
				print "[eval][rv] no score groups found"; \
				ok = 0; \
			} \
			exit ok ? 0 : 1; \
		}' "$$log"; \
	echo "[eval][rv] log is complete enough for scorer parsing"

eval-check-la:
	@log="$(EVAL_LOGS)/serial-la.txt"; \
	test -s "$$log" || { echo "[eval][la] missing log: $$log"; exit 1; }; \
	grep -q '\[CONTEST\] Done:' "$$log" || { echo "[eval][la] incomplete: missing [CONTEST] Done"; exit 1; }; \
	grep -q '#### OS COMP TEST GROUP START ltp-glibc ####' "$$log" || { echo "[eval][la] missing ltp-glibc group"; exit 1; }; \
	grep -q '#### OS COMP TEST GROUP START ltp-musl ####' "$$log" || { echo "[eval][la] missing ltp-musl group"; exit 1; }; \
	awk ' \
		/#### OS COMP TEST GROUP START [A-Za-z0-9-]+ ####/ { start[$$7]++; next } \
		/#### OS COMP TEST GROUP END/ { end[$$7]++; next } \
		END { \
			ok = 1; \
			groups = 0; \
			for (g in start) { \
				groups++; \
				if (start[g] != end[g]) { \
					printf("[eval][la] unmatched group %s: start=%d end=%d\n", g, start[g], end[g]); \
					ok = 0; \
				} \
			} \
			for (g in end) { \
				if (!(g in start)) { \
					printf("[eval][la] group ended without start %s: end=%d\n", g, end[g]); \
					ok = 0; \
				} \
			} \
			if (groups == 0) { \
				print "[eval][la] no score groups found"; \
				ok = 0; \
			} \
			exit ok ? 0 : 1; \
		}' "$$log"; \
	echo "[eval][la] log is complete enough for scorer parsing"

eval-check: eval-check-rv eval-check-la

# ----------------------------------------------------------------
# 2026 final-round evaluation.  Each target builds an 8-CPU image, restores a
# read-only official ext4 base from its .gz, creates a per-run qcow2 overlay,
# runs QEMU with the published 8 GiB/8-CPU TCG configuration, invokes the
# official judge, and archives the complete run under .eval-state/2026.
# ----------------------------------------------------------------
FINAL_EVAL_IMAGE_DIR ?= contest/2026OSImage-Pub
FINAL_EVAL_STATE_DIR ?= .eval-state/2026
FINAL_EVAL_TIMEOUT ?=

define RUN_FINAL_EVAL
	FINAL_EVAL_IMAGE_DIR="$(FINAL_EVAL_IMAGE_DIR)" \
	FINAL_EVAL_STATE_DIR="$(FINAL_EVAL_STATE_DIR)" \
	$(if $(strip $(FINAL_EVAL_TIMEOUT)),FINAL_EVAL_TIMEOUT="$(FINAL_EVAL_TIMEOUT)") \
	bash ./scripts/run_final_eval.sh $(1) $(2)
endef

final-eval-rv-cagent:
	$(call RUN_FINAL_EVAL,riscv64,cagent)

final-eval-la-cagent:
	$(call RUN_FINAL_EVAL,loongarch64,cagent)

final-eval-rv-buildstorm:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm)

final-eval-la-buildstorm:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm)

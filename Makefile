# A20OS Makefile

# Parallel build
NPROC ?= $(or $(shell getconf _NPROCESSORS_ONLN 2>/dev/null),$(shell sysctl -n hw.logicalcpu 2>/dev/null),4)
PYTHON ?= python3
TIMEOUT ?= $(PYTHON) tools/run_with_timeout.py
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

include tools/driver-deployment.mk
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
# Preserve established generic and STM32 output paths used by smoke, contest,
# flash, and QEMU runners. Explicit embedded builds on non-MCU architectures
# get their own suffix so they never share objects with generic.
BUILD_VARIANT = $(ABI)-$(if $(filter 1,$(BRINGUP)),bringup,dev)$(if $(and $(filter embedded,$(DRIVER_DEPLOYMENT)),$(filter-out armv7m,$(ARCH))),-embedded,)$(if $(filter 1,$(NOMMU)),-nommu,)$(if $(filter-out 1,$(NR_CPUS)),-smp$(NR_CPUS),)$(if $(filter y,$(CONFIG_DRIVER_LIFECYCLE_TEST)),-driver-lifecycle,)$(if $(filter y,$(CONFIG_HDA_SMOKE_TEST)),-hda-smoke,)$(if $(filter y,$(CONFIG_NVME_SMOKE_TEST)),-nvme-smoke,)
ifeq ($(ARCH),armv7m)
BUILD_VARIANT := $(BUILD_VARIANT)-$(BOARD)-f$(STM32_FLASH_KB)k-r$(STM32_RAM_KB)k
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(filter 1,$(STM32_QEMU)),-qemu,)
endif
BUILD_DIR = .kernel-build/$(ARCH)-$(BOARD)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img
GUI_FAT32_IMG = $(BUILD_DIR)/gui-fat32.img
EXT4_IMG = $(BUILD_DIR)/ext4.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
ISOFS_IMG = $(BUILD_DIR)/isofs.img
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
WAYLAND_GUI_ARCHES := riscv64 loongarch64 aarch64 x86_64
WAYLAND_GUI ?= $(if $(filter $(ARCH),$(WAYLAND_GUI_ARCHES)),1,0)
GUI_MEDIA ?=
GUI_MEDIA_STAMP = $(BUILD_DIR)/.gui-media-id
WAYLAND_PLAYER_STAMP = user/build/wayland/$(ARCH)/stamp/player
WAYLAND_FFMPEG_STAMP = user/build/wayland/$(ARCH)/stamp/ffmpeg
WAYLAND_STUBS_STAMP = user/build/wayland/$(ARCH)/stamp/stubs
WAYLAND_WESTON_STAMP = user/build/wayland/$(ARCH)/stamp/weston
WAYLAND_WESTON_PATCH = user/wayland/patches/weston-a20.patch
GUI_WAYLAND_DEPS = $(if $(filter 1,$(WAYLAND_GUI)),$(WAYLAND_PLAYER_STAMP) user/wayland/install-image.sh $(if $(strip $(GUI_MEDIA)),$(GUI_MEDIA),),)
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
NATIVE_FUTEX_BIN       := $(NATIVE_BUILD_DIR)/native-futex-$(NATIVE_TAG)
NATIVE_DEBUG_BIN       := $(NATIVE_BUILD_DIR)/native-debug-$(NATIVE_TAG)
NATIVE_EXT_BIN          := $(NATIVE_BUILD_DIR)/native-ext-$(NATIVE_TAG)
NATIVE_MM_BIN          := $(NATIVE_BUILD_DIR)/native-mm-$(NATIVE_TAG)
NATIVE_SIGNAL_BIN      := $(NATIVE_BUILD_DIR)/native-signal-$(NATIVE_TAG)
NATIVE_IPC_BIN         := $(NATIVE_BUILD_DIR)/native-ipc-$(NATIVE_TAG)
NATIVE_CONTRACT_BIN    := $(NATIVE_BUILD_DIR)/native-contract-$(NATIVE_TAG)
NATIVE_SVCMAN_BIN      := $(NATIVE_BUILD_DIR)/svcman-$(NATIVE_TAG)
NATIVE_ECHOD_BIN       := $(NATIVE_BUILD_DIR)/svc-echod-$(NATIVE_TAG)
NATIVE_SHMRING_BIN     := $(NATIVE_BUILD_DIR)/native-shmring-$(NATIVE_TAG)
NATIVE_SHMRINGD_BIN    := $(NATIVE_BUILD_DIR)/shmringd-$(NATIVE_TAG)
NATIVE_CHAND_BIN       := $(NATIVE_BUILD_DIR)/chand-$(NATIVE_TAG)
NATIVE_RTCD_BIN        := $(NATIVE_BUILD_DIR)/native-rtcd-$(NATIVE_TAG)
NATIVE_RTCDD_BIN       := $(NATIVE_BUILD_DIR)/rtcd-$(NATIVE_TAG).a20drv
NATIVE_REGISTRY_BIN    := $(NATIVE_BUILD_DIR)/native-registry-$(NATIVE_TAG)
NATIVE_SVCMGR_BIN      := $(NATIVE_BUILD_DIR)/svcmgr-$(NATIVE_TAG)
NATIVE_ISOLATION_BIN   := $(NATIVE_BUILD_DIR)/native-isolation-$(NATIVE_TAG)
NATIVE_UBDD_BIN        := $(NATIVE_BUILD_DIR)/ubd-$(NATIVE_TAG).a20drv
NATIVE_UINPUTD_BIN     := $(NATIVE_BUILD_DIR)/uinputd-$(NATIVE_TAG).a20drv
NATIVE_PERSONALITY_BIN  := $(NATIVE_BUILD_DIR)/native-personality-$(NATIVE_TAG)
NATIVE_LINUX_BIN        := $(NATIVE_BUILD_DIR)/native-linux-$(NATIVE_TAG)
NATIVE_OUTPUTS         := $(NATIVE_HELLO_BIN) $(NATIVE_HANDLE_BIN) $(NATIVE_LIBC_BIN) $(NATIVE_FUTEX_BIN) $(NATIVE_MM_BIN) $(NATIVE_SIGNAL_BIN) $(NATIVE_IPC_BIN) $(NATIVE_CONTRACT_BIN) $(NATIVE_SVCMAN_BIN) $(NATIVE_ECHOD_BIN) $(NATIVE_SHMRING_BIN) $(NATIVE_SHMRINGD_BIN) $(NATIVE_CHAND_BIN) $(NATIVE_RTCD_BIN) $(NATIVE_RTCDD_BIN) $(NATIVE_REGISTRY_BIN) $(NATIVE_SVCMGR_BIN) $(NATIVE_ISOLATION_BIN) $(NATIVE_UBDD_BIN) $(NATIVE_UINPUTD_BIN) $(NATIVE_PERSONALITY_BIN) $(NATIVE_LINUX_BIN) $(NATIVE_DEBUG_BIN) $(NATIVE_EXT_BIN)
NATIVE_BUILD_STAMP     := $(NATIVE_BUILD_DIR)/.native-build-id
comma := ,
NET_HOSTFWD ?= hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
NETDEV_USER = -netdev user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
SMOKE_TIMEOUT ?= 20s
# TCG boot can take longer than two seconds after a full image rebuild.  Wait
# until the interactive mksh has had time to print its prompt before injecting
# smoke commands; PASS markers and clean poweroff still decide the result.
SMOKE_INPUT_DELAY ?= 8
# mm_stress drives ~8 MiB of ramfs page-cache eviction plus fork/mremap/huge
# page coverage under TCG; it is the heaviest smoke and needs a longer budget.
SMOKE_TIMEOUT_MM_ST ?= 45s
SMOKE_TIMEOUT_MM_FORK_EXEC ?= 120s
SMOKE_LOG_DIR ?= .kernel-build/smoke
STEP35_TIMEOUT ?= 300s
STEP35_INPUT_DELAY ?= 8
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
ARCH_CFLAGS_x86_64      := -m64 -mcmodel=large -mno-red-zone -fno-pic -fno-pie -mgeneral-regs-only -fno-omit-frame-pointer
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
QEMU_GUI_AUDIO_DRIVER ?= $(if $(filter Darwin,$(shell uname -s 2>/dev/null)),coreaudio,pa)
QEMU_GUI_AUDIO_DEVICE ?= hda

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
QEMU_GUI_AUDIO_HW_hda_x86_64 := -device intel-hda \
                                 -device hda-duplex,audiodev=a20audio
QEMU_GUI_AUDIO_HW_hda_riscv64 := -device intel-hda \
                                  -device hda-duplex,audiodev=a20audio
QEMU_GUI_AUDIO_HW_hda_loongarch64 := -device intel-hda \
                                      -device hda-duplex,audiodev=a20audio
QEMU_GUI_AUDIO_HW_virtio_x86_64 := -device virtio-sound-pci,audiodev=a20audio
QEMU_GUI_AUDIO_HW_virtio_riscv64 := -device virtio-sound-device,bus=virtio-mmio-bus.3,audiodev=a20audio
QEMU_GUI_AUDIO_HW_virtio_loongarch64 := -device virtio-sound-pci,audiodev=a20audio
QEMU_GUI_AUDIO_x86_64 = -audiodev driver=$(QEMU_GUI_AUDIO_DRIVER),id=a20audio $(QEMU_GUI_AUDIO_HW_$(QEMU_GUI_AUDIO_DEVICE)_x86_64)
QEMU_GUI_AUDIO_riscv64 = -audiodev driver=$(QEMU_GUI_AUDIO_DRIVER),id=a20audio $(QEMU_GUI_AUDIO_HW_$(QEMU_GUI_AUDIO_DEVICE)_riscv64)
QEMU_GUI_AUDIO_loongarch64 = -audiodev driver=$(QEMU_GUI_AUDIO_DRIVER),id=a20audio $(QEMU_GUI_AUDIO_HW_$(QEMU_GUI_AUDIO_DEVICE)_loongarch64)
QEMU_GUI_DEVICES_DEFAULT := -device virtio-gpu-device \
                            -device virtio-keyboard-device \
                            -device virtio-mouse-device

# Compiler and tools
CCACHE ?= $(shell command -v ccache 2>/dev/null)
CCACHE_PREFIX := $(if $(CCACHE),$(CCACHE) ,)
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
QEMU_GUI_AUDIO := $(QEMU_GUI_AUDIO_$(ARCH))

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
CC := $(CCACHE_PREFIX)$(CLANG_ARMV7M) --target=arm-none-eabi
OBJCOPY := $(LLVM_OBJCOPY_ARMV7M)
ARCH_LDFLAGS += -fuse-ld=lld
else
CC := $(CCACHE_PREFIX)$(CROSS_PREFIX)gcc
OBJCOPY := $(CROSS_PREFIX)objcopy
endif
else
CC := $(CCACHE_PREFIX)$(CROSS_PREFIX)gcc
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
CONFIG_UBSAN ?= $(if $(filter 1,$(BRINGUP)),0,1)
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
ifeq ($(filter 1,$(CONFIG_UBSAN)),1)
# Undefined Behavior Sanitizer: kernel/core/ubsan.c provides the handlers.
# alignment/bounds-strict are excluded to match the packed-struct and
# flexible-array idioms the kernel deliberately uses.
CFLAGS += -fsanitize=undefined -fno-sanitize=alignment,bounds-strict \
          -DCONFIG_UBSAN=1
endif
ifneq ($(strip $(WAIT_TIMER_HEAP_MAX)),)
CFLAGS += -DCONFIG_WAIT_TIMER_HEAP_MAX=$(WAIT_TIMER_HEAP_MAX)
endif
ifneq ($(NR_CPUS),1)
CFLAGS += -DCONFIG_SMP
endif
ifeq ($(COOPERATIVE_BOOT),1)
CFLAGS += -DCONFIG_COOPERATIVE_BOOT
endif
ifeq ($(BOARD),virtualbox-aarch64)
CFLAGS += -DCONFIG_COOPERATIVE_BOOT
endif
CFLAGS += $(DRIVER_DEPLOYMENT_CPPFLAGS)

# ------------------------------------------------------------------
# Hardware/board features consumed by common code.
#
# Feature names stay arch-agnostic so common code never branches on
# CONFIG_<architecture>; the check-arch-boundary gate enforces that.
# ------------------------------------------------------------------
ifeq ($(ARCH),x86_64)
CFLAGS += -DCONFIG_IOPORT -DCONFIG_AHCI \
          -DCONFIG_PCI_MMIO_BASE_LEGACY
endif
ifneq ($(filter x86_64 loongarch64 riscv64,$(ARCH)),)
CFLAGS += -DCONFIG_PCI_MMIO_ALLOC
endif
ifneq ($(filter loongarch64 riscv64,$(ARCH)),)
CFLAGS += -DCONFIG_PCI_MMIO_BASE_ECAM
endif
ifeq ($(ARCH),aarch64)
CFLAGS += -DCONFIG_TRAP_ESR_DIAG
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
# Driver ownership is kept outside this top-level file so deployment policy is
# explicit and generic built-in exceptions remain reviewable.
include tools/driver-sources.mk

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
             $(KERNEL_DIR)/proc/timer_heap.c \
             $(KERNEL_DIR)/proc/current.c \
             $(KERNEL_DIR)/proc/pid.c \
             $(KERNEL_DIR)/proc/proc.c \
             $(KERNEL_DIR)/proc/task.c \
             $(KERNEL_DIR)/proc/exit.c \
             $(KERNEL_DIR)/proc/signal.c \
             $(KERNEL_DIR)/proc/cg_cpu.c \
              $(KERNEL_DIR)/mm/nommu.c \
              $(KERNEL_DIR)/fs/fdtable.c \
	             $(KERNEL_DIR)/fs/diskfs/fat32lite.c \
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
             $(wildcard $(KERNEL_DIR)/fs/*/*.c) \
             $(wildcard $(KERNEL_DIR)/ipc/*.c) \
             $(wildcard $(KERNEL_DIR)/net/*.c) \
             $(wildcard $(KERNEL_DIR)/bpf/*.c) \
             $(wildcard $(KERNEL_DIR)/ext/*.c) \
             $(wildcard $(KERNEL_DIR)/drvmod/*.c) \
              $(DRIVER_KERNEL_SRCS) \
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

include tools/driver-modules.mk

# Object files
LWIP_KERNEL_SRC := $(filter $(KERNEL_DIR)/external/lwip/src/%.c,$(KERNEL_SRC))
KERNEL_OBJ = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter-out user/% $(KERNEL_DIR)/external/lwip/%,$(KERNEL_SRC))) \
              $(patsubst $(KERNEL_DIR)/external/lwip/src/%.c,$(BUILD_DIR)/external/lwip/src/%.o,$(LWIP_KERNEL_SRC))
KERNEL_OBJ += $(EARLY_DRIVER_BLOBS)

# vDSO user image (riscv64): built out-of-tree of ASM_SRC on purpose, it is
# user code linked with its own script.  The vdso.elf FILE is embedded
# verbatim: p_offset == p_vaddr makes file layout == memory layout, ELF
# header included (objcopy -O binary would strip the header and break
# musl's vDSO parser).
VDSO_CC   ?= $(CCACHE_PREFIX)$(RISCV_GNU_CC)
VDSO_SRC_DIR := $(KERNEL_DIR)/vdso/$(ARCH)
VDSO_ELF  := $(BUILD_DIR)/vdso/vdso.elf
VDSO_BLOB := $(BUILD_DIR)/vdso/vdso_blob.o
ifeq ($(ARCH),riscv64)
KERNEL_OBJ += $(VDSO_BLOB)
endif

$(VDSO_ELF): $(VDSO_SRC_DIR)/vdso.S $(VDSO_SRC_DIR)/vdso.ld \
             $(INCLUDE_DIR)/mm/vdso_layout.h
	@mkdir -p $(dir $@)
	$(VDSO_CC) -I$(INCLUDE_DIR) -nostdlib -nostartfiles -shared \
	    -Wl,--build-id=none \
	    -Wl,--hash-style=sysv -T $(VDSO_SRC_DIR)/vdso.ld -o $@ $<

$(VDSO_BLOB): $(VDSO_ELF)
	cd $(BUILD_DIR)/vdso && $(OBJCOPY) -I binary -O elf64-littleriscv \
	    -B riscv:rv64 vdso.elf vdso_blob.o

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
# Targets (split into tools/targets-*.mk)
# ================================================================

include tools/targets-base.mk
include tools/targets-build.mk
include tools/targets-gates.mk
include tools/targets-smoke.mk
include tools/targets-steps.mk
include tools/targets-dev.mk
include tools/stm32.mk
include tools/run-targets.mk
include tools/targets-images.mk
include tools/targets-extra.mk
include tools/targets-native.mk
include tools/targets-native-smoke.mk
include tools/targets-mlibc.mk
include tools/targets-eval.mk

# ================================================================
# Documentation
# ================================================================
# Build standard-reference.pdf from every tracked Markdown file under docs/. The generator validates all required tools and fonts and prints the platform-specific install commands if any is missing. See tools/gen_docs_pdf.sh for details.
.PHONY: docs
docs:
	@bash tools/gen_docs_pdf.sh

# A20OS top-level build

# ================================================================
# Host tools and platform defaults
# ================================================================

NPROC ?= $(or $(shell getconf _NPROCESSORS_ONLN 2>/dev/null),$(shell sysctl -n hw.logicalcpu 2>/dev/null),4)
PYTHON ?= $(if $(shell command -v conda 2>/dev/null),conda run -n a20os --no-capture-output python,python3)
TIMEOUT ?= $(PYTHON) tools/run_with_timeout.py
HOST_OS ?= $(shell uname -s 2>/dev/null)

ifeq ($(HOST_OS),Darwin)
DEFAULT_KERNEL_CHECK_TARGETS := check-riscv64-bringup check-stm32f103
DEFAULT_USER_CHECK_TARGETS := check-riscv64-user
DEFAULT_NATIVE_HANDLE_TARGETS := native-handle-test-rv
DEFAULT_NATIVE_LIBC_TARGETS := native-libc-rv
else
# Canonical hosted build matrix (BUILD_MATRIX_GATE_CONTRACT).  Every per-arch
# gate list below is derived from this single source of truth so an added or
# renamed arch cannot drift across targets.  loongarch32 is a kernel-only
# bring-up member (no QEMU target) and is tracked separately below.
SUPPORTED_HOSTED_ARCHES := riscv64 loongarch64 aarch64 x86_64 arm32 riscv32 ppc64le
# Native-ABI check targets use short arch names (riscv64 -> rv, loongarch64 ->
# la, riscv32 -> rv32; other arches keep their name).
NATIVE_ARCH_NAME = $(if $(filter riscv64,$(1)),rv,$(if $(filter loongarch64,$(1)),la,$(if $(filter riscv32,$(1)),rv32,$(1))))
NATIVE_ARCH_LIST = $(foreach a,$(SUPPORTED_HOSTED_ARCHES),$(call NATIVE_ARCH_NAME,$(a)))

DEFAULT_KERNEL_CHECK_TARGETS := $(foreach a,$(SUPPORTED_HOSTED_ARCHES),check-$(a)-bringup)
DEFAULT_USER_CHECK_TARGETS := $(foreach a,$(SUPPORTED_HOSTED_ARCHES),check-$(a)-user)
DEFAULT_NATIVE_HANDLE_TARGETS := $(foreach n,$(NATIVE_ARCH_LIST),native-handle-test-$(n))
DEFAULT_NATIVE_LIBC_TARGETS := $(foreach n,$(NATIVE_ARCH_LIST),native-libc-$(n))

# ================================================================
# CI kernel build matrix
# ================================================================
# .github/workflows/ci.yml resolves its per-arch kernel build matrix from
# `make -s print-ci-kernel-arches` instead of hardcoding an arch list in YAML.
# That is the point: a hand-maintained YAML list is a second source of truth
# that can silently fall behind SUPPORTED_HOSTED_ARCHES, which is exactly how
# arm32, riscv32 and ppc64le went unbuilt in CI while the local gate covered
# them.  print-ci-kernel-arches also rejects any member that is not a hosted
# arch, so a typo here fails loudly instead of spawning a job that cannot
# build.
#
# This is deliberately a strict subset of SUPPORTED_HOSTED_ARCHES, and the one
# remaining omission is recorded here rather than dropped silently:
#
#   loongarch32 LA32R has no distro cross-toolchain to install: Debian ships
#               no loongarch32 gcc, and the project builds a from-source
#               cloudspurs binutils/gcc la32 fork instead
#               (docs/platforms/loongarch32.md).  tools/ci/Dockerfile cannot
#               apt-get something that does not exist upstream, so CI cannot
#               build it without vendoring a toolchain build.  It is also not
#               a hosted arch, so it is out of SUPPORTED_HOSTED_ARCHES by
#               construction and check-kernel-build-all's separate
#               check-loongarch32-bringup stays local-only.
#
# arm32 used to be omitted here for exactly one reason, and that reason is now
# fixed rather than waived.  Makefile:866-869 below deliberately withholds
# ARCH_HAS_PGTABLE_OPS from arm32, which supplies its own short-descriptor
# page-table backend (kernel/arch/arm32/mm/pgtbl.c), so kernel/include/mm/pt.h
# never declares the transactional cursor there.  Two fault-path sites in
# kernel/mm/fault.c nevertheless called mm_addrspace_lock()/mm_cursor_map()/
# mm_cursor_query()/mm_cursor_unlock() outside any guard, which made
# check-arm32-bringup fail to compile with four implicit-declaration errors --
# and because nothing built arm32 in CI, nobody saw it.  Both sites now go
# through arch hooks in fault.c instead of the cursor directly: the
# fault-around window maps through fault_map_window(), which is one transaction
# where a cursor exists and a per-page pt_map() loop where one does not, so
# arm32 keeps the whole optimisation minus the single descent; and the
# status-driven fault path is compiled out where the per-PTE status sidecar
# does not exist, reported as absent (see
# FAULT_FROM_STATUS_ABSENT_WITHOUT_PGTABLE_OPS in fault.c) rather than declared
# against a backend that cannot implement it.  The guard in pt.h was NOT
# widened: a declared cursor with no implementation behind it is a fabricated
# capability, which fails silently instead of loudly.
#
# Every member below must have its cross toolchain installed by
# tools/ci/Dockerfile; riscv32 uses the rv32 multilib of
# gcc-riscv64-unknown-elf rather than a riscv32-specific package, which
# Makefile:329-341 detects via RISCV_ELF_RV32_MULTIDIR.
CI_KERNEL_ARCHES ?= riscv64 loongarch64 aarch64 x86_64 arm32 riscv32 ppc64le
endif

empty :=
space := $(empty) $(empty)
# subst's argument list is split on literal commas, so the one that separates
# JSON array elements has to hide inside a variable reference.
comma := ,

# Machine-readable form of CI_KERNEL_ARCHES, consumed by ci.yml's resolver job
# (strategy.matrix needs the list before any step of the matrix job can run,
# hence a separate job rather than a step inside it).
.PHONY: print-ci-kernel-arches
print-ci-kernel-arches:
	@for a in $(CI_KERNEL_ARCHES); do \
		case ' $(SUPPORTED_HOSTED_ARCHES) ' in \
			*" $$a "*) ;; \
			*) echo "error: CI_KERNEL_ARCHES member '$$a' is not in SUPPORTED_HOSTED_ARCHES ($(SUPPORTED_HOSTED_ARCHES))" >&2; exit 1 ;; \
		esac; \
	done; \
	printf 'arches=["%s"]\n' '$(subst $(space),"$(comma)",$(CI_KERNEL_ARCHES))'

# ================================================================
# Build configuration
# ================================================================

ARCH ?= riscv64
ABI ?= both
BRINGUP ?= 0
RAMFS_USER ?= 0
OPT ?= -O3
USER_OPT ?= $(OPT)
NR_CPUS ?= 1
EXTERNAL_ROOT ?= 0
COOPERATIVE_BOOT ?= 0
STORAGE_READ_ONLY ?= 0
ALLOW_UNVERIFIED_SMP ?= 0
SMP_VERIFIED_QEMU_ARCHES := riscv64 aarch64 loongarch64 x86_64
PROFILE ?= full
# Swap is on by default; the NOMMU override below forces it back off where
# there is no MMU to demand-page from.  A toggle is rebuild-safe without a
# BUILD_VARIANT component because BUILD_FLAGS_STAMP in tools/targets-images.mk
# keys on $(CFLAGS), which gains -DCONFIG_SWAP when this flips.
CONFIG_SWAP ?= y

# Synthetic driver lifecycle test and the HDA/NVMe in-probe smoke builds.
# All three default to off and are enabled with the on-value their existing
# callers already pass (tools/smoke_cases.py and docs/drivers/meta/
# testing-and-submission.md use `=y`), so the BUILD_VARIANT components below
# and the gate invocations keep working unchanged.
CONFIG_DRIVER_LIFECYCLE_TEST ?= 0
CONFIG_HDA_SMOKE_TEST ?= 0
CONFIG_NVME_SMOKE_TEST ?= 0

# Single knob for the CONFIG_HDA_SMOKE_TEST / CONFIG_NVME_SMOKE_TEST macros in
# the loadable driver packages.  Those macros are consumed *only* by
# kernel/drvmod/examples/{hda,nvme}.c, which tools/driver-modules.mk compiles
# with its own DRVMOD_CFLAGS; the built-in HDA/NVMe drivers they were migrated
# from are gone, so the kernel CFLAGS never carried them.  The per-driver
# CONFIG_* names above now only pick the BUILD_VARIANT component that keeps the
# smoke builds in their own artifact directory.
DRVMOD_SMOKE ?= 0

# STM32-specific configuration. These values are inert for other boards.
STM32_OPENOCD_INTERFACE ?= interface/cmsis-dap.cfg
STM32_OPENOCD_TRANSPORT ?= swd
STM32_OPENOCD_ADAPTER_KHZ ?= 1000
STM32_CMSIS_DAP_SERIAL ?=
STM32_BT_NAME ?= KasaneTeto
STM32_BT_PIN ?= 2233
STM32_BT_UUID ?= 1101
STM32_BT_BAUD ?= 38400
STM32_QEMU ?= 0
# Derive the artifact dirs from BUILD_VARIANT (f<flash>k-r<ram>k[-qemu]) so a
# change to STM32_FLASH_KB/RAM_KB/STM32_QEMU never desyncs the hardcoded paths.
STM32_XUANWU_BUILD_DIR = $(BUILD_DIR)
STM32_XUANWU_ELF = $(STM32_XUANWU_BUILD_DIR)/kernel.elf
STM32_QEMU_BUILD_DIR = $(BUILD_DIR)
STM32_QEMU_BIN = $(STM32_QEMU_BUILD_DIR)/kernel.bin
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

# Driver deployment depends on the selected architecture and board.
include tools/driver-deployment.mk

NOMMU ?= 0
NOMMU_SUPPORTED_ARCHES := riscv64 riscv32 aarch64 arm32 armv7m

# Swap needs a PTE encoding for the 64-bit swap entry (pte_is_swap /
# pte_to_swp_entry / swp_entry_to_pte in kernel/arch/<arch>/include/page_table.h).
# riscv32 and loongarch32 have not defined one, so CONFIG_SWAP=y does not merely
# leave them featureless there -- kernel/mm/{fault,mm,munmap}.c and
# kernel/proc/exit.c fail to compile.  Mirrors the NOMMU gate above: force the
# feature off outside the supported set instead of letting the build break.
SWAP_SUPPORTED_ARCHES := riscv64 loongarch64 aarch64 x86_64 arm32 ppc64le
ifeq ($(NOMMU),1)
CONFIG_SWAP := n
else
ifeq ($(filter $(CONFIG_SWAP),y),y)
ifeq ($(filter $(ARCH),$(SWAP_SUPPORTED_ARCHES)),)
CONFIG_SWAP := n
endif
endif
endif

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

# ================================================================
# Build paths and userspace artifacts
# ================================================================

KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include

# Preserve established generic and STM32 output paths used by smoke, release,
# flash, and QEMU runners. Options that change compiled code, including
# embedded deployment and cooperative boot, get distinct output directories.
BUILD_VARIANT = $(ABI)-$(if $(filter 1,$(BRINGUP)),bringup,dev)$(if $(filter 1,$(RAMFS_USER)),-ramfs-user,)$(if $(and $(filter embedded,$(DRIVER_DEPLOYMENT)),$(filter-out armv7m,$(ARCH))),-embedded,)$(if $(filter 1,$(COOPERATIVE_BOOT)),-cooperative,)$(if $(filter 1,$(STORAGE_READ_ONLY)),-storage-ro,)$(if $(filter 1,$(EXTERNAL_ROOT)),-external-root,)$(if $(filter 1,$(NOMMU)),-nommu,)$(if $(filter-out 1,$(NR_CPUS)),-smp$(NR_CPUS),)$(if $(filter y,$(CONFIG_DRIVER_LIFECYCLE_TEST)),-driver-lifecycle,)$(if $(filter y,$(CONFIG_HDA_SMOKE_TEST)),-hda-smoke,)$(if $(filter y,$(CONFIG_NVME_SMOKE_TEST)),-nvme-smoke,)
ifeq ($(ARCH),armv7m)
BUILD_VARIANT := $(BUILD_VARIANT)-$(BOARD)-f$(STM32_FLASH_KB)k-r$(STM32_RAM_KB)k
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(filter 1,$(STM32_QEMU)),-qemu,)
endif
BUILD_DIR = .kernel-build/$(ARCH)-$(BOARD)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img
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
# User software now comes from Alpine apk packages assembled by a world
# manifest (see packages/world/), not from source trees under user/external/.
# The extra *partition* survives only where a board needs a writable /extra,
# and an apk world image fills it -- see VF2_WORLD in tools/targets-build.mk.
# RISCV_GNU_CC stays: the vDSO is built with it, not with an extra package.
RISCV_GNU_CC ?= riscv64-linux-gnu-gcc
USER_BUILD_ID = $(ARCH):$(NOMMU):$(USER_OPT):$(PROFILE)
# $(wildcard) drops uninitialized git submodules (fastfetch).  The
# build must not fail when a tarball export or a non-recursive clone
# leaves those directories absent entirely.
USER_BUILD_CHECK_DIRS = $(wildcard user/init.c user/cmds user/init_common \
                        user/external/musl user/external/sbase user/external/mksh-cvs2git \
                        user/external/tlse user/external/apps/fastfetch)
NATIVE_TAG_riscv64     := rv
NATIVE_TAG_loongarch64 := la
NATIVE_TAG_loongarch32 := la32
NATIVE_TAG_aarch64     := aarch64
NATIVE_TAG_x86_64      := x86_64
NATIVE_TAG_arm32       := arm32
NATIVE_TAG_armv7m      := armv7m
NATIVE_TAG_riscv32     := rv32
NATIVE_TAG_ppc64le     := ppc64le
NATIVE_TAG             := $(NATIVE_TAG_$(ARCH))
NATIVE_BUILD_DIR       := $(USER_BUILD_DIR)
NATIVE_HANDLE_BIN      := $(NATIVE_BUILD_DIR)/native-handle-$(NATIVE_TAG)
NATIVE_LIBC_BIN        := $(NATIVE_BUILD_DIR)/native-libc-$(NATIVE_TAG)
NATIVE_FUTEX_BIN       := $(NATIVE_BUILD_DIR)/native-futex-$(NATIVE_TAG)
NATIVE_DEEPEN_BIN      := $(NATIVE_BUILD_DIR)/native-deepen-$(NATIVE_TAG)
NATIVE_DEBUG_BIN       := $(NATIVE_BUILD_DIR)/native-debug-$(NATIVE_TAG)
NATIVE_EXT_BIN         := $(NATIVE_BUILD_DIR)/native-ext-$(NATIVE_TAG)
NATIVE_MM_BIN          := $(NATIVE_BUILD_DIR)/native-mm-$(NATIVE_TAG)
NATIVE_SIGNAL_BIN      := $(NATIVE_BUILD_DIR)/native-signal-$(NATIVE_TAG)
NATIVE_IPC_BIN         := $(NATIVE_BUILD_DIR)/native-ipc-$(NATIVE_TAG)
NATIVE_CONTRACT_BIN    := $(NATIVE_BUILD_DIR)/native-contract-$(NATIVE_TAG)
NATIVE_FAKELD_BIN       := $(NATIVE_BUILD_DIR)/fakeld-$(NATIVE_TAG)
NATIVE_DYNPROBE_BIN     := $(NATIVE_BUILD_DIR)/dynprobe-$(NATIVE_TAG)
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
NATIVE_UFSD_BIN        := $(NATIVE_BUILD_DIR)/ufsd-$(NATIVE_TAG)
NATIVE_UINPUTD_BIN     := $(NATIVE_BUILD_DIR)/uinputd-$(NATIVE_TAG).a20drv
NATIVE_UEDUD_BIN       := $(NATIVE_BUILD_DIR)/uedud-$(NATIVE_TAG).a20drv
NATIVE_PERSONALITY_BIN := $(NATIVE_BUILD_DIR)/native-personality-$(NATIVE_TAG)
NATIVE_LINUX_BIN       := $(NATIVE_BUILD_DIR)/native-linux-$(NATIVE_TAG)
NATIVE_CHESS_BIN       := $(NATIVE_BUILD_DIR)/native-chess-$(NATIVE_TAG)
NATIVE_OUTPUTS         := $(NATIVE_HANDLE_BIN) \
                          $(NATIVE_LIBC_BIN) $(NATIVE_FUTEX_BIN) $(NATIVE_DEEPEN_BIN) \
                          $(NATIVE_MM_BIN) $(NATIVE_SIGNAL_BIN) \
                          $(NATIVE_IPC_BIN) $(NATIVE_CONTRACT_BIN) \
                          $(NATIVE_SVCMAN_BIN) $(NATIVE_ECHOD_BIN) \
                          $(NATIVE_SHMRING_BIN) $(NATIVE_SHMRINGD_BIN) \
                          $(NATIVE_CHAND_BIN) $(NATIVE_RTCD_BIN) \
                          $(NATIVE_RTCDD_BIN) $(NATIVE_REGISTRY_BIN) \
                          $(NATIVE_SVCMGR_BIN) $(NATIVE_ISOLATION_BIN) \
                          $(NATIVE_UBDD_BIN) $(NATIVE_UINPUTD_BIN) \
                          $(NATIVE_UEDUD_BIN) \
                          $(NATIVE_UFSD_BIN) \
                          $(NATIVE_PERSONALITY_BIN) $(NATIVE_LINUX_BIN) \
                          $(NATIVE_DEBUG_BIN) $(NATIVE_EXT_BIN) \
                          $(NATIVE_CHESS_BIN)
# fakeld/dynprobe are the rv64 dynamic-linking bring-up probes (08-runtime-status
# §8a): fake_ld.c's _start_dyn entry asm is rv64-only, so building them for any
# other ARCH breaks the whole native-program graph. Keep them rv64-only.
NATIVE_OUTPUTS += $(if $(filter riscv64,$(ARCH)),$(NATIVE_FAKELD_BIN) $(NATIVE_DYNPROBE_BIN))
NATIVE_BUILD_STAMP     := $(NATIVE_BUILD_DIR)/.native-build-id

# ================================================================
# Runtime and smoke-test configuration
# ================================================================

comma := ,
# No implicit host port forwarding.  A host port is a shared, contended
# resource, and this default made every QEMU launch -- including the dozens of
# serial-console smoke gates that never open an inbound connection -- claim
# 5555, so two runs could never be in flight at once and the loser died with
# QEMU's "could not set up host forwarding rule" instead of anything
# diagnosable.  Forwarding is now declared per instance via [net].hostfwd, and
# tools/a20 checks the declared ports are free before starting (waiting, like
# the memory/CPU preflight, when they are not).
NET_HOSTFWD ?=
NETDEV_USER = -netdev user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
SMOKE_TIMEOUT ?= 20s
# TCG boot can take longer than two seconds after a full image rebuild.  Wait
# until the interactive mksh has had time to print its prompt before injecting
# smoke commands; PASS markers and clean poweroff still decide the result.
SMOKE_INPUT_DELAY ?= 8
# A 4-core TCG run plus net_stress_test's 4 concurrent x 4 MiB transfers is
# much slower than the single-core defaults, so it needs its own budget.
SMOKE_TIMEOUT_SMP ?= 180s
SMOKE_LOG_DIR ?= .kernel-build/smoke
STEP35_TIMEOUT ?= 300s
STEP35_LOG_DIR ?= .kernel-build/smoke/step35
WAIT_TIMER_HEAP_MAX ?=
REQUIRE_TIMEOUT_CAPACITY ?= 0
REQUIRE_SMP_RUNQUEUE ?= 0
REQUIRE_LOCK_SPLIT ?= 0
QEMU_MEMORY ?= 1G

# Files installed into generated filesystem images.
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

# ================================================================
# Architecture toolchains and properties
# ================================================================

RISCV_ELF_PREFIX ?= $(if $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null),riscv64-unknown-elf-,$(if $(shell command -v riscv64-elf-gcc 2>/dev/null),riscv64-elf-,riscv64-unknown-elf-))
RISCV_ELF_RV32_MULTIDIR := $(shell if command -v $(RISCV_ELF_PREFIX)gcc >/dev/null 2>&1; then \
	$(RISCV_ELF_PREFIX)gcc -march=rv32imafdc -mabi=ilp32d -print-multi-directory 2>/dev/null; \
fi)
CROSS_PREFIX_riscv64     := $(RISCV_ELF_PREFIX)
CROSS_PREFIX_loongarch64 := loongarch64-linux-gnu-
CROSS_PREFIX_loongarch32 := $(if $(shell command -v loongarch32-linux-gnu-gcc 2>/dev/null),loongarch32-linux-gnu-,loongarch32-unknown-elf-)
CROSS_PREFIX_aarch64     := aarch64-linux-gnu-
CROSS_PREFIX_x86_64      := x86_64-linux-gnu-
CROSS_PREFIX_arm32       := $(if $(shell command -v arm-linux-gnueabihf-gcc 2>/dev/null),arm-linux-gnueabihf-,$(if $(shell command -v arm-none-eabi-gcc 2>/dev/null),arm-none-eabi-,arm-linux-gnueabihf-))
CROSS_PREFIX_armv7m      := $(if $(shell command -v arm-none-eabi-gcc 2>/dev/null),arm-none-eabi-,)
CROSS_PREFIX_riscv32     := $(if $(filter-out .,$(RISCV_ELF_RV32_MULTIDIR)),$(RISCV_ELF_PREFIX),$(if $(shell command -v riscv32-linux-gnu-gcc 2>/dev/null),riscv32-linux-gnu-,riscv64-linux-gnu-))
CROSS_PREFIX_ppc64le     := powerpc64le-linux-gnu-

ARCH_CFLAGS_riscv64     := -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany
ARCH_CFLAGS_loongarch64 := -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static -mstrict-align
ARCH_CFLAGS_loongarch32 := -march=la32v1.0 -mabi=ilp32s -mcmodel=normal -fno-pic -static -fno-store-merging
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
PHYS_BASE_loongarch32 := 0x80000000

ifeq ($(NOMMU),1)
LDFLAGS_NOMMU := -Wl,--defsym=VIRT_BASE=$(PHYS_BASE_$(ARCH))
ARCH_CFLAGS_aarch64 += -mstrict-align
endif

ARCH_LDFLAGS_riscv64     :=
ARCH_LDFLAGS_loongarch64 := -static -no-pie
ARCH_LDFLAGS_loongarch32 := -static -no-pie
ARCH_LDFLAGS_aarch64     := -static -no-pie
ARCH_LDFLAGS_x86_64      := -static -no-pie
ARCH_LDFLAGS_arm32       := -static -no-pie
ARCH_LDFLAGS_armv7m      := -static -Wl,--gc-sections
ARCH_LDFLAGS_riscv32     := -static -no-pie -Wl,-m,elf32lriscv
ARCH_LDFLAGS_ppc64le     := -static -no-pie

ARCH_LIBS_riscv64     :=
ARCH_LIBS_loongarch64 :=
ARCH_LIBS_loongarch32 := $(shell $(CROSS_PREFIX_loongarch32)gcc $(ARCH_CFLAGS_loongarch32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_aarch64     :=
ARCH_LIBS_x86_64      :=
ARCH_LIBS_arm32       := $(shell $(CROSS_PREFIX_arm32)gcc $(ARCH_CFLAGS_arm32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_armv7m      :=
ARCH_LIBS_riscv32     := $(shell $(CROSS_PREFIX_riscv32)gcc $(ARCH_CFLAGS_riscv32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_ppc64le     :=

# ================================================================
# QEMU configuration
# ================================================================

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

# ---- GPU device selection -------------------------------------------------
# GPU_3D=1 selects the virgl-capable virtio-gpu variant so the host offers
# VIRTIO_GPU_F_VIRGL and the guest's 3D passthrough path becomes reachable.
# The plain variant is 2D-only: the guest still scans out, but every 3D ioctl
# fails with -ENXIO.
#
# This only changes what the *host* offers.  virglrenderer runs host-side, so
# a TCG guest still gets host-GPU rendering — which is the only practical way
# to exercise the 3D path on riscv64/aarch64/arm32, where no KVM exists.
# It does, however, require host virgl support (libvirglrenderer + a GL/EGL
# capable host display); under a headless host use QEMU_GUI_DISPLAY=egl-headless.
GPU_3D ?= 0
ifeq ($(GPU_3D),1)
QEMU_GPU_riscv64     := virtio-gpu-gl-device,bus=virtio-mmio-bus.7
QEMU_GPU_loongarch64 := virtio-gpu-gl-pci
QEMU_GPU_aarch64     := virtio-gpu-gl-device,bus=virtio-mmio-bus.7
QEMU_GPU_x86_64      := virtio-gpu-gl-pci
QEMU_GPU_arm32       := virtio-gpu-gl-device,bus=virtio-mmio-bus.7
QEMU_GPU_riscv32     := virtio-gpu-gl-device,bus=virtio-mmio-bus.7
QEMU_GPU_ppc64le     := virtio-gpu-gl-pci
QEMU_GPU_DEFAULT     := virtio-gpu-gl-device
else
QEMU_GPU_riscv64     := virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GPU_loongarch64 := virtio-gpu-pci
QEMU_GPU_aarch64     := virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GPU_x86_64      := virtio-gpu-pci
QEMU_GPU_arm32       := virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GPU_riscv32     := virtio-gpu-device,bus=virtio-mmio-bus.7
QEMU_GPU_ppc64le     := virtio-gpu-pci
QEMU_GPU_DEFAULT     := virtio-gpu-device
endif
QEMU_GPU := $(if $(QEMU_GPU_$(ARCH)),$(QEMU_GPU_$(ARCH)),$(QEMU_GPU_DEFAULT))

# A virtio-mmio bus accepts only one device.  Keep an optional second disk off
# the primary disk's bus; PCI transports can continue to use automatic slots.
QEMU_BLK_SECOND_riscv64     := virtio-blk-device,bus=virtio-mmio-bus.1
QEMU_BLK_SECOND_loongarch64 := virtio-blk-pci
QEMU_BLK_SECOND_x86_64      := virtio-blk-pci

QEMU_NET_riscv64     := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_loongarch64 := virtio-net-pci
QEMU_NET_aarch64     := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_x86_64      := virtio-net-pci
QEMU_NET_arm32       := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_riscv32     := virtio-net-device,bus=virtio-mmio-bus.4
QEMU_NET_ppc64le     := virtio-net-pci

QEMU_GUI_DEVICES_aarch64 := -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
                            -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
                            -device $(QEMU_GPU)
QEMU_GUI_DEVICES_riscv64 := -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
                            -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
                            -device $(QEMU_GPU)
QEMU_GUI_DEVICES_arm32 := -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
                          -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
                          -device $(QEMU_GPU)
# x86_64 input comes from the PS/2 controller (QEMU's default keyboard/mouse
# injection target); the ps2 drvmod publishes its ring to /dev/event0.
# The keyboard and mouse are not optional decoration here.  wlroots' multi
# backend tries libinput first and aborts the whole session when it finds no
# input devices -- it does not fall through to DRM -- so a GUI instance without
# them never reaches a display at all.  start-xfce4-session does not set
# WLR_LIBINPUT_NO_DEVICES because the desktop is meant to be usable, which means
# the devices have to actually be attached.
QEMU_GUI_DEVICES_x86_64 := -vga none \
                           -device $(QEMU_GPU) \
                           -device virtio-keyboard-pci \
                           -device virtio-mouse-pci
QEMU_GUI_DEVICES_loongarch64 := -vga none \
                                  -device $(QEMU_GPU) \
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
QEMU_GUI_DEVICES_DEFAULT := -device $(QEMU_GPU) \
                            -device virtio-keyboard-device \
                            -device virtio-mouse-device

# ================================================================
# Selected build tools and compiler flags
# ================================================================

CCACHE ?= $(shell command -v ccache 2>/dev/null)
CCACHE_PREFIX := $(if $(CCACHE),$(CCACHE) ,)
CROSS_PREFIX := $(CROSS_PREFIX_$(ARCH))
ARCH_CFLAGS  := $(ARCH_CFLAGS_$(ARCH))
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH))
ARCH_LIBS    := $(ARCH_LIBS_$(ARCH))
QEMU         := $(QEMU_$(ARCH))
# The virgl 3D renderer runs on the HOST: QEMU dlopen()s libvirglrenderer and
# feeds it the guest's command stream, so which libvirglrenderer the loader
# finds decides whether Mesa's virtio_gpu_dri.so can attach at all.  Some
# distributions still ship only 1.1.0 (2020), whose capset is too old for a
# current Mesa, so tools/build-virglrenderer.sh installs a new one under a
# local prefix.  Point QEMU_LIBPATH at that prefix's lib directory, or leave
# it empty to use the system one.  Applied as an env prefix rather than via
# -L because -L is QEMU's *data* search path, not its shared-library path.
# meson installs natively under lib/<triplet>/, so both layouts are probed.
# Pointing at a directory that holds no library produces an LD_LIBRARY_PATH
# that silently does nothing, and the symptom is the stale system library still
# being loaded -- which looks exactly like "the build did not take effect".
QEMU_VIRGL_LIB := $(firstword $(wildcard tools/virgl/install/lib/libvirglrenderer.so.1) \
                   $(wildcard tools/virgl/install/lib/*-linux-gnu/libvirglrenderer.so.1))
QEMU_LIBPATH ?= $(patsubst %/,%,$(abspath $(dir $(QEMU_VIRGL_LIB))))
ifneq ($(strip $(QEMU_LIBPATH)),)
QEMU         := env LD_LIBRARY_PATH=$(QEMU_LIBPATH):$$LD_LIBRARY_PATH $(QEMU)
endif
QEMU_FLAGS   := $(QEMU_FLAGS_BASE_$(ARCH)) -m $(QEMU_MEMORY) -nographic -smp $(NR_CPUS)
# Virtualization back-end.  Non-x86_64 targets run under QEMU TCG
# (multi-threaded once SMP).  x86_64 guests on an x86_64 host prefer KVM
# when /dev/kvm is present, falling back to TCG otherwise.
QEMU_TCG_ACCEL ?= tcg,thread=multi
QEMU_ACCEL_x86_64 := $(if $(wildcard /dev/kvm),kvm,$(QEMU_TCG_ACCEL))
QEMU_ACCEL := $(if $(QEMU_ACCEL_$(ARCH)),$(QEMU_ACCEL_$(ARCH)),$(if $(filter-out 1,$(NR_CPUS)),$(QEMU_TCG_ACCEL)))
ifneq ($(QEMU_ACCEL),)
QEMU_FLAGS += -accel $(QEMU_ACCEL)
endif

QEMU_BLK     := $(QEMU_BLK_$(ARCH))
QEMU_BLK_SECOND := $(QEMU_BLK_SECOND_$(ARCH))
QEMU_NET     := $(QEMU_NET_$(ARCH))
QEMU_GUI_DEVICES := $(if $(QEMU_GUI_DEVICES_$(ARCH)),$(QEMU_GUI_DEVICES_$(ARCH)),$(QEMU_GUI_DEVICES_DEFAULT))
QEMU_GUI_AUDIO := $(QEMU_GUI_AUDIO_$(ARCH))

# Arbitrary extra QEMU arguments, appended verbatim to every launch.  This is
# the single hook for instance manifests (machine.extra_qemu) to add devices
# without a Makefile change; the GUI path appends it after the built-in
# device set so it can add rather than replace.  Set by tools/a20.
EXTRA_QEMU ?=
ifneq ($(strip $(EXTRA_QEMU)),)
QEMU_FLAGS += $(EXTRA_QEMU)
endif

# DISPLAY_MODE=gui replaces -nographic with a real display plus the per-arch
# GUI device set (keyboard/mouse/GPU).  Needed by any test that has to see a
# display device: -nographic suppresses the display, and text mode attaches no
# virtio-gpu at all, so a GPU test there would be vacuous.  -serial stdio is
# kept because the smoke harness injects commands and matches the log there.
DISPLAY_MODE ?= text
ifeq ($(DISPLAY_MODE),gui)
QEMU_FLAGS := $(filter-out -nographic,$(QEMU_FLAGS)) -display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) -serial stdio
endif

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
# Snapshot the flags before an optional second disk is appended, so the
# world-image targets can add theirs without inheriting an strays' disk.
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
# Benchmark/release kernels are production builds.  Keeping UBSAN
# enabled there instruments the VFS/MM/syscall hot paths and makes
# benchmark runs measure diagnostics rather than the kernel.  Development
# profiles keep the sanitizer by default; bring-up and benchmark profiles
# opt out explicitly.
CONFIG_UBSAN ?= $(if $(filter 1,$(BRINGUP)),0,$(if $(filter benchmark,$(PROFILE)),0,1))
# Warnings are errors for the kernel: a warning that slips into a release build
# is a regression.  Unverified arches that still carry warnings can build with
# KERNEL_WERROR=0 while they are cleaned up.
KERNEL_WERROR ?= 1
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
ifeq ($(filter 1,$(KERNEL_WERROR)),1)
CFLAGS += -Werror
endif

# Escape hatch for experiment knobs that cannot come from the command line.
# x86_64 is the motivating case: QEMU does not publish -append through fw_cfg
# on the -kernel <ELF> boot path (docs 10.31/10.32), so a20.* parameters are
# unreachable there.  Building two images with
# EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=<n> and interleaving the runs gives
# the same time-adjacent A/B that a runtime switch gives elsewhere.
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)
# 内核栈金丝雀：__stack_chk_guard/__stack_chk_fail 由 kernel/core/stack_protector.c
# 提供；汇编（entry/trampoline/vdso）无 canary。lwip 等 vendored 代码在
# tools/targets-images.mk 的专属规则里显式 -fno-stack-protector，不受影响。
CONFIG_STACK_PROTECTOR ?= 1
ifeq ($(filter 1,$(CONFIG_STACK_PROTECTOR)),1)
CFLAGS += -fstack-protector-strong -DCONFIG_STACK_PROTECTOR=1
endif
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
# pbuf pool canary.  Off by default because it costs a comparison per pool
# operation; turn it on when a pbuf refcount or ownership bug is being hunted,
# since without it the pool has no way to notice a block being handed out twice.
CONFIG_LWIP_MEMP_OVERFLOW_CHECK ?= 0
ifeq ($(filter 1,$(CONFIG_LWIP_MEMP_OVERFLOW_CHECK)),1)
CFLAGS += -DCONFIG_LWIP_MEMP_OVERFLOW_CHECK=1
endif
ifneq ($(NR_CPUS),1)
CFLAGS += -DCONFIG_SMP
endif
ifeq ($(COOPERATIVE_BOOT),1)
CFLAGS += -DCONFIG_COOPERATIVE_BOOT
endif
ifeq ($(STORAGE_READ_ONLY),1)
CFLAGS += -DCONFIG_STORAGE_READ_ONLY -DCONFIG_AHCI
endif
# ARM32's short-descriptor abort path cannot safely demand-page large GUI
# executables yet; eager loading also handles their shared PT_LOAD tail page.
ifeq ($(ARCH),arm32)
CFLAGS += -DCONFIG_ELF_EAGER_LOAD
endif
ifeq ($(BOARD),ls2k1000)
ifeq ($(COOPERATIVE_BOOT),1)
CFLAGS += -DCONFIG_ELF_EAGER_LOAD
ifneq ($(NR_CPUS),1)
$(error COOPERATIVE_BOOT=1 is the single-core recovery profile and cannot be combined with NR_CPUS=$(NR_CPUS))
endif
ifeq ($(STORAGE_READ_ONLY),1)
$(error STORAGE_READ_ONLY=1 is an experiment and cannot be combined with the LS2K1000 cooperative recovery profile)
endif
endif
endif
ifeq ($(BOARD),virtualbox-aarch64)
CFLAGS += -DCONFIG_COOPERATIVE_BOOT
endif
CFLAGS += $(DRIVER_DEPLOYMENT_CPPFLAGS)
ifeq ($(EXTERNAL_ROOT),1)
CFLAGS += -DCONFIG_EXTERNAL_ROOT
endif

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
ELF_MACHINE_loongarch32 := 258
ELF_MACHINE_aarch64     := 183
ELF_MACHINE_x86_64      := 62
ELF_MACHINE_arm32       := 40
ELF_MACHINE_riscv32     := 243
ELF_MACHINE_ppc64le     := 21
ELF_CLASS_riscv64       := 2
ELF_CLASS_loongarch64   := 2
ELF_CLASS_loongarch32   := 1
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
ifeq ($(filter $(ARCH),arm32 armv7m riscv32 loongarch32),)
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

# Bringup / ramfs-user mode markers for conditional compilation.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif
ifeq ($(RAMFS_USER),1)
CFLAGS += -DCONFIG_RAMFS_USER
endif

ifeq ($(NOMMU),1)
CFLAGS += -DCONFIG_NOMMU
endif

# Synthetic driver lifecycle test (disabled by default).
ifeq ($(CONFIG_DRIVER_LIFECYCLE_TEST),y)
CFLAGS += -DCONFIG_DRIVER_LIFECYCLE_TEST
endif

# CONFIG_HDA_SMOKE_TEST / CONFIG_NVME_SMOKE_TEST deliberately add nothing to
# the kernel CFLAGS: the built-in HDA/NVMe drivers were migrated into the
# loadable packages, so these macros are reached only through DRVMOD_SMOKE in
# tools/driver-modules.mk.  The names survive here solely for the BUILD_VARIANT
# components that isolate the smoke artifacts.

ifeq ($(CONFIG_SWAP),y)
CFLAGS += -DCONFIG_SWAP
endif

# RISC-V IOMMU probe: accept QEMU <= 10.0's unshifted TR_RESPONSE.PPN field.
# QEMU fixed this on master (set_field); set to 0 once the required QEMU
# baseline no longer needs the exception.
CONFIG_IOMMU_TRRESP_LEGACY_PPN ?= 1
ifeq ($(CONFIG_IOMMU_TRRESP_LEGACY_PPN),1)
CFLAGS += -DCONFIG_IOMMU_TRRESP_LEGACY_PPN
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

# ================================================================
# Kernel sources and build graph
# ================================================================

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
CFLAGS += -Os -DCONFIG_MCU -DCONFIG_KLOG_BUF_SIZE=256
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
             $(KERNEL_DIR)/core/stack_protector.c \
             $(KERNEL_DIR)/proc/sched.c \
             $(KERNEL_DIR)/proc/park.c \
             $(KERNEL_DIR)/proc/timer_heap.c \
             $(KERNEL_DIR)/proc/current.c \
             $(KERNEL_DIR)/proc/pid.c \
             $(KERNEL_DIR)/proc/proc.c \
             $(KERNEL_DIR)/proc/task.c \
             $(KERNEL_DIR)/proc/exit.c \
             $(KERNEL_DIR)/proc/signal.c \
             $(KERNEL_DIR)/proc/cg_cpu.c \
             $(KERNEL_DIR)/mm/nommu.c \
             $(KERNEL_DIR)/fs/diskfs/fat32lite.c \
             $(if $(filter 1,$(STM32_QEMU)),$(BOARD_DRIVER_DIR)/stm32_uart.c $(BOARD_DRIVER_DIR)/extsram.c,$(wildcard $(BOARD_DRIVER_DIR)/*.c)) \
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
# The generated source/header are checked in intentionally so archive builds do
# not depend on Python being present. After editing
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

# Optional self-contained userspace for physical-board bring-up.  Each static
# ELF is linked as read-only kernel data, then copied into the root ramfs by
# kernel/fs/rootfs_user.c.  Keep this opt-in so normal disk-backed images do
# not grow and existing submission artifacts remain unchanged.
RAMFS_USER_PROGRAMS := init mksh help ls cat ps sleep timer_preempt timer_idle
ifeq ($(STORAGE_READ_ONLY),1)
RAMFS_USER_PROGRAMS += storage_read_test
endif
RAMFS_USER_BLOB_DIR := $(BUILD_DIR)/rootfs-user
RAMFS_USER_BLOBS := $(addprefix $(RAMFS_USER_BLOB_DIR)/,$(addsuffix .o,$(RAMFS_USER_PROGRAMS)))
RAMFS_USER_OBJCOPY_loongarch64 := -O elf64-loongarch -B loongarch
ifeq ($(RAMFS_USER),1)
ifneq ($(ARCH),loongarch64)
$(error RAMFS_USER=1 is currently supported only for ARCH=loongarch64)
endif
KERNEL_OBJ += $(RAMFS_USER_BLOBS)
endif

# vDSO user image (riscv64/loongarch64/x86_64/aarch64/ppc64le): built
# out-of-tree of ASM_SRC on purpose, it is user code linked with its own
# script.  The vdso.elf FILE is embedded verbatim: p_offset == p_vaddr makes
# file layout == memory layout, ELF header included (objcopy -O binary would
# strip the header and break musl's vDSO parser).
VDSO_CC_riscv64       ?= $(CCACHE_PREFIX)$(RISCV_GNU_CC)
VDSO_CC_loongarch64   ?= $(CCACHE_PREFIX)loongarch64-linux-gnu-gcc
VDSO_CC_x86_64        ?= $(CCACHE_PREFIX)x86_64-linux-gnu-gcc
VDSO_CC_aarch64       ?= $(CCACHE_PREFIX)aarch64-linux-gnu-gcc
VDSO_CC_ppc64le       ?= $(CCACHE_PREFIX)powerpc64le-linux-gnu-gcc
VDSO_CC               ?= $(VDSO_CC_$(ARCH))
VDSO_OBJCOPY_riscv64     := -O elf64-littleriscv -B riscv:rv64
VDSO_OBJCOPY_loongarch64 := -O elf64-loongarch -B loongarch
VDSO_OBJCOPY_x86_64      := -O elf64-x86-64 -B i386:x86-64
VDSO_OBJCOPY_aarch64     := -O elf64-littleaarch64 -B aarch64
VDSO_OBJCOPY_ppc64le     := -O elf64-powerpcle -B powerpc:common64
VDSO_SRC_DIR := $(KERNEL_DIR)/vdso/$(ARCH)
VDSO_ELF  := $(BUILD_DIR)/vdso/vdso.elf
VDSO_BLOB := $(BUILD_DIR)/vdso/vdso_blob.o
ifneq ($(filter riscv64 loongarch64 x86_64 aarch64 ppc64le,$(ARCH)),)
KERNEL_OBJ += $(VDSO_BLOB)
endif

$(VDSO_ELF): $(VDSO_SRC_DIR)/vdso.S $(VDSO_SRC_DIR)/vdso.ld \
             $(INCLUDE_DIR)/mm/vdso_layout.h
	@mkdir -p $(dir $@)
	$(VDSO_CC) -I$(INCLUDE_DIR) -nostdlib -nostartfiles -shared \
	    -Wl,--build-id=none \
	    -Wl,--hash-style=sysv -T $(VDSO_SRC_DIR)/vdso.ld -o $@ $<

$(VDSO_BLOB): $(VDSO_ELF)
	cd $(BUILD_DIR)/vdso && $(OBJCOPY) -I binary $(VDSO_OBJCOPY_$(ARCH)) \
	    vdso.elf vdso_blob.o

# ASM sources
ASM_SRC = $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.S' | sort)
ASM_OBJ = $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC))
DEP_FILES = $(filter-out $(RAMFS_USER_BLOBS:.o=.d),$(KERNEL_OBJ:.o=.d)) \
            $(ASM_OBJ:.o=.d)

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
include tools/target-console.mk
include tools/run-targets.mk
include tools/targets-images.mk
include tools/targets-rootfs.mk
include tools/targets-pkg.mk
include tools/targets-distro.mk
include tools/targets-extra.mk
include tools/targets-native.mk
include tools/targets-native-smoke.mk
include tools/targets-mlibc.mk

# ================================================================
# Documentation
# ================================================================
# Build standard-reference.pdf from every tracked Markdown file under docs/.
# The generator validates required tools and fonts and reports platform-specific
# installation commands when a dependency is missing.
.PHONY: docs
docs:
	@bash tools/gen_docs_pdf.sh

# ================================================================
# Envelope choke-point coverage gate (docs/research/08 §4)
# ================================================================
# Regenerates the coverage matrix from syscall_table.def and fails when
# the committed copy drifts -- a newly registered syscall must be
# explicitly classified before the gate passes again.
.PHONY: check-envelope-coverage
check-envelope-coverage:
	@$(PYTHON) tools/gensync.py envelope-coverage \
		--file docs/research/verification/envelope_coverage.md


# The binaries the native stamp rule requires.  make owns this list so the gate
# and the build cannot disagree about what "built" means.  This is exactly
# the set the shell rule tested; the Makefile defines more NATIVE_*_BIN vars
# for other purposes and those are deliberately not part of the gate.
# The stamp existence check must cover exactly what native-programs builds,
# otherwise binaries can go missing from the image while the stamp still passes.
NATIVE_BINS := $(NATIVE_OUTPUTS)

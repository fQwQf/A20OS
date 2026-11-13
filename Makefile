# A20OS top-level build

# ================================================================
# Host tools and platform defaults
# ================================================================

NPROC ?= $(or $(shell getconf _NPROCESSORS_ONLN 2>/dev/null),$(shell sysctl -n hw.logicalcpu 2>/dev/null),4)
PYTHON ?= python3
TIMEOUT ?= $(PYTHON) tools/run_with_timeout.py
HOST_OS ?= $(shell uname -s 2>/dev/null)

# Every temporary file a make-driven step produces stays inside the checkout
# so `make clean` can remove it. The host /tmp is often a small tmpfs: staging
# there once broke image-world mid-apk (No space left on device) while the
# repository disk had tens of GiB free, and nothing in `make clean` could
# reach those leftovers. Absolute path so recursive makes with a different
# cwd resolve it to the same directory. Overridable on the make command line
# (make TMPDIR=...) but not by the ambient environment.
TMPDIR := $(CURDIR)/build/tmp
export TMPDIR
$(shell mkdir -p $(TMPDIR))

ifeq ($(HOST_OS),Darwin)
DEFAULT_KERNEL_CHECK_TARGETS := check-riscv64-bringup check-stm32f103
DEFAULT_USER_CHECK_TARGETS := check-riscv64-user
DEFAULT_NATIVE_HANDLE_TARGETS := native-handle-test-rv
DEFAULT_NATIVE_LIBC_TARGETS := native-libc-rv
else
# Canonical hosted build matrix (BUILD_MATRIX_GATE_CONTRACT).  Every per-arch
# gate list below is derived from this single source of truth so an added or
# renamed arch cannot drift across targets.  Retired LoongArch32/PPC64LE
# implementations remain in the source tree for historical reference, but do
# not participate in active build, run, or test matrices.
SUPPORTED_HOSTED_ARCHES := riscv64 loongarch64 aarch64 x86_64 arm32 riscv32
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
# Every member below must have its cross toolchain installed by
# tools/ci/Dockerfile; riscv32 uses the rv32 multilib of
# gcc-riscv64-unknown-elf rather than a riscv32-specific package, which
# Makefile:329-341 detects via RISCV_ELF_RV32_MULTIDIR.
CI_KERNEL_ARCHES ?= riscv64 loongarch64 aarch64 x86_64 arm32 riscv32
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

# The declarative half of kernel trimming (profiles' curated source lists,
# capability arch matrices, per-arch feature defines) lives in
# components/trim.toml; this fragment is generated from it by
# `make regen-trim-fragment` and `make check-trim-registry` fails while it is
# stale.  The gates below read its variables; the force-off / $(error) logic
# itself stays here, in the build engine.
include components/trim.mk

ARCH ?= riscv64
include tools/mk/llvm.mk

# Instance-driven build selection.  The instance manifest is the single source
# of truth for arch/board/ABI, protocol stack, lanes, resource tier and role:
#
#   make NET_INSTANCE=xfce-x86_64 kernel-only
#
# `tools/a20 build <instance>` uses the same derivation.  Command-line
# assignments still win, because GNU make gives them precedence over eval.
ifneq ($(NET_INSTANCE),)
NET_INSTANCE_FILE := $(if $(filter /%,$(NET_INSTANCE)),$(NET_INSTANCE),$(CURDIR)/instances/$(NET_INSTANCE).toml)
ifeq ($(wildcard $(NET_INSTANCE_FILE)),)
$(error NET_INSTANCE=$(NET_INSTANCE): no manifest at $(NET_INSTANCE_FILE))
endif
NET_INSTANCE_VARS := $(shell $(PYTHON) tools/cli/a20_net_vars.py $(NET_INSTANCE_FILE))
ifeq ($(strip $(NET_INSTANCE_VARS)),)
$(error NET_INSTANCE=$(NET_INSTANCE): failed to derive build variables)
endif
$(foreach assignment,$(NET_INSTANCE_VARS),$(eval $(assignment)))
endif

RETIRED_ARCHES := loongarch32 ppc64le
ifneq ($(filter $(ARCH),$(RETIRED_ARCHES)),)
$(error ARCH=$(ARCH) is retired from active builds; for historical reproduction, check out v0.18 or earlier (see docs/releases/v1.0.md))
endif
# Optional architecture-owned build policy; no board facts in the engine.
-include kernel/arch/$(ARCH)/config.mk
ABI ?= both
BRINGUP ?= 0
RAMFS_USER ?= 0
RAMFS_FASTFETCH ?= 0
WIFI_RTW89 ?= 0
# Generic desktop diagnostics/session tools are independent of the radio.
RAMFS_DESKTOP_TOOLS ?= $(RAMFS_DISTRO)
ifeq ($(RAMFS_DESKTOP_TOOLS),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_DESKTOP_TOOLS=1 requires RAMFS_USER=1)
endif
endif
ifeq ($(WIFI_RTW89),1)
ifneq ($(ARCH):$(DRIVER_DEPLOYMENT):$(RAMFS_USER),x86_64:embedded:1)
$(error WIFI_RTW89=1 requires ARCH=x86_64 DRIVER_DEPLOYMENT=embedded RAMFS_USER=1)
endif
endif
ifeq ($(filter $(RAMFS_FASTFETCH),0 1),)
$(error RAMFS_FASTFETCH must be 0 or 1)
endif
ifeq ($(RAMFS_FASTFETCH),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_FASTFETCH=1 requires RAMFS_USER=1)
endif
ifeq ($(wildcard user/external/apps/fastfetch/src/fastfetch.c),)
$(error fastfetch source missing; run 'git submodule update --init user/external/apps/fastfetch')
endif
endif
RAMFS_NATIVE_TESTS ?= 0
RAMFS_RT_TESTS ?= 0
RAMFS_NET_TESTS ?= 0
RAMFS_FPU_TESTS ?= 0
ifeq ($(filter $(RAMFS_FPU_TESTS),0 1),)
$(error RAMFS_FPU_TESTS must be 0 or 1)
endif
ifeq ($(RAMFS_FPU_TESTS),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_FPU_TESTS=1 requires RAMFS_USER=1)
endif
endif
RAMFS_MEDIA ?= 0
RAMFS_DISTRO ?= 0
ifeq ($(filter $(RAMFS_DISTRO),0 1),)
$(error RAMFS_DISTRO must be 0 or 1)
endif
ifeq ($(RAMFS_DISTRO),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_DISTRO=1 requires RAMFS_USER=1)
endif
ifneq ($(filter 1,$(STORAGE_READ_ONLY) $(EXTERNAL_ROOT) $(RAMFS_MEDIA) $(NOMMU)),)
$(error RAMFS_DISTRO=1 conflicts with STORAGE_READ_ONLY EXTERNAL_ROOT RAMFS_MEDIA NOMMU)
endif
endif
ifeq ($(filter $(RAMFS_MEDIA),0 1),)
$(error RAMFS_MEDIA must be 0 or 1)
endif
ifeq ($(RAMFS_MEDIA),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_MEDIA=1 requires RAMFS_USER=1)
endif
endif
ifeq ($(filter $(RAMFS_NATIVE_TESTS),0 1),)
$(error RAMFS_NATIVE_TESTS must be 0 or 1)
endif
OPT ?= -O3
USER_OPT ?= $(OPT)
NR_CPUS ?= 1
# BUILD_VARIANT is expanded while QEMU_FLAGS is assembled below.  Its
# preemption default must exist before the QEMU image paths are captured, or
# `-kernel` and `-drive` can point at different build directories.
CONFIG_KERNEL_PREEMPT ?= 1
# Functional diagnostic; separate artifacts, never enabled in normal builds.
SMP_SELFTEST ?= 0
# Explicit freestanding BSD bootstrap diagnostic artifact; never a fallback backend.
BSD_BOOTSTRAP_OBJECT ?=
BSD_CLANG ?= $(if $(wildcard $(CURDIR)/build/toolchains/bsd-clang19/root/usr/lib/llvm-19/bin/clang),$(CURDIR)/build/toolchains/bsd-clang19/root/usr/lib/llvm-19/bin/clang,clang)
# Optional shared-RAM multi-process QEMU platform; ordinary builds unchanged.
QEMU_SHARED_SMP ?= 0
ifneq ($(filter $(QEMU_SHARED_SMP),0 1),$(QEMU_SHARED_SMP))
$(error QEMU_SHARED_SMP must be 0 or 1)
endif
ifeq ($(QEMU_SHARED_SMP),1)
ifneq ($(ARCH),x86_64)
$(error QEMU_SHARED_SMP currently requires ARCH=x86_64)
endif
endif
# Network lanes. 1 keeps every lane index folded to 0, which is the
# embedded shape and byte-for-byte the pre-lane behaviour. Raise it
# only together with stage D of docs/net/net-lanes.md; stages A-C
# compute and report lanes but do not yet route packets by them.
NET_LANES ?= 1
# Compile-time capability: hosts do not transit packets. Router builds enable
# IPv4/IPv6 forwarding; this does not assert that the full router suite passes.
NET_ROLE ?= host
ifeq ($(filter $(NET_ROLE),host router),)
$(error NET_ROLE must be host or router)
endif
# Network resource tier, 1..3, consumed by kernel/include/net/profile.h as
# CONFIG_NET_PROFILE. 2 (DEFAULT) is the historical hosted configuration and
# stays the default so no existing target changes footprint. 1 (EMBEDDED) is
# the small-SRAM ceiling set; 3 (SERVER) is for high-connection-count hosts.
# An out-of-range value is rejected below rather than silently falling into
# the header's #else branch, which is DEFAULT: a typo must not quietly hand a
# memory-constrained build the server-sized pools.
NET_PROFILE ?= 2
ifeq ($(filter 1 2 3,$(NET_PROFILE)),)
$(error NET_PROFILE=$(NET_PROFILE) is not a valid tier; use 1 (embedded), 2 (default) or 3 (server))
endif
EXTERNAL_ROOT ?= 0
COOPERATIVE_BOOT ?= 0
STORAGE_READ_ONLY ?= 0
ALLOW_UNVERIFIED_SMP ?= 0
# Arch matrices come from components/trim.toml via components/trim.mk.
SMP_VERIFIED_QEMU_ARCHES := $(TRIM_CAP_SMP_VERIFIED_QEMU_ARCHES)
PROFILE ?= full
# Swap is on by default; the NOMMU override below forces it back off where
# there is no MMU to demand-page from.  A toggle is rebuild-safe without a
# BUILD_VARIANT component because BUILD_FLAGS_STAMP in tools/mk/targets-images.mk
# keys on $(CFLAGS), which gains -DCONFIG_SWAP when this flips.
CONFIG_SWAP ?= y

# Foreign-architecture execve forwarding (a20.xlator=*).  On by default because
# the channel is off at runtime unless an administrator both passes a20.xlator=1
# and names a translator path, so a compiled-in-but-unconfigured kernel behaves
# exactly like one without the feature.  CONFIG_XLATOR=0 is the embedded /
# low-resource story: kernel/proc/xlator.c is not compiled, task_t loses the
# xlator_host field, exec.c loses the re-exec branch and mm/wx.c loses the W^X
# exemption.  Same rebuild-safety argument as CONFIG_SWAP above: the stamp in
# tools/mk/targets-images.mk keys on $(CFLAGS), which gains -DCONFIG_XLATOR when
# this flips, so a stale object can never be linked against the wrong knob.
#
# CONFIG_XLATOR additionally gets a BUILD_VARIANT component when off, because
# smoke-exec-xlator and smoke-exec-xlator-off boot the same ARCH/ABI/BOARD and
# would otherwise share one output directory.  The stamp forces a full rebuild
# when the flag flips, so sharing that directory means the two cases cannot run
# concurrently under `make -j check`; a distinct directory costs one more
# build and removes the race.
CONFIG_XLATOR ?= y

# Bring-up only: prompt for the kernel command line on the console when
# firmware supplies none.  Off by default, because it moves authority over
# kernel configuration -- including the a20.xlator.<guest> path -- from
# whoever built the image to whoever holds the serial port at boot.  Set
# UART_CMDLINE=1 on the builds where that is the intent (QEMU loongarch virt
# has no other way to receive a command line at all).  It cannot be enabled
# from the command line: reading the command line is what this is for.
UART_CMDLINE ?= n

# A translator JITs guest code, which needs a 64-bit host with real page
# protections (mprotect RWX) and an MMU to demand-page from.  Targets outside
# that set get the feature forced off rather than a kernel that fails to build
# or a JIT that cannot work: armv7m/arm32 and riscv32 are 32-bit or NOMMU.
# Mirrors the NOMMU and SWAP gates below -- force the feature off outside the
# supported set instead of letting the build break.  The arch list itself is
# capability "xlator" in components/trim.toml.
XLATOR_SUPPORTED_ARCHES := $(TRIM_CAP_XLATOR_ARCHES)

# Synthetic driver lifecycle test, the platform-bus IRQ resource self-check,
# and the HDA/NVMe in-probe smoke builds.
# All four default to off and are enabled with the on-value their existing
# callers already pass (tools/smoke_cases.py and docs/drivers/meta/
# testing-and-submission.md use `=y`), so the BUILD_VARIANT components below
# and the gate invocations keep working unchanged.
CONFIG_DRIVER_LIFECYCLE_TEST ?= 0
CONFIG_PLATFORM_IRQ_TEST ?= 0
CONFIG_PCI_PIRQ_TEST ?= 0
CONFIG_CHANNEL_DONATE_TEST ?= n
CONFIG_HDA_SMOKE_TEST ?= 0
CONFIG_NVME_SMOKE_TEST ?= 0

# Slab integrity checking.  At 0 the allocator trusts its free lists; at 1
# every kmalloc/kfree walks the page's free list and panics on a node that is
# out of bounds, misaligned, cyclic, or inconsistent with the page's
# accounting.  It costs O(free objects) per allocation, so it is a debugging
# build (its own BUILD_VARIANT component) rather than a default -- but it is
# the only detector that can see a slab page being scribbled on while it is
# still live, which is the shape of the multi-threaded corruption reports in
# docs/status/current/known-issues.md.
CONFIG_SLAB_DEBUG ?= 0

# Single knob for the CONFIG_HDA_SMOKE_TEST / CONFIG_NVME_SMOKE_TEST macros in
# the loadable driver packages.  Those macros are consumed *only* by
# kernel/drvmod/examples/{hda,nvme}.c, which tools/mk/driver-modules.mk compiles
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
# Dedicated raw USART2/3 cluster firmware; 0 preserves the peripheral image.
STM32_CLUSTER_PORT ?= 0
# Experimental native shared-RAM kernel; requires the documented fabric ABI.
MCU_SHARED_KERNEL ?= 0
MCU_SHARED_RAM_KB ?= 1024
MCU_SHARED_TEST ?= 0
MCU_NATIVE_PROGRAM ?= 0
# Opt-in: boot the CMSIS learner demo on the physical Hi3861 (U-mode).
HI3861_LITEOS_DEMO ?= 0
ifeq ($(filter $(HI3861_LITEOS_DEMO),0 1),)
$(error HI3861_LITEOS_DEMO must be 0 or 1)
endif
# The RV32 demo board always runs its learner experiments as flat U-mode
# programs; a command-line MCU_NATIVE_PROGRAM=0 still wins.
ifeq ($(ARCH)/$(BOARD),riscv32/qemu-virt-riscv32-mcu)
MCU_NATIVE_PROGRAM := 1
endif
ifeq ($(ARCH)/$(BOARD),riscv32/hi3861)
# Same flat-program foundation; the demo image stays opt-in until the U-mode
# path is confirmed on the physical part.
MCU_NATIVE_PROGRAM := 1
endif
NATIVE_FLAT_FILES ?= 0
ifeq ($(filter $(NATIVE_FLAT_FILES),0 1),)
$(error NATIVE_FLAT_FILES must be 0 or 1)
endif
ifeq ($(NATIVE_FLAT_FILES),1)
ifneq ($(MCU_NATIVE_PROGRAM),1)
$(error NATIVE_FLAT_FILES currently requires the native flat program profile)
endif
endif
ifeq ($(filter $(MCU_NATIVE_PROGRAM),0 1),)
$(error MCU_NATIVE_PROGRAM must be 0 or 1)
endif
ifeq ($(MCU_NATIVE_PROGRAM),1)
ifeq ($(filter $(ABI),native both),)
$(error MCU_NATIVE_PROGRAM requires ABI=native or ABI=both)
endif
ifeq ($(ARCH),armv7m)
ifneq ($(MCU_SHARED_KERNEL),1)
$(error MCU_NATIVE_PROGRAM on armv7m requires MCU_SHARED_KERNEL=1)
endif
endif
endif
ifeq ($(filter $(MCU_SHARED_KERNEL),0 1),)
$(error MCU_SHARED_KERNEL must be 0 or 1)
endif
ifeq ($(filter $(MCU_SHARED_TEST),0 1),)
$(error MCU_SHARED_TEST must be 0 or 1)
endif
ifeq ($(MCU_SHARED_KERNEL),1)
ifneq ($(ARCH),armv7m)
$(error MCU_SHARED_KERNEL currently requires ARCH=armv7m)
endif
ifeq ($(filter $(NR_CPUS),2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
$(error MCU_SHARED_KERNEL requires NR_CPUS=2..16)
endif
ifneq ($(STM32_CLUSTER_PORT),0)
$(error MCU_SHARED_KERNEL requires STM32_CLUSTER_PORT=0)
endif
ifeq ($(STM32_QEMU),1)
$(error MCU_SHARED_KERNEL requires the shared-memory fabric, absent in stm32vldiscovery)
endif
ifeq ($(STM32_XUANWU),1)
$(error MCU_SHARED_KERNEL uses a custom fabric board, not the Xuanwu SRAM wiring)
endif
ifeq ($(filter $(MCU_SHARED_RAM_KB),64 128 256 512 1024),)
$(error MCU_SHARED_RAM_KB must be 64, 128, 256, 512 or 1024 for the current FSMC address-pin map)
endif
else
ifneq ($(MCU_SHARED_TEST),0)
$(error MCU_SHARED_TEST requires MCU_SHARED_KERNEL=1)
endif
endif
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
STM32_FLASH_KB ?= 64
STM32_RAM_KB ?= 20
STM32_XUANWU ?= 0
else
BOARD ?= qemu-virt-$(ARCH)
endif
ifeq ($(MCU_SHARED_KERNEL),1)
ifneq ($(BOARD),stm32f103)
$(error MCU_SHARED_KERNEL currently requires BOARD=stm32f103 with custom fabric wiring)
endif
endif

# MCU-class builds (components/trim.toml [profile.mcu]) replace the whole
# kernel source list and imply a NOMMU, kernel-only bring-up.  An ARCH listed
# in a profile's arches gets that profile forced on, so adding a second
# MCU-class architecture is one TOML edit.
MCU_PROFILE_SELECTED := $(strip $(filter $(ARCH),$(TRIM_PROFILE_MCU_ARCHES)) $(filter $(ARCH)/$(BOARD),$(TRIM_PROFILE_MCU_BOARDS)))
ifneq ($(MCU_PROFILE_SELECTED),)
override PROFILE := mcu
ifneq (,$(TRIM_PROFILE_MCU_FORCE_NOMMU))
override NOMMU := 1
endif
ifneq (,$(TRIM_PROFILE_MCU_FORCE_BRINGUP))
override BRINGUP := 1
endif
endif

# Stack selection is separate from resource budgets and QEMU NET_BACKEND.
# Hosted 64-bit usage includes ordinary MMU VMs and PC desktops/servers. Static
# driver deployment is independent of this distinction. Physical embedded
# boards, 32-bit targets and NOMMU keep lwIP; MCU firmware has no stack.
NET_HOSTED_BOARDS := $(addprefix qemu-virt-,riscv64 loongarch64 aarch64 x86_64) x86_64-pc virtualbox-aarch64
NET_STACK ?= $(if $(MCU_PROFILE_SELECTED),none,$(if $(filter 1,$(NOMMU)),lwip,$(if $(filter $(BOARD),$(NET_HOSTED_BOARDS)),bsd,lwip)))
# Router flow storage is independent of the BSD socket and mbuf profile.
NET_CONNTRACK_ENTRIES ?= $(if $(and $(filter bsd,$(NET_STACK)),$(filter router,$(NET_ROLE))),4096)
ifneq ($(strip $(NET_CONNTRACK_ENTRIES)),)
ifneq ($(NET_STACK),bsd)
$(error NET_CONNTRACK_ENTRIES requires NET_STACK=bsd)
endif
ifneq ($(words $(NET_CONNTRACK_ENTRIES)),1)
$(error NET_CONNTRACK_ENTRIES must be one capacity)
endif
ifeq ($(filter 256 512 1024 2048 4096 8192 16384,$(NET_CONNTRACK_ENTRIES)),)
$(error NET_CONNTRACK_ENTRIES must be a power of two from 256 to 16384)
endif
endif
# Instances set NET_STACK through [net].stack (tools/a20 or NET_INSTANCE=).
# The default above is only for a bare `make` with no manifest.
.PHONY: check-net-backend-host
check-net-backend-host:
	sh tools/tests/net_backend_host.sh
LWIP_SOURCE_ROOT = $(if $(filter native,$(NET_STACK)),$(KERNEL_DIR)/net/backends/native/protocol,$(if $(filter lwip,$(NET_STACK)),$(KERNEL_DIR)/external/lwip-upstream,))
ifeq ($(NET_ROLE),router)
ifeq ($(NET_STACK),none)
$(error NET_ROLE=router requires a TCP/IP backend)
endif
endif
ifeq ($(NET_STACK),bsd)
ifeq ($(filter $(ARCH),x86_64 aarch64 riscv64 loongarch64),)
$(error NET_STACK=bsd requires ARCH=x86_64, aarch64, riscv64 or loongarch64; 32-bit targets use lwip)
endif
ifneq ($(NET_LANES),1)
$(error BSD uses one serialized protocol executor; set NET_LANES=1)
endif
ifneq ($(NET_PROFILE),2)
$(error NET_STACK=bsd currently validates only NET_PROFILE=2; embedded/server tiers need separate budget evidence)
endif
endif
ifeq ($(NET_STACK),lwip)
ifneq ($(NET_LANES),1)
$(error pristine lwIP requires NET_LANES=1; its core is serialized independently of NR_CPUS)
endif
endif
ifeq ($(filter $(NET_STACK),lwip native bsd none),)
$(error NET_STACK must be lwip, native, bsd or none)
endif
ifeq ($(NET_STACK),none)
ifeq ($(PROFILE),mcu)
else
$(error NET_STACK=none currently requires the MCU profile)
endif
else
ifeq ($(PROFILE),mcu)
$(error the MCU profile does not have enough runtime services for TCP/IP)
endif
endif

# Driver deployment depends on the selected architecture and board.
include tools/mk/driver-deployment.mk

# One protocol, profile-selected resource budgets. MCU firmware keeps the
# single-transaction budget; hosted kernels may select any bounded tier.
CLUSTER_PROFILE ?= $(if $(filter mcu,$(PROFILE)),1,2)
CLUSTER_UART_COM2 ?= 0
ifeq ($(CLUSTER_UART_COM2),1)
ifneq ($(ARCH),x86_64)
$(error CLUSTER_UART_COM2 requires ARCH=x86_64)
endif
ifeq ($(CLUSTER_PROFILE),1)
$(error CLUSTER_UART_COM2 requires DEFAULT or SERVER cluster profile)
endif
endif
ifeq ($(filter 1 2 3,$(CLUSTER_PROFILE)),)
$(error CLUSTER_PROFILE=$(CLUSTER_PROFILE) is invalid; use 1, 2 or 3)
endif
ifeq ($(PROFILE),mcu)
ifneq ($(CLUSTER_PROFILE),1)
$(error MCU firmware requires CLUSTER_PROFILE=1)
endif
endif

NOMMU ?= 0
NOMMU_SUPPORTED_ARCHES := $(TRIM_CAP_NOMMU_ARCHES)

# Swap needs a PTE encoding for the 64-bit swap entry (pte_is_swap /
# pte_to_swp_entry / swp_entry_to_pte in kernel/arch/<arch>/include/page_table.h).
# riscv32 and loongarch32 have not defined one, so CONFIG_SWAP=y does not merely
# leave them featureless there -- kernel/mm/{fault,mm,munmap}.c and
# kernel/proc/exit.c fail to compile.  Mirrors the NOMMU gate above: force the
# feature off outside the supported set instead of letting the build break.
SWAP_SUPPORTED_ARCHES := $(TRIM_CAP_SWAP_ARCHES)
ifeq ($(NOMMU),1)
CONFIG_SWAP := n
else
ifeq ($(filter $(CONFIG_SWAP),y),y)
ifeq ($(filter $(ARCH),$(SWAP_SUPPORTED_ARCHES)),)
CONFIG_SWAP := n
endif
endif
endif

# Foreign-architecture translation channel (see CONFIG_XLATOR at the top and
# docs/kernel/exec-xlator/).  Three ways it gets forced off:
#   NOMMU=1        no MMU to demand-page guest memory from;
#   PROFILE=mcu    that build replaces the whole source list anyway;
#   unsupported    32-bit hosts cannot host a 64-bit guest JIT at all.
# Same shape as the SWAP gate above: a user-set CONFIG_XLATOR=n is never
# flipped back on.
ifeq ($(NOMMU),1)
CONFIG_XLATOR := n
else
ifeq ($(filter $(PROFILE),mcu),mcu)
CONFIG_XLATOR := n
else
ifeq ($(filter $(CONFIG_XLATOR),y),y)
ifeq ($(filter $(ARCH),$(XLATOR_SUPPORTED_ARCHES)),)
CONFIG_XLATOR := n
endif
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
$(error NR_CPUS=$(NR_CPUS) is unverified for ARCH=$(ARCH) BOARD=$(BOARD); set ALLOW_UNVERIFIED_SMP=1 only for explicit SMP bringup experiments)
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
# `make help`
# ================================================================
# There are well over a thousand targets here, almost all of them generated per
# instance/arch/smoke combination.  Printing them is how a newcomer gets a wall
# of noise instead of an answer, so `make help` lists the handful of entry
# points that actually get typed, grouped by what you are trying to do.  The
# full set stays discoverable with `make -qp | grep '::'`, a deliberate opt-in.
#
# Placed next to .DEFAULT_GOAL, right where a newcomer's eye goes first, and
# every value it interpolates is either defined above (ARCH, BOARD, ABI,
# BRINGUP, NR_CPUS) or is a recursively-expanded variable defined by the
# includes further down (CHECK_FAST_GATES).  Help text that quotes a hand-typed
# copy of the arch list or the gate tier goes stale silently; this cannot.
#
# The fast-tier gate list is broken into fixed-size chunks so it stays readable
# in a terminal, and each chunk is guarded by $(if $(wordlist ...)) so an absent
# chunk contributes no argument (and therefore no blank line) to printf.  The
# four chunks below cover 16 gate names; the tier has 15, so a gate added past
# that needs a fifth chunk here rather than silently going unlisted.
help-gate-chunk = $(if $(wordlist $(1),$(2),$(CHECK_FAST_GATES)),'     $(wordlist $(1),$(2),$(CHECK_FAST_GATES))')

.PHONY: help
help:
	@printf '%s\n' \
	  'A20OS -- hybrid kernel.  Usage: make <target> [VAR=VALUE ...]' \
	  '' \
	  "Now building: ARCH=$(ARCH) BOARD=$(BOARD) ABI=$(ABI) BRINGUP=$(BRINGUP) NR_CPUS=$(NR_CPUS)" \
	  '' \
	  '  Day-to-day development' \
	  '    make run                       build ARCH=$$ARCH and boot it in QEMU' \
	  '    make run-riscv64               ... same, for a named arch (also -loongarch64,' \
	  '                                   -aarch64/-arm64, -x86_64, -arm32, -riscv32)' \
	  '    make debug-riscv64             boot paused with a GDB server on :1234' \
	  '    make dev-build                 kernel + userspace + test images, no QEMU' \
	  '    make kernel-only               just the kernel, for ARCH/BOARD' \
	  '    make clean                     remove build output for the current variant' \
	  '' \
	  '  Tests -- fast and host-only (no QEMU, no cross toolchain)' \
	  '    make check                     the whole fast test tier' \
	  '' \
	  "  What 'make check' runs ($(words $(CHECK_FAST_GATES)) entries):" \
	  $(call help-gate-chunk,1,4) \
	  $(call help-gate-chunk,5,8) \
	  $(call help-gate-chunk,9,12) \
	  $(call help-gate-chunk,13,16) \
	  '' \
	  '    make check-manifests           instances/ components/ Makefile agree' \
	  '    make host-tests                host-gcc unit tests for kernel format/helper logic' \
	  '' \
	  '  Tests and builds -- need a cross toolchain and/or QEMU' \
	  '    make check-kernel-build        build-only bring-up gate for the current ARCH' \
	  '    make check-build-matrix        bring-up + userspace builds across the arch matrix' \
	  '    make check-cluster-send        reliable one-way SEND, loss and mixed routed profiles' \
	  '    make check-cluster-hardening   random loss/delay/CRC/flood/partition acceptance' \
	  '    make check-cluster-fuzz-smoke  five minutes of varied sanitizer mutations' \
	  '    make check-cluster-performance Native local/loopback/UDP RPC measurements' \
	  '    make demo-wordcount            sixteen shards over eight servers or a mixed MCU cluster' \
	  '    make smoke-riscv64             BRINGUP boot stages + self power-off (no syscalls)' \
	  '    make smoke-abi-linux           Linux ABI syscall smoke in QEMU' \
	  '' \
	  '  Release (what a bare `make` does)' \
	  '    make                           dual-arch release: kernel-rv kernel-la disk*.img' \
	  '    make image-world               package -> repository -> bootable image' \
	  '    make docs                      build standard-reference.pdf from docs/' \
	  '' \
	  '  Instance tooling (the declarative layer the targets above wrap)' \
	  '    tools/a20 list                 every predefined instance' \
	  '    tools/a20 run <instance>       build and boot an instance' \
	  '    tools/a20 debug <instance>     boot an instance with a GDB server' \
	  '    tools/a20 test <instance>      run an instance smoke' \
	  '    tools/a20 check                validate instances/, arch matrix, registries' \
	  '    docs/dev/instances.md              instance fields and the component registry' \
	  '' \
	  '  Boards and MCU' \
	  '    make BRINGUP=1 run-<arch>      kernel-only bring-up variant' \
	  '    make vf2-sdcard                VisionFive 2 SD card' \
	  '    make stm32f103-bringup         STM32F103 firmware (64 KiB flash)' \
	  '    tools/a20 package hi3861       Hi3861 kernel A image (hupg)' \
	  '' \
	  'Docs: docs/dev/build.md (build), docs/testing-gates.md (gates),' \
	  '      docs/dev/instances.md (instances), docs/CONTRIBUTING.md (style + gates).'

# ================================================================
# Build paths and userspace artifacts
# ================================================================

KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include

# The NAT algorithm slice can be selected independently while its C reference
# remains buildable. The Rust object has a separate output directory so a C
# image and a Rust image can never reuse one another's kernel artifacts.
# Which architectures a selector can default to `rust` on.
#
# tools/mk/rust-kernel.mk is included far below and maps each architecture to a
# Rust target, so the lists are spelled here too; a selector that is `rust` on an
# architecture with no target is a hard error in rust-kernel.mk, which is what
# keeps the two from drifting apart silently.  The MCU trim profile
# (components/trim.mk's TRIM_PROFILE_MCU_ARCHES) compiles an explicit source list
# with no kernel/net/*.c at all, so nothing that needs those files can be the
# default there.  Everywhere else the Rust implementation is the production path;
# `c` still selects the C reference.
NET_RUST_ARCHES := x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m
# netfilter_rewrite has no riscv32 Rust target (see RUST_NETFILTER_TARGET_*).
NETFILTER_RUST_ARCHES := x86_64 aarch64 loongarch64 riscv64 arm32 armv7m
NET_RUST_OK := $(if $(MCU_PROFILE_SELECTED),,$(filter-out $(TRIM_PROFILE_MCU_ARCHES),$(NET_RUST_ARCHES)))
NETFILTER_RUST_OK := $(if $(MCU_PROFILE_SELECTED),,$(filter-out $(TRIM_PROFILE_MCU_ARCHES),$(NETFILTER_RUST_ARCHES)))

NETFILTER_REWRITE_IMPL ?= $(if $(filter $(ARCH),$(NETFILTER_RUST_OK)),rust,c)
ifeq ($(filter $(NETFILTER_REWRITE_IMPL),c rust),)
$(error NETFILTER_REWRITE_IMPL must be c or rust)
endif
TASK_OBJECTS_IMPL ?= c
ifeq ($(filter $(TASK_OBJECTS_IMPL),c rust),)
$(error TASK_OBJECTS_IMPL must be c or rust)
endif
CHANNEL_SEND_IMPL ?= rust
ifneq ($(CHANNEL_SEND_IMPL),rust)
$(error CHANNEL_SEND_IMPL is fixed to rust; the C transaction fallback has been retired)
endif
CHANNEL_RECV_TXN_IMPL ?= rust
ifneq ($(CHANNEL_RECV_TXN_IMPL),rust)
$(error CHANNEL_RECV_TXN_IMPL is fixed to rust; the C transaction fallback has been retired)
endif
HANDLE_SLOT_TRANSITIONS_IMPL ?= c
ifeq ($(filter $(HANDLE_SLOT_TRANSITIONS_IMPL),c rust),)
$(error HANDLE_SLOT_TRANSITIONS_IMPL must be c or rust)
endif
PROCESS_VM_IMPL ?= c
ifeq ($(filter $(PROCESS_VM_IMPL),c rust),)
$(error PROCESS_VM_IMPL must be c or rust)
endif
ifeq ($(PROCESS_VM_IMPL),rust)
ifeq ($(filter $(ABI),linux both),)
$(error PROCESS_VM_IMPL=rust requires ABI=linux or ABI=both)
endif
endif
PARK_WAIT_IMPL ?= c
ifeq ($(filter $(PARK_WAIT_IMPL),c rust),)
$(error PARK_WAIT_IMPL must be c or rust)
endif
ifeq ($(ARCH),riscv32)
ifeq ($(PARK_WAIT_IMPL),rust)
ifneq ($(NR_CPUS),1)
$(error PARK_WAIT_IMPL=rust on ARCH=riscv32 currently requires NR_CPUS=1; its 64-bit adapter masks local IRQs but is not cross-hart atomic)
endif
endif
endif
WAIT_QUEUE_IMPL ?= c
ifeq ($(filter $(WAIT_QUEUE_IMPL),c rust),)
$(error WAIT_QUEUE_IMPL must be c or rust)
endif
RW_MUTEX_IMPL ?= c
ifeq ($(filter $(RW_MUTEX_IMPL),c rust),)
$(error RW_MUTEX_IMPL must be c or rust)
endif
MUTEX_STATE_IMPL ?= c
ifeq ($(filter $(MUTEX_STATE_IMPL),c rust),)
$(error MUTEX_STATE_IMPL must be c or rust)
endif
COMPLETION_IMPL ?= c
ifeq ($(filter $(COMPLETION_IMPL),c rust),)
$(error COMPLETION_IMPL must be c or rust)
endif
MM_SHUTDOWN_WRITE_IMPL ?= c
ifeq ($(filter $(MM_SHUTDOWN_WRITE_IMPL),c rust),)
$(error MM_SHUTDOWN_WRITE_IMPL must be c or rust)
endif
TASK_LIFECYCLE_IMPL ?= c
ifeq ($(filter $(TASK_LIFECYCLE_IMPL),c rust),)
$(error TASK_LIFECYCLE_IMPL must be c or rust)
endif
TASK_EXIT_IMPL ?= c
ifeq ($(filter $(TASK_EXIT_IMPL),c rust),)
$(error TASK_EXIT_IMPL must be c or rust)
endif
WAIT4_IMPL ?= c
ifeq ($(filter $(WAIT4_IMPL),c rust),)
$(error WAIT4_IMPL must be c or rust)
endif
TASK_PLACEMENT_IMPL ?= c
ifeq ($(filter $(TASK_PLACEMENT_IMPL),c rust),)
$(error TASK_PLACEMENT_IMPL must be c or rust)
endif
EEVDF_MATH_IMPL ?= c
ifeq ($(filter $(EEVDF_MATH_IMPL),c rust),)
$(error EEVDF_MATH_IMPL must be c or rust)
endif
RT_SCHED_IMPL ?= c
ifeq ($(filter $(RT_SCHED_IMPL),c rust),)
$(error RT_SCHED_IMPL must be c or rust)
endif
TASK_CONSTRUCTION_IMPL ?= c
ifeq ($(filter $(TASK_CONSTRUCTION_IMPL),c rust),)
$(error TASK_CONSTRUCTION_IMPL must be c or rust)
endif
TASK_CONSTRUCTION_TEST ?= 0
ifeq ($(filter $(TASK_CONSTRUCTION_TEST),0 1),)
$(error TASK_CONSTRUCTION_TEST must be 0 or 1)
endif
ELF_EXEC_TEST ?= 0
ifeq ($(filter $(ELF_EXEC_TEST),0 1),)
$(error ELF_EXEC_TEST must be 0 or 1)
endif
# Test-profile only: makes one designated user address report a repaired page
# fault without changing the mapping, so the trap-storm detector's test can
# inject the "repaired yet re-trapping" shape deterministically.
TRAP_STORM_TEST ?= 0
ifeq ($(filter $(TRAP_STORM_TEST),0 1),)
$(error TRAP_STORM_TEST must be 0 or 1)
endif
# Test-profile only: lets the futex COW-window probe skip one COW resolution so
# the locked re-validation is forced to reject and retry.
FUTEX_COW_WINDOW_TEST ?= 0
ifeq ($(filter $(FUTEX_COW_WINDOW_TEST),0 1),)
$(error FUTEX_COW_WINDOW_TEST must be 0 or 1)
endif
# Test-profile only: builds the guest syscall_smoke so it arms the private
# one-shot clone failure and checks the fork rollback path.
SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST ?= 0
ifeq ($(filter $(SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST),0 1),)
$(error SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST must be 0 or 1)
endif
PID_IMPL ?= c
ifeq ($(filter $(PID_IMPL),c rust),)
$(error PID_IMPL must be c or rust)
endif
ifeq ($(PID_IMPL),rust)
ifeq ($(PROFILE),mcu)
$(error PID_IMPL=rust needs the non-MCU PID id space; kernel/proc/pid_alloc.rs is sized for PID_MAX_LIMIT=4194304)
endif
endif
VMA_IMPL ?= c
ifeq ($(filter $(VMA_IMPL),c rust),)
$(error VMA_IMPL must be c or rust)
endif
VMO_PAGE_IMPL ?= c
ifeq ($(filter $(VMO_PAGE_IMPL),c rust),)
$(error VMO_PAGE_IMPL must be c or rust)
endif
VMO_LIFECYCLE_IMPL ?= c
ifeq ($(filter $(VMO_LIFECYCLE_IMPL),c rust),)
$(error VMO_LIFECYCLE_IMPL must be c or rust)
endif
VMO_PAGER_IMPL ?= rust
ifneq ($(VMO_PAGER_IMPL),rust)
$(error VMO_PAGER_IMPL is fixed to rust; the C transaction fallback has been retired)
endif
DRIVER_REGISTRY_IMPL ?= c
ifeq ($(filter $(DRIVER_REGISTRY_IMPL),c rust),)
$(error DRIVER_REGISTRY_IMPL must be c or rust)
endif
VIRTIO_BLK_CORE_IMPL ?= c
ifeq ($(filter $(VIRTIO_BLK_CORE_IMPL),c rust),)
$(error VIRTIO_BLK_CORE_IMPL must be c or rust)
endif
ENVELOPE_POLICY_IMPL ?= rust
ifneq ($(ENVELOPE_POLICY_IMPL),rust)
$(error ENVELOPE_POLICY_IMPL is fixed to rust; the C transaction fallback has been retired)
endif
ENVELOPE_ACQUIRE_RACE_TEST ?= 0
ifeq ($(filter $(ENVELOPE_ACQUIRE_RACE_TEST),0 1),)
$(error ENVELOPE_ACQUIRE_RACE_TEST must be 0 or 1)
endif
ifeq ($(ENVELOPE_ACQUIRE_RACE_TEST),1)
ifeq ($(filter $(ABI),linux both),)
$(error ENVELOPE_ACQUIRE_RACE_TEST requires ABI=linux or ABI=both)
endif
endif
FDTABLE_STATE_IMPL ?= c
ifeq ($(filter $(FDTABLE_STATE_IMPL),c rust),)
$(error FDTABLE_STATE_IMPL must be c or rust)
endif
EVENTPOLL_STATE_IMPL ?= c
ifeq ($(filter $(EVENTPOLL_STATE_IMPL),c rust),)
$(error EVENTPOLL_STATE_IMPL must be c or rust)
endif
EVENT_DESCRIPTOR_STATE_IMPL ?= rust
ifneq ($(EVENT_DESCRIPTOR_STATE_IMPL),rust)
$(error EVENT_DESCRIPTOR_STATE_IMPL is fixed to rust; the C transaction fallback has been retired)
endif
SYSV_SEM_IMPL ?= rust
ifneq ($(SYSV_SEM_IMPL),rust)
$(error SYSV_SEM_IMPL is fixed to rust)
endif
POSIX_MQ_IMPL ?= rust
ifneq ($(POSIX_MQ_IMPL),rust)
$(error POSIX_MQ_IMPL is fixed to rust)
endif
SYSV_MSG_IMPL ?= rust
ifneq ($(SYSV_MSG_IMPL),rust)
$(error SYSV_MSG_IMPL is fixed to rust)
endif
SYSV_SHM_IMPL ?= rust
ifneq ($(SYSV_SHM_IMPL),rust)
$(error SYSV_SHM_IMPL is fixed to rust)
endif
USERFAULTFD_STATE_IMPL ?= rust
ifneq ($(USERFAULTFD_STATE_IMPL),rust)
$(error USERFAULTFD_STATE_IMPL is fixed to rust)
endif
FUTEX_WAIT_IMPL ?= rust
ifneq ($(FUTEX_WAIT_IMPL),rust)
$(error FUTEX_WAIT_IMPL is fixed to rust)
endif
LINUX_SCM_RIGHTS_IMPL ?= $(if $(filter $(ABI),linux both),$(if $(filter $(ARCH),$(NET_RUST_OK)),rust,c),c)
ifeq ($(filter $(LINUX_SCM_RIGHTS_IMPL),c rust),)
$(error LINUX_SCM_RIGHTS_IMPL must be c or rust)
endif
LINUX_MMSG_TXN_IMPL ?= $(if $(filter $(ABI),linux both),$(if $(filter $(ARCH),$(NET_RUST_OK)),rust,c),c)
ifeq ($(filter $(LINUX_MMSG_TXN_IMPL),c rust),)
$(error LINUX_MMSG_TXN_IMPL must be c or rust)
endif
NET_SOCKET_QUEUE_IMPL ?= $(if $(filter $(ARCH),$(NET_RUST_OK)),rust,c)
ifeq ($(filter $(NET_SOCKET_QUEUE_IMPL),c rust),)
$(error NET_SOCKET_QUEUE_IMPL must be c or rust)
endif
# Full systems must be able to truncate/reap depth-1 trees produced by normal
# desktop writes. The checked C fallback only accepts inline extent roots.
EXT4_EXTENT_TXN_IMPL ?= $(if $(filter full,$(PROFILE)),rust,c)
ifeq ($(filter $(EXT4_EXTENT_TXN_IMPL),c rust),)
$(error EXT4_EXTENT_TXN_IMPL must be c or rust)
endif
KEYRING_IMPL ?= c
ifeq ($(filter $(KEYRING_IMPL),c rust),)
$(error KEYRING_IMPL must be c or rust)
endif
VFS_LIFECYCLE_IMPL ?= rust
ifneq ($(VFS_LIFECYCLE_IMPL),rust)
$(error VFS_LIFECYCLE_IMPL is fixed to rust for the selected VFS lifecycle implementation)
endif
# The signal core (kernel/proc/signal_core.rs) is always the Rust
# implementation; the C files are mechanical bridges (locks, usercopy, wait
# queues, the hardware signal frame).
ifeq ($(SYSV_SHM_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error SYSV_SHM_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/sysv_shm.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_SYSV_SHM_OBJECT)
endif
endif
ifeq ($(USERFAULTFD_STATE_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error USERFAULTFD_STATE_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/userfaultfd.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_USERFAULTFD_STATE_OBJECT)
endif
endif
ifeq ($(FUTEX_WAIT_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error FUTEX_WAIT_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/futex.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_FUTEX_WAIT_OBJECT)
endif
endif

# Canonical (sb, ino) -> vnode identity index.  rust selects
# kernel/fs/vfs/vnode_identity.rs as the production path; c selects the
# header-only reference model (vnode_identity_c.h), which is retained solely as
# the host differential harness oracle and an explicit C-side regression image.
# The selector only matters where kernel/fs/uxfs/uxfs.c is in the profile; the
# MCU trim excludes it and stays Rust-free.
# UXFS RPC engine (admission, deadline/retry sequencing, stale-reply filtering,
# errno mapping).  rust is the production path; c keeps the reference loop in
# uxfs.c for the baseline image and the host harness.
# UXFS per-operation decision engine (request construction, response decoding,
# canonical-identity/page-cache/size transitions for the namespace and data
# plane).  rust is the production path; c keeps the inline op layer in uxfs.c
# for the baseline image and the host harness.
# Block cache structural state (hash/LRU/refs/dirty/holds/quarantine/eviction
# policy).  rust is the production path; c keeps the reference model in
# block_cache.c for the baseline image and the host differential harness.
# Page cache structural state (hash/LRU/mapping/dirty lists/free stack/
# refcount+pin accounting/detach/eviction/dirty-batch ordering).  rust is the
# production path; c keeps the reference model in page_cache.c for the baseline
# image and the host differential harness.
# VFS dentry cache state (hash chains, LRU, free list, match + second chance,
# eviction policy, bucket drop).
# procfs read/seek offset state and directory-cursor state (dirent layout,
# budget, cursor advance).  rust is the production path; c keeps the reference
# loops in procfs.c for the baseline image and the host differential harness.
# procfs node table, node classification and content plan (name table,
# parent-scoped node table, file type/mode, generator and capacity policy).
# VFS path/name state (join, split, normalise with and without a chroot floor,
# containment test, trailing-slash trim).  rust is the production path; c keeps
# the reference bodies in path.c for the baseline image and the host harness.
# VFS path-resolution walk (component iteration, symlink following with its
# RESOLVE_* decisions and loop limit, mount crossing, boundary checks, errno
# mapping).  rust is the production path; c keeps the reference walk in
# path_resolution.c for the baseline image and the host harness.
# VFS directory-mutation orchestration (unlink/rmdir/rename decision order,
# flag validation, sticky gate, ordered side effects).
# Mount-namespace lifecycle (in-flight admission counters, shutdown begin/abort,
# teardown-pass policy).
# Mount-table orchestration (umount path matching + busy rules, move_mount
# validation order).
# Open-file I/O-path policy (read/write admission, page-cache vs buffered vs
# direct data path).
ifeq ($(LINUX_SCM_RIGHTS_IMPL),rust)
ifeq ($(filter $(ABI),linux both),)
$(error LINUX_SCM_RIGHTS_IMPL=rust requires ABI=linux or ABI=both)
endif
endif
ifeq ($(LINUX_MMSG_TXN_IMPL),rust)
ifeq ($(filter $(ABI),linux both),)
$(error LINUX_MMSG_TXN_IMPL=rust requires ABI=linux or ABI=both)
endif
endif
ifeq ($(KEYRING_IMPL),rust)
ifeq ($(filter $(ABI),linux both),)
$(error KEYRING_IMPL=rust requires ABI=linux or ABI=both)
endif
endif
RUST_FS_STATE_ACTIVE = $(if $(filter $(KERNEL_DIR)/fs/%,$(KERNEL_SRC)),rust,)
RUST_SIGNAL_CORE_ACTIVE = $(if $(filter $(KERNEL_DIR)/proc/signal.c,$(KERNEL_SRC)),rust,)
RUST_RUNTIME_TEST ?= 0
ifeq ($(filter $(RUST_RUNTIME_TEST),0 1),)
$(error RUST_RUNTIME_TEST must be 0 or 1)
endif
ifeq ($(RUST_RUNTIME_TEST),1)
ifeq ($(filter rust,$(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(DONTUNMAP_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL) $(USER_COPY_IMPL) $(PROCESS_VM_IMPL) $(PARK_WAIT_IMPL) $(WAIT_QUEUE_IMPL) $(RW_MUTEX_IMPL) $(MUTEX_STATE_IMPL) $(COMPLETION_IMPL) $(PID_IMPL) $(VMA_IMPL) $(VMO_PAGE_IMPL) $(VMO_LIFECYCLE_IMPL) $(VMO_PAGER_IMPL) $(DRIVER_REGISTRY_IMPL) $(VIRTIO_BLK_CORE_IMPL) $(ENVELOPE_POLICY_IMPL) $(FDTABLE_STATE_IMPL) $(EVENTPOLL_STATE_IMPL) $(EVENT_DESCRIPTOR_STATE_IMPL) $(LINUX_SCM_RIGHTS_IMPL) $(LINUX_MMSG_TXN_IMPL) $(NET_SOCKET_QUEUE_IMPL) $(EXT4_EXTENT_TXN_IMPL) $(KEYRING_IMPL) $(MM_SHUTDOWN_WRITE_IMPL)),)
endif
ifeq ($(filter rust,$(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL) $(PROCESS_VM_IMPL) $(PARK_WAIT_IMPL) $(WAIT_TIMER_IMPL) $(WAIT_QUEUE_IMPL) $(RW_MUTEX_IMPL) $(MUTEX_STATE_IMPL) $(COMPLETION_IMPL) $(TASK_LIFECYCLE_IMPL) $(TASK_EXIT_IMPL) $(WAIT4_IMPL) $(TASK_PLACEMENT_IMPL) $(EEVDF_MATH_IMPL) $(RT_SCHED_IMPL) $(TASK_CONSTRUCTION_IMPL) $(VMO_PAGER_IMPL) $(DRIVER_REGISTRY_IMPL) $(VIRTIO_BLK_CORE_IMPL) $(ENVELOPE_POLICY_IMPL) $(FDTABLE_STATE_IMPL) $(EVENTPOLL_STATE_IMPL) $(EVENT_DESCRIPTOR_STATE_IMPL) $(LINUX_SCM_RIGHTS_IMPL) $(LINUX_MMSG_TXN_IMPL) $(NET_SOCKET_QUEUE_IMPL) $(EXT4_EXTENT_TXN_IMPL) $(KEYRING_IMPL)),)
$(error RUST_RUNTIME_TEST=1 requires at least one Rust kernel selector)
endif
endif
VMO_PAGER_TEST ?= 0
ifeq ($(filter $(VMO_PAGER_TEST),0 1),)
$(error VMO_PAGER_TEST must be 0 or 1)
endif
ifeq ($(VMO_PAGER_TEST),1)
ifneq ($(VMO_PAGER_IMPL),rust)
$(error VMO_PAGER_TEST=1 requires VMO_PAGER_IMPL=rust)
endif
ifeq ($(NOMMU),1)
$(error VMO_PAGER_TEST=1 requires an MMU profile)
endif
endif
VMO_PAGE_TEST ?= 0
ifeq ($(filter $(VMO_PAGE_TEST),0 1),)
$(error VMO_PAGE_TEST must be 0 or 1)
endif
ifeq ($(VMO_PAGE_TEST),1)
ifeq ($(NOMMU),1)
$(error VMO_PAGE_TEST=1 requires an MMU profile)
endif
endif
MNTNS_TEST ?= 0
ifeq ($(filter $(MNTNS_TEST),0 1),)
$(error MNTNS_TEST must be 0 or 1)
endif
RV32_SYNC_TEST ?= 0
ifeq ($(filter $(RV32_SYNC_TEST),0 1),)
$(error RV32_SYNC_TEST must be 0 or 1)
endif
ifeq ($(RV32_SYNC_TEST),1)
ifneq ($(ARCH),riscv32)
$(error RV32_SYNC_TEST=1 requires ARCH=riscv32)
endif
ifneq ($(NR_CPUS),1)
$(error RV32_SYNC_TEST=1 requires NR_CPUS=1)
endif
ifneq ($(BRINGUP),1)
$(error RV32_SYNC_TEST=1 requires BRINGUP=1)
endif
ifeq ($(PROFILE),mcu)
$(error RV32_SYNC_TEST=1 requires the RV32 kernel profile)
endif
endif
USER_COPY_TEST ?= 0
ifeq ($(filter $(USER_COPY_TEST),0 1),)
$(error USER_COPY_TEST must be 0 or 1)
endif
HANDLE_SLOT_TRANSITIONS_ACTIVE := $(if $(filter $(ABI),native both),1,)
ifeq ($(HANDLE_SLOT_TRANSITIONS_IMPL),rust)
ifeq ($(HANDLE_SLOT_TRANSITIONS_ACTIVE),)
$(error HANDLE_SLOT_TRANSITIONS_IMPL=rust requires ABI=native or ABI=both)
endif
endif
ifeq ($(DONTUNMAP_TXN_IMPL),rust)
ifeq ($(NOMMU),1)
$(error DONTUNMAP_TXN_IMPL=rust requires an MMU profile)
endif
endif
# CHANNEL_RECV_TXN_IMPL is fixed to rust for every profile, but the module only
# exists where the native ABI does: ABI_SRCS above pulls abi/native/ in for
# ABI=native|both only, and both the Rust rule and its KERNEL_OBJ edge are
# gated on kernel/abi/native/sys_native_ipc.c being in KERNEL_SRC.  Under
# ABI=linux the module is therefore absent rather than misconfigured, so there
# is deliberately no "requires ABI=native or ABI=both" guard here; one used to
# reject every ABI=linux instance (22 manifests, make smoke-abi-linux among
# them) for a module those profiles never build.
TASK_OBJECTS_ACTIVE := $(if $(filter $(ABI),native both),1,)
ifeq ($(ARCH),armv7m)
ifneq ($(MCU_NATIVE_PROGRAM),1)
TASK_OBJECTS_ACTIVE :=
endif
endif

# Preserve established generic and STM32 output paths used by smoke, release,
# flash, and QEMU runners. Options that change compiled code, including
# embedded deployment and cooperative boot, get distinct output directories.
BUILD_VARIANT = $(if $(filter llvm,$(TOOLCHAIN)),llvm-rt-,)$(ABI)-$(if $(filter 1,$(BRINGUP)),bringup,dev)$(if $(filter 1,$(HI3861_LITEOS_DEMO)),-hi-demo,)$(if $(filter 1,$(RAMFS_USER)),-ramfs-user,)$(if $(filter 1,$(RAMFS_FASTFETCH)),-fastfetch,)$(if $(filter 1,$(WIFI_RTW89)),-wifi,)$(if $(filter 1,$(RAMFS_DESKTOP_TOOLS)),-desktop-tools,)$(if $(filter 1,$(RAMFS_RT_TESTS)),-rt-tests,)$(if $(filter 1,$(RAMFS_MEDIA)),-media,)$(if $(filter 1,$(RAMFS_DISTRO)),-distro,)$(if $(filter 1,$(RAMFS_FPU_TESTS)),-fpu-test,)$(if $(and $(filter embedded,$(DRIVER_DEPLOYMENT)),$(filter-out armv7m,$(ARCH))),-embedded,)$(if $(filter 1,$(COOPERATIVE_BOOT)),-cooperative,)$(if $(filter 1,$(STORAGE_READ_ONLY)),-storage-ro,)$(if $(filter 1,$(EXTERNAL_ROOT)),-external-root,)$(if $(filter 1,$(NOMMU)),-nommu,)$(if $(filter-out 1,$(NR_CPUS)),-smp$(NR_CPUS),)$(if $(filter-out 1,$(NET_LANES)),-lanes$(NET_LANES),)$(if $(filter-out 2,$(NET_PROFILE)),-netp$(NET_PROFILE),)$(if $(and $(filter-out mcu,$(PROFILE)),$(filter-out 2,$(CLUSTER_PROFILE))),-clxp$(CLUSTER_PROFILE),)$(if $(filter 1,$(CONFIG_KERNEL_PREEMPT)),-preempt,)$(if $(filter 1,$(SMP_SELFTEST)),-smp-test,)$(if $(filter y,$(CONFIG_DRIVER_LIFECYCLE_TEST)),-driver-lifecycle,)$(if $(filter y,$(CONFIG_PLATFORM_IRQ_TEST)),-platform-irq,)$(if $(filter y,$(CONFIG_PCI_PIRQ_TEST)),-pirq-test,)$(if $(filter y,$(CONFIG_CHANNEL_DONATE_TEST)),-channel-donate-test,)$(if $(filter y,$(CONFIG_HDA_SMOKE_TEST)),-hda-smoke,)$(if $(filter y,$(CONFIG_NVME_SMOKE_TEST)),-nvme-smoke,)$(if $(filter 1,$(CONFIG_SLAB_DEBUG)),-slabdbg,)$(if $(filter-out y,$(CONFIG_XLATOR)),-noxlator)

ifeq ($(NETFILTER_REWRITE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-nfrust
endif
ifeq ($(TASK_OBJECTS_IMPL),rust)
ifeq ($(TASK_OBJECTS_ACTIVE),)
$(error TASK_OBJECTS_IMPL=rust requires an active Native task-object profile (ABI=native/both; MCU also requires MCU_NATIVE_PROGRAM=1))
endif
BUILD_VARIANT := $(BUILD_VARIANT)-taskrust
endif
ifeq ($(CHANNEL_SEND_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/ipc/a20_channel.c,$(KERNEL_SRC)),)
BUILD_VARIANT := $(BUILD_VARIANT)-channelsendrust
endif
endif
ifeq ($(CHANNEL_RECV_TXN_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/abi/native/sys_native_ipc.c,$(KERNEL_SRC)),)
BUILD_VARIANT := $(BUILD_VARIANT)-recvtxnrust
endif
endif
ifeq ($(DONTUNMAP_TXN_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-mremaprust
endif
ifeq ($(HANDLE_SLOT_TRANSITIONS_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-handleslotrust
endif
ifeq ($(PROCESS_VM_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-processvmrust
endif
ifeq ($(PARK_WAIT_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-parkwaitrust
endif
ifeq ($(WAIT_QUEUE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-waitqueuerust
endif
ifeq ($(RW_MUTEX_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-rwmutexrust
endif
ifeq ($(MUTEX_STATE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-mutexrust
endif
ifeq ($(COMPLETION_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-completionrust
endif
ifeq ($(MM_SHUTDOWN_WRITE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-mm-shutdown-write-rust
endif
ifeq ($(TASK_LIFECYCLE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-tasklifecycle-rust
endif
ifeq ($(TASK_EXIT_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-taskexit-rust
endif
ifeq ($(WAIT4_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-wait4-rust
endif
ifeq ($(TASK_PLACEMENT_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-taskplacement-rust
endif
ifeq ($(EEVDF_MATH_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-eevdfmath-rust
endif
ifeq ($(RT_SCHED_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-rt-sched-rust
endif
ifeq ($(TASK_CONSTRUCTION_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-taskconstruction-rust
endif
ifeq ($(TASK_CONSTRUCTION_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-taskconstruction-test
endif
ifeq ($(ELF_EXEC_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-elfexectest
endif
ifeq ($(TRAP_STORM_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-trapstorm-test
endif
ifeq ($(FUTEX_COW_WINDOW_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-futexcow-test
endif
ifeq ($(PID_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-pid-rust
endif
ifeq ($(VMA_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-vmarust
endif
ifeq ($(VMO_PAGE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-vmopagerust
endif
ifeq ($(VMO_LIFECYCLE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-vmolifecycle-rust
endif
ifeq ($(VMO_PAGER_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-vmopager-rust
endif
ifeq ($(DRIVER_REGISTRY_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-driverregistry-rust
endif
ifeq ($(VIRTIO_BLK_CORE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-virtio-blk-core-rust
endif
ifeq ($(ENVELOPE_POLICY_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-envelope-policy-rust
endif
BUILD_VARIANT := $(BUILD_VARIANT)-fdtable-rust
ifeq ($(EVENTPOLL_STATE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-eventpoll-rust
endif
ifeq ($(EVENT_DESCRIPTOR_STATE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-event-desc-rust
endif
ifeq ($(SYSV_SEM_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-sysv-sem-rust
endif
ifeq ($(POSIX_MQ_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-posix-mq-rust
endif
ifeq ($(SYSV_MSG_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-sysv-msg-rust
endif
ifeq ($(SYSV_SHM_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-sysv-shm-rust
endif
ifeq ($(ENVELOPE_ACQUIRE_RACE_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-envelope-acquire-race-test
endif
ifeq ($(LINUX_SCM_RIGHTS_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-scm-rights-rust
endif
ifeq ($(LINUX_MMSG_TXN_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-linux-mmsg-rust
endif
ifeq ($(NET_SOCKET_QUEUE_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-net-socket-queue-rust
endif
# Keep the C reference model only for the explicit regression/oracle selector;
# the Rust index is the default production path.
BUILD_VARIANT := $(BUILD_VARIANT)-uxfs-vnident-rust
BUILD_VARIANT := $(BUILD_VARIANT)-uxfs-rpc-rust
BUILD_VARIANT := $(BUILD_VARIANT)-uxfs-ops-rust
BUILD_VARIANT := $(BUILD_VARIANT)-blockcache-rust
BUILD_VARIANT := $(BUILD_VARIANT)-pagecache-rust
BUILD_VARIANT := $(BUILD_VARIANT)-dcache-rust
BUILD_VARIANT := $(BUILD_VARIANT)-procfs-rust
BUILD_VARIANT := $(BUILD_VARIANT)-procfsnode-rust
BUILD_VARIANT := $(BUILD_VARIANT)-vfspath-rust
BUILD_VARIANT := $(BUILD_VARIANT)-vfswalk-rust
BUILD_VARIANT := $(BUILD_VARIANT)-vfsmutate-rust
BUILD_VARIANT := $(BUILD_VARIANT)-vfsmntns-rust
BUILD_VARIANT := $(BUILD_VARIANT)-vfsmount-rust
BUILD_VARIANT := $(BUILD_VARIANT)-vfsfile-rust
ifeq ($(KEYRING_IMPL),rust)
BUILD_VARIANT := $(BUILD_VARIANT)-keyring-rust
endif
BUILD_VARIANT := $(BUILD_VARIANT)-signal-rust
ifeq ($(VMO_PAGER_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-vmo-pager-test
endif
ifeq ($(RUST_RUNTIME_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-rust-runtime-test
endif
ifeq ($(VMO_PAGE_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-vmo-page-test
endif
ifeq ($(MNTNS_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-mntns-test
endif
ifeq ($(RV32_SYNC_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-rv32-sync-test
endif
ifeq ($(USER_COPY_TEST),1)
BUILD_VARIANT := $(BUILD_VARIANT)-usercopy-test
endif

ifeq ($(ARCH),armv7m)
BUILD_VARIANT := $(BUILD_VARIANT)-$(BOARD)-f$(STM32_FLASH_KB)k-r$(STM32_RAM_KB)k
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(filter 1,$(STM32_QEMU)),-qemu,)$(if $(filter-out 0,$(STM32_CLUSTER_PORT)),-cluart$(STM32_CLUSTER_PORT),)
endif
ifeq ($(CLUSTER_UART_COM2),1)
BUILD_VARIANT := $(BUILD_VARIANT)-clcom2
endif
ifeq ($(MCU_SHARED_KERNEL),1)
BUILD_VARIANT := $(BUILD_VARIANT)-shared$(MCU_SHARED_RAM_KB)k$(if $(filter 1,$(MCU_SHARED_TEST)),-selftest,)
endif
ifeq ($(MCU_NATIVE_PROGRAM),1)
BUILD_VARIANT := $(BUILD_VARIANT)-program$(if $(filter 1,$(NATIVE_FLAT_FILES)),-files,)
endif
ifeq ($(RAMFS_NATIVE_TESTS),1)
BUILD_VARIANT := $(BUILD_VARIANT)-native-tests
endif
ifeq ($(QEMU_SHARED_SMP),1)
BUILD_VARIANT := $(BUILD_VARIANT)-shared-qemu
endif
# A fully integrated Rust profile can enable enough modules that the descriptive
# suffix exceeds NAME_MAX when combined with ARCH and BOARD. Keep existing short
# paths stable; hash only long variants, with the digest covering the full
# descriptive selector/configuration string.
CLUSTER_SSI ?= 0
ifneq ($(words $(CLUSTER_SSI)),1)
$(error CLUSTER_SSI must be one value: 0 or 1)
endif
ifeq ($(filter $(CLUSTER_SSI),0 1),)
$(error CLUSTER_SSI must be 0 or 1)
endif
ifeq ($(CLUSTER_SSI),1)
ifneq ($(ARCH):$(NR_CPUS):$(NOMMU),x86_64:1:0)
$(error CLUSTER_SSI=1 currently requires ARCH=x86_64 NR_CPUS=1 NOMMU=0)
endif
ifeq ($(filter $(ABI),linux both),)
$(error CLUSTER_SSI=1 requires ABI=linux or both)
endif
BUILD_VARIANT := $(BUILD_VARIANT)-ssi1
endif
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(ARCH_CONFIG_VARIANT),-$(ARCH_CONFIG_VARIANT),)
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(filter router,$(NET_ROLE)),-router,)-$(NET_STACK)
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(strip $(NET_CONNTRACK_ENTRIES)),-ct$(NET_CONNTRACK_ENTRIES),)
BUILD_VARIANT := $(BUILD_VARIANT)$(if $(filter 1,$(RAMFS_NET_TESTS)),-net-tests,)
ifneq ($(strip $(BSD_BOOTSTRAP_OBJECT)),)
ifneq ($(ARCH),x86_64)
$(error BSD_BOOTSTRAP_OBJECT currently requires ARCH=x86_64)
endif
ifeq ($(wildcard $(BSD_BOOTSTRAP_OBJECT)),)
$(error BSD_BOOTSTRAP_OBJECT does not exist)
endif
BUILD_VARIANT := $(BUILD_VARIANT)-bsdboot-$(shell sha256sum '$(BSD_BOOTSTRAP_OBJECT)' | cut -c1-12)
endif
BUILD_VARIANT_FULL := $(BUILD_VARIANT)
ifneq ($(shell printf '%s' '$(BUILD_VARIANT_FULL)' | awk 'length($$0) > 120 { print "long" }'),)
BUILD_VARIANT := $(firstword $(subst -, ,$(BUILD_VARIANT_FULL)))-$(shell printf '%s' '$(BUILD_VARIANT_FULL)' | sha256sum | cut -c1-16)
endif
BUILD_DIR = .kernel-build/$(ARCH)-$(BOARD)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img

# Gates that launch QEMU themselves need the image paths the default variant
# writes, but BUILD_VARIANT carries a suffix for every option that changes
# compiled code (see line 419) -- so a gate that spells the directory out goes
# stale the moment one of those defaults flips, and fails with a missing
# fat32.img rather than anything to do with what it was testing.  This prints
# the directory the current configuration actually uses, so a gate asks the
# Makefile instead of predicting it.  Call it with the same overrides the gate
# builds with:
#
#   $(MAKE) ARCH=x86_64 print-build-dir
.PHONY: print-build-dir
print-build-dir:
	@echo '$(BUILD_DIR)'
EXT4_IMG = $(BUILD_DIR)/ext4.img
EXT4_JOURNAL_IMG = $(BUILD_DIR)/ext4-journal.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
ISOFS_IMG = $(BUILD_DIR)/isofs.img
USER_LIBC_VARIANT = $(ARCH)$(if $(filter 1,$(NOMMU)),-nommu,)
# Driver blobs contain inline CPU/MMIO helpers, so shared-domain modules must
# never overwrite packages consumed by ordinary kernels.
USER_VARIANT = $(if $(filter llvm,$(TOOLCHAIN)),llvm-rt-,)$(USER_LIBC_VARIANT)$(if $(filter 1,$(QEMU_SHARED_SMP)),-shared-qemu,)
USER_BUILD_DIR = user/build/$(USER_VARIANT)
USER_BUILD_STAMP = $(USER_BUILD_DIR)/.build-id
ARCH_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/include
BOARD_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/boards/$(BOARD)
# Select exactly one board. Architecture recursion must not collect sibling
# boards or standalone EFI applications now owned by this same arch tree.
ARCH_KERNEL_C_SRC = $(shell find $(KERNEL_DIR)/arch/$(ARCH) -path '*/boards' -prune -o -path '*/boot/uefi' -prune -o -path '*/mcu' -prune -o -type f -name '*.c' -print | sort)
ARCH_KERNEL_ASM_SRC = $(shell find $(KERNEL_DIR)/arch/$(ARCH) -path '*/boards' -prune -o -path '*/boot/uefi' -prune -o -path '*/mcu' -prune -o -type f -name '*.S' -print | sort)
ifneq ($(filter riscv32 riscv64,$(ARCH)),)
ARCH_KERNEL_ASM_SRC += $(KERNEL_DIR)/arch/common/riscv/hyp_sbi_payload.S
endif
BOARD_DRIVER_DIR = $(BOARD_INCLUDE_DIR)/drivers
EXT4_STAGING_DIR = $(BUILD_DIR)/ext4-staging
BUILD_TIME_HDR = $(BUILD_DIR)/generated/build_time.h
STM32_BT_CONFIG_HDR = $(BUILD_DIR)/generated/stm32_bluetooth_config.h
STM32_WIFI_CONFIG_HDR = $(BUILD_DIR)/generated/stm32_wifi_config.h
FAT32_IMAGE_MB ?= 128
EXT4_IMAGE_MB ?= 128
# User software now comes from Alpine apk packages assembled by a world
# manifest (see packages/world/), not from source trees under user/external/.
# The extra *partition* survives only where a board needs a writable /extra,
# and an apk world image fills it -- see VF2_WORLD in tools/mk/targets-build.mk.
# RISCV_GNU_CC stays: the vDSO is built with it, not with an extra package.
RISCV_GNU_CC ?= riscv64-linux-gnu-gcc
USER_BUILD_ID = $(ARCH):$(NOMMU):$(USER_OPT):$(PROFILE):ENVELOPE_ACQUIRE_RACE_TEST=$(ENVELOPE_ACQUIRE_RACE_TEST)$(if $(filter 1,$(SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST)),:SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST=1,)$(if $(filter 1,$(TRAP_STORM_TEST)),:TRAP_STORM_TEST=1,)$(if $(filter 1,$(FUTEX_COW_WINDOW_TEST)),:FUTEX_COW_WINDOW_TEST=1,)
# $(wildcard) drops uninitialized git submodules (fastfetch).  The
# build must not fail when a tarball export or a non-recursive clone
# leaves those directories absent entirely.
USER_BUILD_CHECK_DIRS = $(wildcard user/init.c user/cmds user/arch user/init_common \
                        user/external/musl user/external/sbase user/external/mksh-cvs2git \
                        user/external/tlse user/external/apps/fastfetch \
                        user/extra/fastfetch_gen_headers.sh user/diagnostics packages/overlay/xfce) \
                        $(if $(filter 1,$(WIFI_RTW89)),user/wifi user/external/rtw89 tools/drivers/build_rtw89.py)
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
NATIVE_SPAWN_BIN       := $(NATIVE_BUILD_DIR)/native-spawn-$(NATIVE_TAG)
NATIVE_FUTEX_BIN       := $(NATIVE_BUILD_DIR)/native-futex-$(NATIVE_TAG)
NATIVE_LATENCY_BIN     := $(NATIVE_BUILD_DIR)/native-latency-$(NATIVE_TAG)
NATIVE_DEEPEN_BIN      := $(NATIVE_BUILD_DIR)/native-deepen-$(NATIVE_TAG)
NATIVE_DEBUG_BIN       := $(NATIVE_BUILD_DIR)/native-debug-$(NATIVE_TAG)
NATIVE_EXT_BIN         := $(NATIVE_BUILD_DIR)/native-ext-$(NATIVE_TAG)
NATIVE_KEXEC_BIN       := $(NATIVE_BUILD_DIR)/native-kexec-$(NATIVE_TAG)
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
NATIVE_ABI_BEHAVIOR_BIN := $(NATIVE_BUILD_DIR)/native-abi-behavior-$(NATIVE_TAG)
NATIVE_LINUX_BIN       := $(NATIVE_BUILD_DIR)/native-linux-$(NATIVE_TAG)
NATIVE_CHESS_BIN       := $(NATIVE_BUILD_DIR)/native-chess-$(NATIVE_TAG)
NATIVE_CLUSTER_BIN     := $(NATIVE_BUILD_DIR)/native-cluster-$(NATIVE_TAG)
NATIVE_CLUSTER_AGENT_BIN := $(NATIVE_BUILD_DIR)/cluster-agent-$(NATIVE_TAG)
NATIVE_CLUSTER_PROBE_BIN := $(NATIVE_BUILD_DIR)/cluster-task-probe-$(NATIVE_TAG)
NATIVE_CLUSTER_HANG_BIN := $(NATIVE_BUILD_DIR)/cluster-hang-probe-$(NATIVE_TAG)
NATIVE_CLUSTER_WORD8_BIN := $(NATIVE_BUILD_DIR)/native-cluster-word8-$(NATIVE_TAG)
NATIVE_CLUSTER_BENCH_BIN := $(NATIVE_BUILD_DIR)/native-cluster-bench-$(NATIVE_TAG)
NATIVE_CLUSTER_LOAD_BIN := $(NATIVE_BUILD_DIR)/native-cluster-load-$(NATIVE_TAG)
NATIVE_APPCTL_BIN := $(NATIVE_BUILD_DIR)/appctl-run-$(NATIVE_TAG)
NATIVE_APPD_BIN := $(NATIVE_BUILD_DIR)/appd-$(NATIVE_TAG)
NATIVE_APPEXEC_BIN := $(NATIVE_BUILD_DIR)/app-exec-$(NATIVE_TAG)
NATIVE_APP_TEST_BIN := $(NATIVE_BUILD_DIR)/native-apps-$(NATIVE_TAG)
NATIVE_APP_CHILD_BIN := $(NATIVE_BUILD_DIR)/native-app-child-$(NATIVE_TAG)
NATIVE_APP_EXIT_BIN := $(NATIVE_BUILD_DIR)/native-app-exit-$(NATIVE_TAG)
NATIVE_OBJECTD_BIN := $(NATIVE_BUILD_DIR)/objectd-$(NATIVE_TAG)
NATIVE_OBJECT_TEST_BIN := $(NATIVE_BUILD_DIR)/native-objects-$(NATIVE_TAG)
NATIVE_CLUSTER_INSTANCE_BIN := $(NATIVE_BUILD_DIR)/native-cluster-instance-$(NATIVE_TAG)
NATIVE_CLUSTER_MESH_BIN := $(NATIVE_BUILD_DIR)/native-cluster-mesh-$(NATIVE_TAG)
NATIVE_CLUSTER_WORDCOUNT_BIN := $(NATIVE_BUILD_DIR)/cluster-wordcount-$(NATIVE_TAG)
NATIVE_JOBD_BIN := $(NATIVE_BUILD_DIR)/jobd-$(NATIVE_TAG)
NATIVE_CLUSTER_SUPERVISOR_BIN := $(NATIVE_BUILD_DIR)/cluster-supervisor-$(NATIVE_TAG)
NATIVE_CLUSTER_SUPERVISOR_TEST_BIN := $(NATIVE_BUILD_DIR)/native-cluster-supervisor-$(NATIVE_TAG)
NATIVE_CLUSTERD_BIN := $(NATIVE_BUILD_DIR)/clusterd-$(NATIVE_TAG)
NATIVE_OUTPUTS         := $(NATIVE_HANDLE_BIN) \
                          $(NATIVE_LIBC_BIN) $(NATIVE_FUTEX_BIN) $(NATIVE_SPAWN_BIN) $(NATIVE_DEEPEN_BIN) \
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
                          $(NATIVE_ABI_BEHAVIOR_BIN) \
                          $(NATIVE_DEBUG_BIN) $(NATIVE_EXT_BIN) $(NATIVE_KEXEC_BIN) \
                          $(NATIVE_CHESS_BIN) $(NATIVE_CLUSTER_BIN) \
                          $(NATIVE_CLUSTER_AGENT_BIN) $(NATIVE_CLUSTER_PROBE_BIN) \
                          $(NATIVE_CLUSTER_HANG_BIN) $(NATIVE_CLUSTERD_BIN) $(NATIVE_JOBD_BIN) $(NATIVE_CLUSTER_MESH_BIN) $(NATIVE_CLUSTER_INSTANCE_BIN) $(NATIVE_OBJECTD_BIN) $(NATIVE_OBJECT_TEST_BIN) $(NATIVE_APPCTL_BIN) $(NATIVE_APPD_BIN) $(NATIVE_APPEXEC_BIN) $(NATIVE_APP_TEST_BIN) $(NATIVE_APP_CHILD_BIN) $(NATIVE_APP_EXIT_BIN) $(NATIVE_CLUSTER_SUPERVISOR_BIN) $(NATIVE_CLUSTER_SUPERVISOR_TEST_BIN) $(NATIVE_CLUSTER_WORDCOUNT_BIN) $(NATIVE_CLUSTER_WORD8_BIN) $(NATIVE_CLUSTER_BENCH_BIN) $(NATIVE_CLUSTER_LOAD_BIN)
# fake_ld.c provides entry shims for these four 64-bit architectures.
NATIVE_OUTPUTS += $(if $(filter riscv64 aarch64 x86_64 loongarch64,$(ARCH)),$(NATIVE_FAKELD_BIN) $(NATIVE_DYNPROBE_BIN))
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
# Per-instance network backend.  Empty = the SLIRP user backend below, which
# every existing instance keeps using.  An instance overrides it through
# [net].backend (emitted as this variable by tools/a20) to attach its NIC to
# something else -- the cluster demo uses QEMU's socket UDP tunnel so two
# instances share one L2 segment (docs/cluster/02-udp-demo.md).  hostfwd only
# exists on the user backend, which is why NET_HOSTFWD folds into the default
# and not into an override.
NET_BACKEND ?=
ifeq ($(strip $(NET_BACKEND)),)
NET_BACKEND = user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
endif
NETDEV_USER = -netdev $(NET_BACKEND)
# Per-instance NIC MAC ([net].mac -> this variable).  Empty = QEMU's built-in
# default, which is fine for one guest but collides as soon as two instances
# share an L2 segment.
NET_MAC ?=
# Guest-side static IPv4 ([net].guest_ip / guest_netmask / guest_gateway ->
# these variables), assembled into the kernel's a20.* boot arguments below.
# Empty = unchanged behaviour: DHCP against the SLIRP backend, or an
# unconfigured link.  The key names are the kernel contract in
# kernel/net/control/net_config.c; the net smokes in tools/mk/targets-smoke.mk pass the
# same keys by hand.
NET_GUEST_IP ?=
NET_GUEST_NETMASK ?=
NET_GUEST_GATEWAY ?=
SMOKE_TIMEOUT ?= 20s
# The DNAT gate boots a guest, waits for it to install a NAT rule, and then
# needs the host-side probe's own retry window on top, so it needs more than
# the single-shot serial gates use.
SMOKE_TIMEOUT_NAT ?= 90s
# TCG boot can take longer than two seconds after a full image rebuild.  Wait
# until the interactive mksh has had time to print its prompt before injecting
# smoke commands; PASS markers and clean poweroff still decide the result.
SMOKE_INPUT_DELAY ?= 8
# A 4-core TCG run plus net_stress_test's 4 concurrent x 4 MiB transfers is
# much slower than the single-core defaults, so it needs its own budget.
SMOKE_TIMEOUT_SMP ?= 180s
# The ext4 journal gate runs eight TCG boots and four host fsck passes, so it
# needs far longer than a single smoke boot and a longer settle time than the
# interactive-shell default.
SMOKE_TIMEOUT_EXT4 ?= 180
SMOKE_INPUT_DELAY_EXT4 ?= 22
# The AHCI gate boots x86_64 with an extra controller behind the root bus and
# runs the fsync test against it, so it needs both a later shell injection
# point (two HBAs are probed before the shell prompt) and a longer budget than
# a single-device boot.
SMOKE_TIMEOUT_AHCI ?= 120s
SMOKE_INPUT_DELAY_AHCI ?= 20
# The e1000 gate boots x86_64, injects a listener, and then needs the host-side
# probe's own retry window on top (the guest must have booted and opened the port
# before a connection can land).  It also probes a second PCI function behind the
# root bus before the shell prompt appears, so its injection point is later than
# the single-device default for the same reason AHCI's is.
SMOKE_TIMEOUT_E1000 ?= 120s
SMOKE_INPUT_DELAY_E1000 ?= 20
# Same shape as the e1000 gate and the same reason for each value: the rtl8139
# probe's own retry window sits on top of the guest's boot time, and the guest
# needs to enumerate a PCI function, ring programming and a listener open
# before the forwarded connection can land.
SMOKE_TIMEOUT_RTL8139 ?= 120s
SMOKE_INPUT_DELAY_RTL8139 ?= 20
# virtio-console: the host probe first waits for the guest to publish
# "VPORT_TEST: READY" (boot, DriverStore activation of virtio-console.a20drv,
# /dev/vport0 open), then holds its own 90 s window for the echo.
SMOKE_TIMEOUT_VPORT ?= 150s
SMOKE_INPUT_DELAY_VPORT ?= 20
SMOKE_LOG_DIR ?= .kernel-build/smoke
STEP35_TIMEOUT ?= 300s
STEP35_LOG_DIR ?= .kernel-build/smoke/step35
WAIT_TIMER_HEAP_MAX ?=
WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS ?= 0
ifeq ($(WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS),1)
ifneq ($(WAIT_TIMER_HEAP_MAX),1)
$(error WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS=1 requires WAIT_TIMER_HEAP_MAX=1)
endif
endif
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
RISCV_ELF_RV32_MULTIDIR := $(if $(filter gcc,$(TOOLCHAIN)),$(shell if command -v $(RISCV_ELF_PREFIX)gcc >/dev/null 2>&1; then \
	$(RISCV_ELF_PREFIX)gcc -march=rv32imafdc -mabi=ilp32d -print-multi-directory 2>/dev/null; \
fi))
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
# The canary lives at the absolute address of the global guard symbol: the
# kernel never touches FS or GS bases for stack protection, so the syscall and
# IRQ entry paths need no FS-base save/swap/restore round trip per crossing.
# PPC64LE needs the same =global form because it is PIC.
ARCH_CFLAGS_x86_64      := -m64 -mcmodel=large -mno-red-zone -fno-pic -fno-pie -mgeneral-regs-only -fno-omit-frame-pointer \
                           -mstack-protector-guard=global
ARCH_CFLAGS_arm32       := -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -static -mno-unaligned-access -fno-omit-frame-pointer
ARCH_CFLAGS_armv7m      := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-short-enums -fno-pic -static \
                           -ffunction-sections -fdata-sections -fno-unwind-tables \
                           -fno-asynchronous-unwind-tables
ARCH_CFLAGS_riscv32     := -march=rv32imafdc -mabi=ilp32d -mcmodel=medany -fno-pic -static
# ppc64le 独自需要 -mstack-protector-guard=global：PPC64 ELFv2 的 GCC 默认把
# canary 当成 TLS 变量经 r13 取（ld 9,-28688(13)），而内核的 __stack_chk_guard
# 是 kernel/core/stack_protector.c 里的普通 .data 全局，且 r13 已被
# arch_set_task_pointer() 征用为 task 指针、从不指向 TLS 基址。带 r13 取值的
# __stack_chk_guard 会在 kernel_main 的第一个 printf 里直接 DSEG（canary 读到
# 垃圾页，随后程序校验杀死内核）。其余架构的 GCC 默认就是 global，无需覆盖。
ARCH_CFLAGS_ppc64le     := -m64 -mcpu=power8 -mtune=power8 -mlong-double-64 -fno-pic -static -mno-vsx -mno-altivec -fno-tree-vectorize -mstack-protector-guard=global

PHYS_BASE_aarch64     := 0x40080000
PHYS_BASE_arm32       := 0x40080000
PHYS_BASE_armv7m      := 0x20000000
PHYS_BASE_ppc64le     := 0x00400000
PHYS_BASE_riscv32     := 0x80200000
PHYS_BASE_riscv64     := 0x80200000
PHYS_BASE_x86_64      := $(X86_KERNEL_LOAD_ADDRESS_CANONICAL)
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
ARCH_LIBS_loongarch32 = $(shell $(CROSS_PREFIX_loongarch32)gcc $(ARCH_CFLAGS_loongarch32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_aarch64     :=
ARCH_LIBS_x86_64      :=
ARCH_LIBS_arm32       = $(shell $(CROSS_PREFIX_arm32)gcc $(ARCH_CFLAGS_arm32) -print-libgcc-file-name 2>/dev/null)
ARCH_LIBS_armv7m      :=
ARCH_LIBS_riscv32     = $(shell $(CROSS_PREFIX_riscv32)gcc $(ARCH_CFLAGS_riscv32) -print-libgcc-file-name 2>/dev/null)
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
# aarch64 was missing here, so an instance declaring [rootfs].world -- which is
# every xfce-*-style Alpine desktop, xfce-aarch64 included -- expanded to an
# empty device and emitted "-device ,drive=xworld".
#
# The second disk goes on a second virtio-mmio bus, not on PCI: the board's
# enumerate_devices only calls virtio_mmio_enumerate and never initialises the
# PCI host, so a virtio-blk-pci here would satisfy QEMU and still be invisible to
# the kernel.  The primary disk takes bus.0, which is why riscv64 does this too.
QEMU_BLK_SECOND_aarch64     := virtio-blk-device,bus=virtio-mmio-bus.1

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
ARCH_CFLAGS  := $(ARCH_CFLAGS_$(ARCH)) $(ARCH_CONFIG_CFLAGS)
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH)) $(ARCH_CONFIG_LDFLAGS)
ARCH_LIBS    := $(if $(filter gcc,$(TOOLCHAIN)),$(ARCH_LIBS_$(ARCH)))
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
# Boards may select a QEMU model/handoff without changing architecture code.
-include $(BOARD_INCLUDE_DIR)/board.mk
QEMU_MACHINE_CPUS ?= $(NR_CPUS)
QEMU_KERNEL ?= $(KERNEL_ELF)
QEMU_FLAGS := $(or $(QEMU_FLAGS_BASE_$(BOARD)),$(QEMU_FLAGS_BASE_$(ARCH))) \
              -m $(QEMU_MEMORY) -nographic -smp $(QEMU_MACHINE_CPUS) $(QEMU_BOOT_FLAGS)
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

ifeq ($(TOOLCHAIN):$(ARCH),gcc:armv7m)
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

ifeq ($(TOOLCHAIN),llvm)
CC := $(LLVM_CC)
OBJCOPY := $(LLVM_PREFIX)llvm-objcopy$(LLVM_SUFFIX)
ARCH_CFLAGS := $(filter-out -fno-store-merging -mstrict-align,$(ARCH_CFLAGS)) -fno-pic -fno-pie $(LLVM_KERNEL_WARNINGS)
ARCH_LDFLAGS += -no-pie $(LLVM_LINK)
ifeq ($(ARCH),loongarch64)
ARCH_CFLAGS := $(filter-out -mcmodel=normal,$(ARCH_CFLAGS)) -mcmodel=medium
endif
ARCH_LIBS := $(LLVM_KERNEL_BUILTINS)
endif

# The handoff bundle reads the kernel's load address out of the ELF program
# headers rather than repeating a per-board address, so it needs a readelf that
# understands this architecture's objects.
READELF := $(if $(filter llvm,$(TOOLCHAIN)),$(LLVM_PREFIX)llvm-readelf$(LLVM_SUFFIX),$(CROSS_PREFIX)readelf)

ifeq ($(CC),)
$(error Unsupported ARCH '$(ARCH)')
endif

MKFS_FAT ?= $(or $(shell command -v mkfs.fat 2>/dev/null),$(wildcard /usr/sbin/mkfs.fat),$(wildcard /sbin/mkfs.fat),mkfs.fat)
MKFS_EXT4 ?= $(or $(shell command -v mkfs.ext4 2>/dev/null),$(wildcard /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4),$(wildcard /usr/local/opt/e2fsprogs/sbin/mkfs.ext4),$(wildcard /usr/sbin/mkfs.ext4),$(wildcard /sbin/mkfs.ext4),mkfs.ext4)
LIBGCC_S_ARCH := $(if $(filter gcc,$(TOOLCHAIN)),$(shell $(CC) $(ARCH_CFLAGS) -print-file-name=libgcc_s.so.1 2>/dev/null))
ifeq ($(LIBGCC_S_ARCH),libgcc_s.so.1)
LIBGCC_S_ARCH :=
endif

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device $(QEMU_BLK),drive=x0
QEMU_FLAGS += $(NETDEV_USER) -device $(QEMU_NET),netdev=net$(if $(strip $(NET_MAC)),$(comma)mac=$(NET_MAC))
# Guest static IPv4, when the instance declares one.  a20.dhcp=0 keeps lwIP
# from racing a DHCP client against the static address on a link that has no
# DHCP server (the cluster's UDP tunnel is one).  The a20.* key names are the
# kernel contract in kernel/net/control/net_config.c.
ifneq ($(strip $(NET_GUEST_IP)),)
NET_GUEST_APPEND := a20.dhcp=0 a20.ip=$(NET_GUEST_IP)
ifneq ($(strip $(NET_GUEST_NETMASK)),)
NET_GUEST_APPEND += a20.netmask=$(NET_GUEST_NETMASK)
endif
ifneq ($(strip $(NET_GUEST_GATEWAY)),)
NET_GUEST_APPEND += a20.gateway=$(NET_GUEST_GATEWAY)
endif
endif
# The bulk console runner merges independent UART/virtserialport sockets.
# Avoid QEMU's lossy virtconsole path and stdio-mux writable-watch limitation.
# Keyboard input retains the existing UART TTY signal/foreground discipline;
# userspace output changes transport after the bulk binding is published.
QEMU_CONSOLE ?= serial
ifeq ($(QEMU_CONSOLE),virtio)
ifneq ($(ARCH),x86_64)
$(error QEMU_CONSOLE=virtio currently requires ARCH=x86_64)
endif
ifneq ($(DISPLAY_MODE),text)
$(error QEMU_CONSOLE=virtio requires DISPLAY_MODE=text)
endif
QEMU_FLAGS := $(filter-out -nographic,$(QEMU_FLAGS)) -display none -monitor none
QEMU_CONSOLE_RUNNER := $(PYTHON) tools/qemu_console.py --
NET_GUEST_APPEND += a20.console=vport0
endif
ifneq ($(strip $(NET_GUEST_APPEND)),)
QEMU_FLAGS += -append '$(NET_GUEST_APPEND)'
endif
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
CFLAGS = $(if $(filter lwip,$(NET_STACK)),-DCONFIG_LWIP_UPSTREAM=1,) -Wall -Wextra $(OPT) -ffreestanding -nostdlib \
         -fno-builtin -fno-common -std=gnu99 \
         -MMD -MP \
         -I$(ARCH_INCLUDE_DIR) -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(KERNEL_DIR)/net/backends/lwip/port \
         $(if $(LWIP_SOURCE_ROOT),-I$(LWIP_SOURCE_ROOT)/src/include,) \
         -I$(KERNEL_DIR)/external/littlefs \
         -I$(KERNEL_DIR)/external/littlefs/compat \
         -DLFS_MALLOC=lfs_kmalloc -DLFS_FREE=lfs_kfree \
         -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT -DLFS_NO_TRACE \
         -I$(BOARD_INCLUDE_DIR) -I$(BOARD_INCLUDE_DIR)/include -I$(BUILD_DIR)/generated $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_ABI_$(shell echo $(ABI) | tr a-z A-Z) \
         -DCONFIG_NR_CPUS=$(NR_CPUS) \
         -DCONFIG_BOARD_$(shell echo $(BOARD) | tr a-z A-Z | tr - _) \
         -DCONFIG_NET_LANES=$(NET_LANES) \
         -DCONFIG_NET_PROFILE=$(NET_PROFILE) \
         -DCONFIG_NET_ROUTER=$(if $(filter router,$(NET_ROLE)),1,0) \
         -DCONFIG_NET_BACKEND_NAME='"$(NET_STACK)"' \
         -DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE) \
         -DCONFIG_SLAB_DEBUG=$(CONFIG_SLAB_DEBUG)
ifneq ($(strip $(NET_CONNTRACK_ENTRIES)),)
CFLAGS += -DCONFIG_NET_CONNTRACK_ENTRIES=$(NET_CONNTRACK_ENTRIES)
endif
ifneq ($(filter lwip native,$(NET_STACK)),)
CFLAGS += -DCONFIG_NET_BACKEND_LWIP=1
endif
ifeq ($(NET_STACK),bsd)
CFLAGS += -DCONFIG_NET_BACKEND_BSD=1
endif
ifeq ($(NETFILTER_REWRITE_IMPL),rust)
CFLAGS += -DCONFIG_NETFILTER_REWRITE_RUST
endif
ifeq ($(TASK_OBJECTS_IMPL),rust)
CFLAGS += -DCONFIG_TASK_OBJECT_RUST
endif
ifeq ($(CHANNEL_SEND_IMPL),rust)
CFLAGS += -DCONFIG_A20_CHANNEL_SEND_RUST
endif
ifeq ($(CHANNEL_RECV_TXN_IMPL),rust)
CFLAGS += -DCONFIG_CHANNEL_RECV_TXN_RUST
endif
ifeq ($(HANDLE_SLOT_TRANSITIONS_IMPL),rust)
CFLAGS += -DCONFIG_HANDLE_SLOT_TRANSITIONS_RUST
endif
ifeq ($(PROCESS_VM_IMPL),rust)
CFLAGS += -DCONFIG_PROCESS_VM_RUST
endif
ifeq ($(PARK_WAIT_IMPL),rust)
CFLAGS += -DCONFIG_PARK_WAIT_RUST
endif
ifeq ($(WAIT_QUEUE_IMPL),rust)
CFLAGS += -DCONFIG_WAIT_QUEUE_RUST
endif
ifeq ($(RW_MUTEX_IMPL),rust)
CFLAGS += -DCONFIG_RW_MUTEX_RUST
endif
ifeq ($(MUTEX_STATE_IMPL),rust)
CFLAGS += -DCONFIG_MUTEX_STATE_RUST
endif
ifeq ($(COMPLETION_IMPL),rust)
CFLAGS += -DCONFIG_COMPLETION_RUST
endif
ifeq ($(MM_SHUTDOWN_WRITE_IMPL),rust)
CFLAGS += -DCONFIG_MM_SHUTDOWN_WRITE_RUST
endif
ifeq ($(TASK_LIFECYCLE_IMPL),rust)
CFLAGS += -DCONFIG_TASK_LIFECYCLE_RUST
endif
ifeq ($(TASK_EXIT_IMPL),rust)
CFLAGS += -DCONFIG_TASK_EXIT_RUST
endif
ifeq ($(WAIT4_IMPL),rust)
CFLAGS += -DCONFIG_WAIT4_RUST
endif
ifeq ($(TASK_PLACEMENT_IMPL),rust)
CFLAGS += -DCONFIG_TASK_PLACEMENT_RUST
endif
ifeq ($(EEVDF_MATH_IMPL),rust)
CFLAGS += -DCONFIG_EEVDF_MATH_RUST
endif
ifeq ($(RT_SCHED_IMPL),rust)
CFLAGS += -DCONFIG_RT_SCHED_RUST
endif
ifeq ($(TASK_CONSTRUCTION_IMPL),rust)
CFLAGS += -DCONFIG_TASK_CONSTRUCTION_RUST
endif
ifeq ($(TASK_CONSTRUCTION_TEST),1)
CFLAGS += -DCONFIG_TASK_CONSTRUCTION_TEST
endif
ifeq ($(ELF_EXEC_TEST),1)
CFLAGS += -DCONFIG_ELF_EXEC_TEST
endif
ifeq ($(TRAP_STORM_TEST),1)
CFLAGS += -DCONFIG_TRAP_STORM_TEST
endif
ifeq ($(FUTEX_COW_WINDOW_TEST),1)
CFLAGS += -DCONFIG_FUTEX_COW_WINDOW_TEST
endif
ifeq ($(PID_IMPL),rust)
CFLAGS += -DCONFIG_PID_RUST
endif
ifneq ($(PROFILE),mcu)
CFLAGS += -DCONFIG_ALARM_TIMER_RUST
endif
ifeq ($(VMA_IMPL),rust)
CFLAGS += -DCONFIG_VMA_REGION_RUST
endif
ifeq ($(VMO_PAGE_IMPL),rust)
CFLAGS += -DCONFIG_VMO_PAGE_RUST
endif
ifeq ($(VMO_LIFECYCLE_IMPL),rust)
CFLAGS += -DCONFIG_VMO_LIFECYCLE_RUST
endif
ifeq ($(VMO_PAGER_IMPL),rust)
CFLAGS += -DCONFIG_VMO_PAGER_RUST
endif
ifeq ($(DRIVER_REGISTRY_IMPL),rust)
CFLAGS += -DCONFIG_DRIVER_REGISTRY_RUST
endif
ifeq ($(VIRTIO_BLK_CORE_IMPL),rust)
CFLAGS += -DCONFIG_VIRTIO_BLK_CORE_RUST
endif
ifeq ($(ENVELOPE_POLICY_IMPL),rust)
CFLAGS += -DCONFIG_ENVELOPE_POLICY_RUST
endif
ifeq ($(ENVELOPE_ACQUIRE_RACE_TEST),1)
CFLAGS += -DCONFIG_ENVELOPE_ACQUIRE_RACE_TEST
endif
CFLAGS += -DCONFIG_FDTABLE_STATE_RUST
ifeq ($(EVENTPOLL_STATE_IMPL),rust)
CFLAGS += -DCONFIG_EVENTPOLL_STATE_RUST
endif
ifeq ($(EVENT_DESCRIPTOR_STATE_IMPL),rust)
CFLAGS += -DCONFIG_EVENT_DESCRIPTOR_STATE_RUST
endif
CFLAGS += -DCONFIG_SYSV_SEM_RUST
CFLAGS += -DCONFIG_POSIX_MQ_RUST
CFLAGS += -DCONFIG_SYSV_MSG_RUST
CFLAGS += -DCONFIG_SYSV_SHM_RUST
ifeq ($(USERFAULTFD_STATE_IMPL),rust)
CFLAGS += -DCONFIG_USERFAULTFD_STATE_RUST
endif
ifeq ($(FUTEX_WAIT_IMPL),rust)
CFLAGS += -DCONFIG_FUTEX_WAIT_RUST
endif
ifeq ($(LINUX_SCM_RIGHTS_IMPL),rust)
CFLAGS += -DCONFIG_LINUX_SCM_RIGHTS_RUST
endif
ifeq ($(LINUX_MMSG_TXN_IMPL),rust)
CFLAGS += -DCONFIG_LINUX_MMSG_TXN_RUST
endif
ifeq ($(NET_SOCKET_QUEUE_IMPL),rust)
CFLAGS += -DCONFIG_NET_SOCKET_QUEUE_RUST
endif
ifeq ($(KEYRING_IMPL),rust)
CFLAGS += -DCONFIG_KEYRING_RUST
endif
ifeq ($(VMO_PAGER_TEST),1)
CFLAGS += -DCONFIG_VMO_PAGER_TEST
endif
ifeq ($(RUST_RUNTIME_TEST),1)
CFLAGS += -DCONFIG_RUST_RUNTIME_PANIC_TEST
endif
ifeq ($(VMO_PAGE_TEST),1)
CFLAGS += -DCONFIG_VMO_PAGE_TEST
endif
ifeq ($(MNTNS_TEST),1)
CFLAGS += -DCONFIG_MNTNS_TEST
endif
ifeq ($(RV32_SYNC_TEST),1)
CFLAGS += -DCONFIG_RV32_SYNC_TEST
endif
ifeq ($(USER_COPY_TEST),1)
CFLAGS += -DCONFIG_USER_COPY_TEST
endif
ifeq ($(filter 1,$(KERNEL_WERROR)),1)
CFLAGS += -Werror
endif

# Escape hatch for experiment knobs that cannot come from the command line.
# x86_64 is the motivating case: QEMU does not publish -append through fw_cfg
# on the -kernel <ELF> boot path (docs 10.31/10.32), so a20.* parameters are
# unreachable there.  Building two images with
# EXTRA_CFLAGS=-DCONFIG_ANON_PROV_DEFAULT=<n> and interleaving the runs gives
# the same time-adjacent A/B that a runtime switch gives elsewhere.
ifeq ($(CLUSTER_UART_COM2),1)
CFLAGS += -DCONFIG_CLUSTER_UART_COM2
endif
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)
CFLAGS += -DCONFIG_VFS_RUST_LIFECYCLE
ifeq ($(EXT4_EXTENT_TXN_IMPL),rust)
CFLAGS += -DCONFIG_EXT4_RUST_EXTENT_TXN
endif
CFLAGS += -DCONFIG_UXFS_VNODE_IDENTITY_RUST
CFLAGS += -DCONFIG_UXFS_RPC_STATE_RUST
CFLAGS += -DCONFIG_UXFS_OPS_STATE_RUST
CFLAGS += -DCONFIG_BLOCK_CACHE_STATE_RUST
CFLAGS += -DCONFIG_PAGE_CACHE_STATE_RUST
CFLAGS += -DCONFIG_VFS_DCACHE_STATE_RUST
CFLAGS += -DCONFIG_PROCFS_STATE_RUST
CFLAGS += -DCONFIG_PROCFS_NODE_STATE_RUST
CFLAGS += -DCONFIG_VFS_PATH_STATE_RUST
CFLAGS += -DCONFIG_VFS_PATH_WALK_STATE_RUST
CFLAGS += -DCONFIG_VFS_MUTATE_STATE_RUST
CFLAGS += -DCONFIG_VFS_MNTNS_STATE_RUST
CFLAGS += -DCONFIG_VFS_MOUNT_STATE_RUST
CFLAGS += -DCONFIG_FILE_STATE_RUST
ifeq ($(QEMU_SHARED_SMP),1)
ifneq ($(BOARD),qemu-virt-x86_64)
$(error QEMU_SHARED_SMP requires BOARD=qemu-virt-x86_64)
endif
ifneq ($(NR_CPUS),2)
$(error QEMU_SHARED_SMP currently requires NR_CPUS=2)
endif
ifneq ($(BRINGUP),1)
$(error QEMU_SHARED_SMP currently requires BRINGUP=1: full disk-root and graphics ownership are not validated)
endif
CFLAGS += -DCONFIG_QEMU_SHARED_SMP -DCONFIG_SHARED_DEVICE_CALL -DCONFIG_SHARED_MMIO_OWNER=0
endif
# 内核栈金丝雀：__stack_chk_guard/__stack_chk_fail 由 kernel/core/stack_protector.c
# 提供；汇编（entry/trampoline/vdso）无 canary。lwip 等 vendored 代码在
# tools/mk/targets-images.mk 的专属规则里显式 -fno-stack-protector，不受影响。
CONFIG_STACK_PROTECTOR ?= 1
ifeq ($(filter 1,$(CONFIG_STACK_PROTECTOR)),1)
CFLAGS += -fstack-protector-strong -DCONFIG_STACK_PROTECTOR=1
endif
ifeq ($(filter 1,$(CONFIG_UBSAN)),1)
# Undefined Behavior Sanitizer: kernel/core/ubsan.c provides the handlers.
# alignment/bounds-strict are excluded to match the packed-struct and
# flexible-array idioms the kernel deliberately uses.
CFLAGS += -fsanitize=undefined -fno-sanitize=alignment $(if $(filter gcc,$(TOOLCHAIN)),-fno-sanitize=bounds-strict,) \
          -DCONFIG_UBSAN=1
endif
ifneq ($(strip $(WAIT_TIMER_HEAP_MAX)),)
CFLAGS += -DCONFIG_WAIT_TIMER_HEAP_MAX=$(WAIT_TIMER_HEAP_MAX)
endif
ifeq ($(WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS),1)
CFLAGS += -DCONFIG_WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS
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
# 内核抢占（CONFIG_KERNEL_PREEMPT）：hosted 与 RV32 M-mode 默认使用 IRQ 返回点。
# armv7m 用 PendSV 建模切换点，preempt 计数/tick 驱动的内核抢占模型与之不兼容；
# 该 profile 的切换点必须由 PendSV 触发，不能由 IRQ 尾声触发。
ifneq ($(ARCH),armv7m)
ifeq ($(filter 1,$(CONFIG_KERNEL_PREEMPT)),1)
CFLAGS += -DCONFIG_KERNEL_PREEMPT
endif
endif
# ARM32's short-descriptor abort path cannot safely demand-page large GUI
# executables yet; eager loading also handles their shared PT_LOAD tail page.
ifeq ($(ARCH),arm32)
CFLAGS += -DCONFIG_ELF_EAGER_LOAD
endif
ifeq ($(BOARD),ls2k1000)
ifeq ($(COOPERATIVE_BOOT),1)
CFLAGS += -DCONFIG_ELF_EAGER_LOAD
# Register dumps at the first task switch and on panic.  This used to be keyed on
# CONFIG_BOARD_LS2K1000 inside kernel/core/panic.c and kernel/proc/task.c, which
# put a board name in architecture-neutral files; the flag is opt-in here
# instead, and the CSRs themselves moved to
# kernel/arch/loongarch64/platform/debug.c behind arch_panic_dump() and
# arch_debug_dump_user_state().
CFLAGS += -DCONFIG_DEBUG_BOOT_TRACE
ifneq ($(NR_CPUS),1)
$(error COOPERATIVE_BOOT=1 is the single-core recovery profile and cannot be combined with NR_CPUS=$(NR_CPUS))
endif
ifeq ($(STORAGE_READ_ONLY),1)
$(error STORAGE_READ_ONLY=1 is an experiment and cannot be combined with the LS2K1000 cooperative recovery profile)
endif
endif
endif
# LS2K0300 first hardware image deliberately uses a polled, single-core shell.
ifeq ($(BOARD),ls2k0300)
ifneq ($(ARCH),loongarch64)
$(error BOARD=ls2k0300 requires ARCH=loongarch64)
endif
ifneq ($(NR_CPUS),1)
$(error LS2K0300 is a single-core SoC; use NR_CPUS=1)
endif
CFLAGS += -DCONFIG_COOPERATIVE_BOOT -DCONFIG_ELF_EAGER_LOAD
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
# CONFIG_<architecture>.
# The per-arch defines and the PCIe arch lists come from
# components/trim.toml via components/trim.mk.
# ------------------------------------------------------------------
CFLAGS += $(TRIM_ARCH_CPPFLAGS_$(ARCH))

# Boards whose SoC has no PCIe root complex must leave the MMIO-allocation
# window undefined, not defined-as-zero: pci_bus.c would otherwise map physical
# address 0 as a device window.  The SophGo SG2000/CV1800B boards are the
# first RISC-V targets in the tree without PCIe, and are named as
# board_exclusions of the two pcie-mmio capabilities in components/trim.toml.
ifeq ($(filter $(BOARD),$(TRIM_CAP_PCIE_MMIO_ALLOC_BOARD_EXCLUSIONS)),)
ifneq ($(filter $(ARCH),$(TRIM_CAP_PCIE_MMIO_ALLOC_ARCHES)),)
CFLAGS += -DCONFIG_PCI_MMIO_ALLOC
endif
endif
ifeq ($(filter $(BOARD),$(TRIM_CAP_PCIE_MMIO_ECAM_BOARD_EXCLUSIONS)),)
ifneq ($(filter $(ARCH),$(TRIM_CAP_PCIE_MMIO_ECAM_ARCHES)),)
CFLAGS += -DCONFIG_PCI_MMIO_BASE_ECAM
endif
endif
ELF_MACHINE_armv7m      := 40
ELF_CLASS_armv7m        := 1
NATIVE_CALL_ABI_x86_64 := A20_EXEC_ABI_X86_64
NATIVE_CALL_ABI_aarch64 := A20_EXEC_ABI_AARCH64
NATIVE_CALL_ABI_riscv64 := A20_EXEC_ABI_RV64_DOUBLE
NATIVE_CALL_ABI_riscv32 := $(if $(filter mcu,$(PROFILE)),A20_EXEC_ABI_RV32_SOFT,A20_EXEC_ABI_RV32_DOUBLE)
NATIVE_CALL_ABI_arm32 := A20_EXEC_ABI_ARM_HARD
NATIVE_CALL_ABI_armv7m := A20_EXEC_ABI_THUMB_SOFT
NATIVE_CALL_ABI_loongarch64 := A20_EXEC_ABI_LA64_DOUBLE
NATIVE_CALL_ABI_loongarch32 := A20_EXEC_ABI_LA32_SOFT
NATIVE_CALL_ABI_ppc64le := A20_EXEC_ABI_PPC64_ELFV2
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
CFLAGS += -DARCH_ELF_MACHINE=$(ELF_MACHINE_$(ARCH)) \
          -DARCH_ELF_CLASS=$(ELF_CLASS_$(ARCH)) \
          -DARCH_NATIVE_CALL_ABI=$(NATIVE_CALL_ABI_$(ARCH))
ifeq ($(ARCH),armv7m)
CFLAGS += -DSTM32_FLASH_KB=$(STM32_FLASH_KB) -DSTM32_RAM_KB=$(STM32_RAM_KB)
ifeq ($(MCU_SHARED_KERNEL),1)
# Distinct physical chips do not share a Cortex-M exclusive monitor.
# Every GCC atomic operation goes through the fabric semaphore implementation.
CFLAGS += -DCONFIG_MCU_SHARED_KERNEL -DCONFIG_SHARED_FABRIC_ATOMICS \
          -fno-inline-atomics -mno-unaligned-access
ifeq ($(MCU_SHARED_TEST),1)
CFLAGS += -DCONFIG_MCU_SHARED_TEST
endif
ifeq ($(MCU_NATIVE_PROGRAM),1)
CFLAGS += -DCONFIG_MCU_NATIVE_PROGRAM -DCONFIG_NATIVE_TASK_HANDLES_ONLY -DCONFIG_NATIVE_FLAT_THREAD
ifeq ($(NATIVE_FLAT_FILES),1)
CFLAGS += -DCONFIG_NATIVE_FLAT_FILES
endif
ifeq ($(MCU_SHARED_TEST),1)
CFLAGS += -DMCU_TEST_ELF_PATH=\"$(BUILD_DIR)/mcu-test-program.elf\"
endif
endif
endif


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
ifeq ($(filter $(STM32_CLUSTER_PORT),0 2 3),)
$(error STM32_CLUSTER_PORT must be 0, 2 or 3; USART1 is the console)
endif
ifneq ($(STM32_CLUSTER_PORT),0)
ifeq ($(STM32_QEMU),1)
$(error STM32_CLUSTER_PORT requires a physical STM32 configuration)
endif
CFLAGS += -DCONFIG_STM32_CLUSTER_PORT=$(STM32_CLUSTER_PORT)
endif
ifeq ($(STM32_QEMU),1)
# The 8 KiB QEMU probe carries only idle; use the existing bounded timer
# heap override instead of spending 2.5 KiB on the 64-entry MCU default.
CFLAGS += -DCONFIG_STM32_QEMU -DCONFIG_MCU_DISABLE_CLUSTER_LEAF -DCONFIG_WAIT_TIMER_HEAP_MAX=4
endif
endif

ifeq ($(ARCH),riscv32)
ifeq ($(MCU_NATIVE_PROGRAM),1)
# Native flat programs on RV32 M-mode: the user-task ABI, the flat memory
# model and the embedded learner demo image are one profile.
CFLAGS += -DCONFIG_MCU_NATIVE_PROGRAM -DCONFIG_NATIVE_TASK_HANDLES_ONLY -DCONFIG_NATIVE_FLAT_THREAD
CFLAGS += -DMCU_DEMO_ELF_PATH=\"$(BUILD_DIR)/liteos-demo.elf\"
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
ifeq ($(SMP_SELFTEST),1)
ifeq ($(PROFILE),mcu)
$(error SMP_SELFTEST=1 is a hosted bringup diagnostic; use check-mcu-shared-kernel for the MCU profile)
endif
ifneq ($(BRINGUP),1)
$(error SMP_SELFTEST=1 requires BRINGUP=1)
endif
ifeq ($(NR_CPUS),1)
$(error SMP_SELFTEST=1 requires NR_CPUS greater than one)
endif
CFLAGS += -DCONFIG_SMP_SELFTEST
endif

ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif
ifeq ($(RAMFS_USER),1)
CFLAGS += -DCONFIG_RAMFS_USER
endif

ifeq ($(NOMMU),1)
CFLAGS += -DCONFIG_NOMMU
# Flat user ownership is a memory-model capability. Tiny firmware without
# userspace retains its original task layout and memory budget.
ifneq ($(filter-out mcu,$(PROFILE))$(filter 1,$(MCU_NATIVE_PROGRAM)),)
CONFIG_FLAT_USER_MEMORY := 1
CFLAGS += -DCONFIG_FLAT_USER_MEMORY
endif
endif

# Synthetic driver lifecycle test (disabled by default).
ifeq ($(CONFIG_DRIVER_LIFECYCLE_TEST),y)
CFLAGS += -DCONFIG_DRIVER_LIFECYCLE_TEST
endif

# Platform-bus IRQ resource self-check (smoke-platform-irq-fallback).  Empty at
# 0.  It registers synthetic platform devices on the real bus and claims a real
# IRQ line, so it is a gate build and not a shipping default.
ifeq ($(CONFIG_PLATFORM_IRQ_TEST),y)
CFLAGS += -DCONFIG_PLATFORM_IRQ_TEST
endif

# i440fx PIRQ write/readback self-check (smoke-pirq-i440fx).
ifeq ($(CONFIG_PCI_PIRQ_TEST),y)
CFLAGS += -DCONFIG_PCI_PIRQ_TEST
endif

# Deterministic regression hook for the channel-call deferred-wake fallback.
# Enabled only by smoke-native-ipc-wake-race.
ifeq ($(CONFIG_CHANNEL_DONATE_TEST),y)
CFLAGS += -DCONFIG_CHANNEL_DONATE_TEST
endif

# CONFIG_HDA_SMOKE_TEST / CONFIG_NVME_SMOKE_TEST deliberately add nothing to
# the kernel CFLAGS: the built-in HDA/NVMe drivers were migrated into the
# loadable packages, so these macros are reached only through DRVMOD_SMOKE in
# tools/mk/driver-modules.mk.  The names survive here solely for the BUILD_VARIANT
# components that isolate the smoke artifacts.

ifeq ($(CONFIG_SWAP),y)
CFLAGS += -DCONFIG_SWAP
endif

ifeq ($(CONFIG_XLATOR),y)
CFLAGS += -DCONFIG_XLATOR
endif

# No console to prompt on in either of these, and no reason to carry the
# reader: an MCU build replaces the source list, and a NOMMU target may be
# headless by construction.
ifneq ($(NOMMU),1)
ifneq ($(filter $(PROFILE),mcu),mcu)
ifeq ($(filter y,$(UART_CMDLINE)),y)
CFLAGS += -DCONFIG_UART_CMDLINE
endif
endif
endif

# RISC-V IOMMU probe: accept QEMU <= 10.0's unshifted TR_RESPONSE.PPN field.
# QEMU fixed this on master (set_field); set to 0 once the required QEMU
# baseline no longer needs the exception.
CONFIG_IOMMU_TRRESP_LEGACY_PPN ?= 1
ifeq ($(CONFIG_IOMMU_TRRESP_LEGACY_PPN),1)
CFLAGS += -DCONFIG_IOMMU_TRRESP_LEGACY_PPN
endif

BOARD_LDSCRIPT = $(KERNEL_DIR)/arch/$(ARCH)/boards/$(BOARD)/ldscript.ld
ifeq ($(wildcard $(BOARD_LDSCRIPT)),)
LDSCRIPT = $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld
else
LDSCRIPT = $(BOARD_LDSCRIPT)
endif

ifeq ($(MCU_SHARED_KERNEL),1)
LDSCRIPT = $(KERNEL_DIR)/arch/armv7m/boot/shared.ld
endif

LDFLAGS = -nostdlib -nostartfiles -Wl,--build-id=none -T $(LDSCRIPT) $(ARCH_LDFLAGS) $(LDFLAGS_NOMMU)
ifeq ($(ARCH),armv7m)
LDFLAGS += -Wl,--defsym=CLUSTER_NVRAM_LENGTH=$(if $(filter 1,$(STM32_QEMU)),0,$(if $(filter 16 32 64 128,$(STM32_FLASH_KB)),2,4))K \
           -Wl,--defsym=FLASH_LENGTH=$(STM32_FLASH_KB)K \
           -Wl,--defsym=RAM_LENGTH=$(STM32_RAM_KB)K
ifeq ($(MCU_SHARED_KERNEL),1)
LDFLAGS += -Wl,--defsym=SHARED_LENGTH=$(MCU_SHARED_RAM_KB)K
endif
endif

# ================================================================
# Kernel sources and build graph
# ================================================================

# Driver ownership is kept outside this top-level file so deployment policy is
# explicit and generic built-in exceptions remain reviewable.
include tools/mk/driver-sources.mk

# Shared ISA code is selected here, never by architecture tests in core C.
ifneq ($(filter riscv32 riscv64,$(ARCH)),)
ifneq ($(PROFILE),mcu)
CFLAGS += -DARCH_HAS_HYP_SBI
endif
endif
ARCH_SHARED_SRC_riscv64 := $(KERNEL_DIR)/arch/common/riscv/linux_syscalls.c \
                          $(KERNEL_DIR)/arch/common/riscv/hyp_sbi.c \
                          $(KERNEL_DIR)/arch/common/riscv/hyp_sbi_test.c \
                          $(KERNEL_DIR)/arch/common/riscv/kexec.c
ARCH_SHARED_SRC_riscv32 := $(if $(filter-out mcu,$(PROFILE)),$(ARCH_SHARED_SRC_riscv64))

# ABI-specific source directories
ifeq ($(ABI),both)
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/linux/*.c) \
           $(wildcard $(KERNEL_DIR)/abi/native/*.c)
else
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/$(ABI)/*.c)
endif

include $(KERNEL_DIR)/net/sources.mk

ifeq ($(PROFILE),mcu)
# The curated core of the MCU trim lives in components/trim.toml
# ([profile.mcu].sources); this is the only place a source enters the MCU
# build, so a profile change is one reviewed TOML edit.  The wildcard parts
# below stay here on purpose: board drivers, platform files and the arch are
# per-board facts the registry cannot enumerate.
CFLAGS += $(TRIM_PROFILE_MCU_OPT) $(TRIM_PROFILE_MCU_CPPFLAGS)
# The MCU trim links one syscall table: the profile the CMSIS manifest
# declares.  tools/cmsis_trim.py emits both the table subset and the feature
# header from user/cmsis_rtos2/cmsis_rtos2.toml, so kernel and library cannot
# disagree about which syscalls exist.
CFLAGS += -I$(BUILD_DIR)/generated/cmsis -DA20_NATIVE_SYSCALL_TABLE=\"syscall_table_mcu.def\"
KERNEL_SRC = $(TRIM_PROFILE_MCU_SOURCES) \
             $(wildcard $(KERNEL_DIR)/boot/*.c) $(KERNEL_DIR)/firmware/node.c \
             $(if $(filter 1,$(STM32_QEMU)),$(BOARD_DRIVER_DIR)/stm32_uart.c $(BOARD_DRIVER_DIR)/extsram.c,$(wildcard $(BOARD_DRIVER_DIR)/*.c)) \
             $(wildcard $(KERNEL_DIR)/arch/$(ARCH)/boards/$(BOARD)/*.c) \
             $(ARCH_KERNEL_C_SRC) $(ARCH_SHARED_SRC_$(ARCH))

ifeq ($(ARCH)/$(BOARD),riscv32/hi3861)
# The Hi3861 image carries the standard A20OS driver core.  The QEMU MCU
# regression stays on the minimal shell-only source set.
KERNEL_SRC += \
    kernel/drivers/core/driver_core.c \
    kernel/drivers/core/driver_class_mcu.c \
    kernel/drivers/bus/platform_bus.c \
    kernel/firmware/device.c \
    kernel/firmware/irq.c \
    kernel/drivers/gpio/hi3861_gpio.c \
    kernel/drivers/i2c/i2c-core.c \
    kernel/drivers/i2c/hi3861_i2c.c \
    kernel/drivers/spi/hi3861_spi.c \
    kernel/drivers/pwm/hi3861_pwm.c \
    kernel/drivers/adc/hi3861_adc.c
endif
else
# The CONFIG_XLATOR=n case drops kernel/proc/xlator.c from the source list
# outright rather than compiling it to an empty translation unit -- the same
# shape as the mm/nommu.c filter-out a few lines below.  CONFIG_SWAP instead
# leaves swap.o in the link as a ~1 KB object with no code in it; for a page
# cache that is not worth special-casing, but "cut the channel out" should
# mean the file is not built at all.
KERNEL_SRC = $(wildcard $(KERNEL_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/core/*.c) \
             $(wildcard $(KERNEL_DIR)/boot/*.c) $(wildcard $(KERNEL_DIR)/firmware/*.c) \
             $(wildcard $(KERNEL_DIR)/firmware/devicetree/*.c) \
             $(filter-out $(KERNEL_DIR)/firmware/acpi/uacpi_os.c,$(wildcard $(KERNEL_DIR)/firmware/acpi/*.c)) \
             $(wildcard $(KERNEL_DIR)/hyp/*.c) \
             $(filter-out $(KERNEL_DIR)/mm/nommu.c $(KERNEL_DIR)/mm/vmo_page_selftest.c,$(wildcard $(KERNEL_DIR)/mm/*.c)) \
             $(if $(filter-out y,$(CONFIG_XLATOR)),$(filter-out $(KERNEL_DIR)/proc/xlator.c,$(wildcard $(KERNEL_DIR)/proc/*.c)),$(wildcard $(KERNEL_DIR)/proc/*.c)) \
             $(filter-out $(KERNEL_DIR)/fs/rootfs_overlay.c,$(wildcard $(KERNEL_DIR)/fs/*.c)) \
             $(wildcard $(KERNEL_DIR)/fs/*/*.c) \
             $(wildcard $(KERNEL_DIR)/ipc/*.c) \
             $(A20_NET_SRC) \
             $(wildcard $(KERNEL_DIR)/cluster/*.c) \
             $(wildcard $(KERNEL_DIR)/bpf/*.c) \
             $(wildcard $(KERNEL_DIR)/ext/*.c) \
             $(wildcard $(KERNEL_DIR)/drvmod/*.c) \
             $(KERNEL_DIR)/arch/common/module_reloc.c $(KERNEL_DIR)/arch/common/elf_reloc.c \
             $(DRIVER_KERNEL_SRCS) \
             $(wildcard $(BOARD_DRIVER_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/arch/$(ARCH)/boards/$(BOARD)/*.c) \
             $(ABI_SRCS) \
             $(wildcard $(KERNEL_DIR)/syscall/*.c) \
             $(wildcard $(KERNEL_DIR)/shell/*.c) \
             $(ARCH_KERNEL_C_SRC) $(ARCH_SHARED_SRC_$(ARCH)) \
             $(LWIP_SRC) \
             $(LFS_SRC)

ifneq ($(filter $(ARCH),x86_64 aarch64),)
CFLAGS += -DCONFIG_UACPI -DCONFIG_ACPI -Ikernel/external/uacpi/include
KERNEL_SRC += $(KERNEL_DIR)/firmware/acpi/uacpi_os.c $(wildcard $(KERNEL_DIR)/external/uacpi/source/*.c)
endif

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

ifeq ($(VMO_PAGE_TEST),1)
KERNEL_SRC += $(KERNEL_DIR)/mm/vmo_page_selftest.c
endif

include $(KERNEL_DIR)/net/backends/lwip/sources.mk
include $(KERNEL_DIR)/external/littlefs/sources.mk
endif

ifeq ($(PROFILE),mcu)
else
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/ipc/envelope_acquire_race_test.c,$(KERNEL_SRC))
ifeq ($(ENVELOPE_ACQUIRE_RACE_TEST),1)
KERNEL_SRC += $(KERNEL_DIR)/ipc/envelope_acquire_race_test.c
endif
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/ipc/keyring_rust_bridge.c,$(KERNEL_SRC))
ifeq ($(KEYRING_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/ipc/keyring.c,$(KERNEL_SRC)),)
$(error KEYRING_IMPL=rust requires kernel/ipc/keyring.c in this profile)
endif
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/ipc/keyring.c,$(KERNEL_SRC))
KERNEL_SRC += $(KERNEL_DIR)/ipc/keyring_rust_bridge.c
endif
endif

ifneq ($(filter $(KERNEL_DIR)/net/control/route.c,$(KERNEL_SRC)),)
KERNEL_ROUTE_OBJECT = $(RUST_NET_ROUTE_OBJECT)
endif

include tools/mk/driver-modules.mk
ifeq ($(NET_STACK),bsd)
# Cluster SSI's opt-in selftests currently inspect lwIP memory statistics.
# Keep the desktop/BSD build independent instead of inventing a second stats ABI.
KERNEL_SRC := $(filter-out $(KERNEL_DIR)/cluster/selftest.c $(KERNEL_DIR)/cluster/udp_selftest.c,$(KERNEL_SRC))
endif

# Object files
LWIP_KERNEL_SRC := $(filter $(LWIP_SOURCE_ROOT)/src/%.c,$(KERNEL_SRC))
LFS_KERNEL_SRC := $(filter $(KERNEL_DIR)/external/littlefs/%.c,$(KERNEL_SRC))
# KERNEL_SRC is complete here, so source-bound Rust modules count as active
# runtime-test selectors for the profile actually being built.
ifeq ($(RUST_RUNTIME_TEST),1)
ifeq ($(filter rust, $(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_SEND_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(DONTUNMAP_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL) $(USER_COPY_IMPL) $(PROCESS_VM_IMPL) $(PARK_WAIT_IMPL) $(WAIT_TIMER_IMPL) $(WAIT_QUEUE_IMPL) $(RW_MUTEX_IMPL) $(MUTEX_STATE_IMPL) $(COMPLETION_IMPL) $(PID_IMPL) $(VMA_IMPL) $(VMO_PAGE_IMPL) $(VMO_LIFECYCLE_IMPL) $(VMO_PAGER_IMPL) $(DRIVER_REGISTRY_IMPL) $(VIRTIO_BLK_CORE_IMPL) $(ENVELOPE_POLICY_IMPL) $(FDTABLE_STATE_IMPL) $(EVENTPOLL_STATE_IMPL) $(EVENT_DESCRIPTOR_STATE_IMPL) $(SYSV_SEM_IMPL) $(LINUX_SCM_RIGHTS_IMPL) $(LINUX_MMSG_TXN_IMPL) $(NET_SOCKET_QUEUE_IMPL) $(EXT4_EXTENT_TXN_IMPL) $(KEYRING_IMPL) $(MM_SHUTDOWN_WRITE_IMPL) $(USERFAULTFD_STATE_IMPL) $(FUTEX_WAIT_IMPL) $(POSIX_MQ_IMPL) $(SYSV_MSG_IMPL) $(SYSV_SHM_IMPL) $(RUST_FS_STATE_ACTIVE) $(RUST_SIGNAL_CORE_ACTIVE)),)
$(error RUST_RUNTIME_TEST=1 requires at least one Rust kernel selector)
endif
endif
include tools/mk/rust-kernel.mk
ifeq ($(CLUSTER_SSI),1)
CFLAGS += -DCONFIG_CLUSTER_SSI
SSI_STATE_OBJECT := $(BUILD_DIR)/rust/ssi_state.o
.PHONY: FORCE_SSI_STATE_FLAGS
FORCE_SSI_STATE_FLAGS:
$(BUILD_DIR)/rust/.ssi-state-flags: FORCE_SSI_STATE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose) --edition=2021 --target x86_64-unknown-none panic=abort code-model=large no-redzone=yes opt=$(RUST_OPT_LEVEL)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi
$(SSI_STATE_OBJECT): kernel/cluster/ssi_state.rs $(BUILD_DIR)/rust/.ssi-state-flags
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-none -C panic=abort -C code-model=large -C no-redzone=yes -C opt-level=$(RUST_OPT_LEVEL) -o $@ $<
endif
.PHONY: check-cluster-ssi check-cluster-ssi-network
check-cluster-ssi:
	$(PYTHON) tools/cluster/check-cluster-ssi.py
check-cluster-ssi-network:
	$(PYTHON) tools/cluster/check-cluster-ssi-network.py
KERNEL_OBJ = $(KERNEL_ROUTE_OBJECT) $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter-out user/% $(LWIP_SOURCE_ROOT)/% $(KERNEL_DIR)/external/littlefs/%,$(KERNEL_SRC))) \
              $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(LWIP_KERNEL_SRC)) \
              $(patsubst $(KERNEL_DIR)/external/littlefs/%.c,$(BUILD_DIR)/external/littlefs/%.o,$(LFS_KERNEL_SRC))
ifeq ($(NET_STACK),bsd)
BSD_CORE_DIR = $(BUILD_DIR)/bsd-core
BSD_CORE_OBJECT = $(BSD_CORE_DIR)/bsd-core-isolated.o
.PHONY: FORCE_BSD_CORE
FORCE_BSD_CORE:
$(BSD_CORE_OBJECT): FORCE_BSD_CORE
	@rm -rf $(BSD_CORE_DIR)
	$(PYTHON) tools/probes/probe-bsd-core.py --cc '$(if $(filter llvm,$(TOOLCHAIN)),$(CLANG),$(BSD_CLANG))' --arch $(ARCH) --cross-prefix '$(CROSS_PREFIX_$(ARCH))' --nr-cpus $(NR_CPUS) --emulated-tls --net-profile $(NET_PROFILE) $(if $(filter router,$(NET_ROLE)),--router) --out $(BSD_CORE_DIR)
KERNEL_OBJ += $(BSD_CORE_OBJECT)
CFLAGS += -DCONFIG_NET_STACK_BSD=1
LDFLAGS += -Wl,-T,kernel/net/backends/bsd/sets.ld
endif
ifneq ($(strip $(BSD_BOOTSTRAP_OBJECT)),)
CFLAGS += -DCONFIG_BSD_BOOTSTRAP_TEST
KERNEL_SRC += kernel/net/backends/bsd/bootstrap_test.c kernel/net/backends/bsd/diagnostic_nic_host.c
KERNEL_OBJ += $(abspath $(BSD_BOOTSTRAP_OBJECT))
LDFLAGS += -Wl,-T,kernel/net/backends/bsd/sets.ld
endif
KERNEL_OBJ += $(RUST_LANDLOCK_OBJECT) $(RUST_SECCOMP_OBJECT)
KERNEL_OBJ += $(EARLY_DRIVER_BLOBS)
ifeq ($(NETFILTER_REWRITE_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/net/filter/netfilter_rewrite.c,$(KERNEL_SRC)),)
$(error NETFILTER_REWRITE_IMPL=rust requires kernel/net/filter/netfilter_rewrite.c in this profile)
endif
KERNEL_OBJ += $(RUST_NETFILTER_OBJECT)
endif
# The IPv4 filter is Rust and has no C fallback; the object is part of the
# kernel wherever netfilter.c is, and it brings the Rust runtime archive.
ifneq ($(filter $(KERNEL_DIR)/net/filter/netfilter.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the filter Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NETFILTER_FILTER_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The conntrack/NAT state machine is Rust and has no C fallback; the object is
# part of the kernel wherever the C file that calls into it is, and it brings the
# Rust runtime archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/filter/netfilter_nat.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the conntrack/NAT Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NETFILTER_NAT_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifeq ($(TASK_OBJECTS_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/proc/task_object.c,$(KERNEL_SRC)),)
$(error TASK_OBJECTS_IMPL=rust requires kernel/proc/task_object.c in this profile)
endif
KERNEL_OBJ += $(RUST_TASK_OBJECT)
endif
ifeq ($(CHANNEL_SEND_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/ipc/a20_channel.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_CHANNEL_SEND_OBJECT)
KERNEL_OBJ += $(RUST_EVENT_QUEUE_OBJECT)
endif
endif
ifeq ($(CHANNEL_RECV_TXN_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/abi/native/sys_native_ipc.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_CHANNEL_RECV_TXN_OBJECT)
endif
endif
# The dontunmap transaction is Rust on every MMU profile that builds
# mm/mremap.c.  The transaction itself is under #ifndef CONFIG_NOMMU there, so
# NOMMU neither compiles it nor links the object.
ifneq ($(filter $(KERNEL_DIR)/mm/mremap.c,$(KERNEL_SRC)),)
ifneq ($(NOMMU),1)
KERNEL_OBJ += $(RUST_DONTUNMAP_TXN_OBJECT)
endif
endif
ifeq ($(HANDLE_SLOT_TRANSITIONS_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/abi/native/handle_table.c,$(KERNEL_SRC)),)
$(error HANDLE_SLOT_TRANSITIONS_IMPL=rust requires kernel/abi/native/handle_table.c in this profile)
endif
KERNEL_OBJ += $(RUST_HANDLE_SLOT_OBJECT)
KERNEL_OBJ += $(RUST_HANDLE_TABLE_STATE_OBJECT)
endif
# The user copy primitives are Rust on every profile that builds mm/mm.c.
ifneq ($(filter $(KERNEL_DIR)/mm/mm.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_USERCOPY_OBJECT)
endif
ifeq ($(PROCESS_VM_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/mm/process_vm.c,$(KERNEL_SRC)),)
$(error PROCESS_VM_IMPL=rust requires kernel/mm/process_vm.c in this profile)
endif
KERNEL_OBJ += $(RUST_PROCESS_VM_OBJECT)
endif
ifeq ($(PARK_WAIT_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/proc/park.c,$(KERNEL_SRC)),)
$(error PARK_WAIT_IMPL=rust requires kernel/proc/park.c in this profile)
endif
KERNEL_OBJ += $(RUST_PARK_WAIT_OBJECT)
endif
ifeq ($(WAIT_QUEUE_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/core/sync.c,$(KERNEL_SRC)),)
$(error WAIT_QUEUE_IMPL=rust requires kernel/core/sync.c in this profile)
endif
KERNEL_OBJ += $(RUST_WAIT_QUEUE_STATE_OBJECT)
endif
ifeq ($(RW_MUTEX_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/core/sync.c,$(KERNEL_SRC)),)
$(error RW_MUTEX_IMPL=rust requires kernel/core/sync.c in this profile)
endif
KERNEL_OBJ += $(RUST_RW_MUTEX_STATE_OBJECT)
endif
ifeq ($(MUTEX_STATE_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/core/sync.c,$(KERNEL_SRC)),)
$(error MUTEX_STATE_IMPL=rust requires kernel/core/sync.c in this profile)
endif
KERNEL_OBJ += $(RUST_MUTEX_STATE_OBJECT)
endif
ifeq ($(COMPLETION_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/core/sync.c,$(KERNEL_SRC)),)
$(error COMPLETION_IMPL=rust requires kernel/core/sync.c in this profile)
endif
KERNEL_OBJ += $(RUST_COMPLETION_STATE_OBJECT)
endif
# The scheduler arms are Rust-only now; the objects follow sched.c.
ifneq ($(filter $(KERNEL_DIR)/proc/sched.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_TASK_PLACEMENT_OBJECT)
KERNEL_OBJ += $(RUST_EEVDF_MATH_OBJECT)
KERNEL_OBJ += $(RUST_RT_SCHED_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The translator configuration and its parsers are Rust-only now; the objects
# follow the C file that calls them.
ifneq ($(filter $(KERNEL_DIR)/proc/xlator.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_XLATOR_TMPL_OBJECT)
KERNEL_OBJ += $(RUST_XLATOR_CONFIG_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The task-list membership is Rust-only now; the object follows the C file
# that calls it.
ifneq ($(filter $(KERNEL_DIR)/proc/proc.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_TASKLIST_OBJECT)
KERNEL_OBJ += $(SSI_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The accounting reports are Rust-only now.
ifneq ($(filter $(KERNEL_DIR)/proc/proc.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_PROC_ACCOUNTING_OBJECT)
endif
# The ptrace stop/resume state machine is Rust-only now.
ifneq ($(filter $(KERNEL_DIR)/proc/debug.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_PTRACE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The core-dump layout is Rust-only now; the object follows the C file that
# calls it.
ifneq ($(filter $(KERNEL_DIR)/proc/coredump.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_COREDUMP_LAYOUT_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The checkpoint image validation and the Rust-owned register-frame mapping
# are used by the restore path and (through the rv64 trap_frame.h inlines)
# by every ptrace/coredump regset export/import; the object follows debug.c.
ifneq ($(filter $(KERNEL_DIR)/proc/debug.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_CKPT_IMAGE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The fork/clone transaction is Rust-only now; the object follows fork.c,
# which the MCU profile does not build.
ifneq ($(filter $(KERNEL_DIR)/proc/fork.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_CLONE_TXN_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
KERNEL_OBJ += $(RUST_ELF_CORE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
# cred.c, userns.c, timer_posix.c and resource_ns.c now always call into Rust:
# the C policies were deleted with their selectors, so these objects are linked
# wherever the C file they serve is built.  The Rust panic runtime follows them
# -- without it a checked arithmetic path in a Rust object would fail to link in
# the all-C-selector default profile.  Only the PID id space keeps a selector,
# because the MCU profile still uses the small C space.
ifneq ($(filter $(KERNEL_DIR)/proc/cred.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_CRED_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/userns.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_USERNS_POLICY_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/timer_posix.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_POSIX_TIMER_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/resource_ns.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_RESOURCE_NS_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/pidns.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_PIDNS_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifeq ($(PID_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/proc/pid.c,$(KERNEL_SRC)),)
$(error PID_IMPL=rust requires kernel/proc/pid.c in this profile)
endif
KERNEL_OBJ += $(RUST_PID_ALLOC_OBJECT)
endif
# The resource-limit policy is Rust-only now; the object follows the C file
# that calls it.
ifneq ($(filter $(KERNEL_DIR)/abi/linux/sys_proc.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_RLIMIT_POLICY_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The prctl/setns policy is Rust-only now.
ifneq ($(filter $(KERNEL_DIR)/abi/linux/sys_proc.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_SYS_POLICY_OBJECT)
endif
# The SIGALRM heap is Rust wherever the profile has the non-MCU capacity.  The
# MCU profile keeps the small C heap: the Rust array is sized for 8192 entries.
ifneq ($(PROFILE),mcu)
ifneq ($(filter $(KERNEL_DIR)/proc/timer_heap.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_ALARM_TIMER_OBJECT)
endif
endif
ifneq ($(filter $(KERNEL_DIR)/proc/timer_heap.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_WAIT_TIMER_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/wait.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_WAIT4_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The task lifecycle, construction and exit ordering are Rust-only now; the
# objects follow the C files that call them (exit.c is also built for MCU).
ifneq ($(filter $(KERNEL_DIR)/proc/task_lifecycle.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_TASK_LIFECYCLE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The shared-write admission state machine is Rust on every profile that builds
# mm/shutdown_write.c.
ifneq ($(filter $(KERNEL_DIR)/mm/shutdown_write.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MM_SHUTDOWN_WRITE_OBJECT)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/exit.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_EXIT_REPARENT_OBJECT)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/task_construction.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_TASK_CONSTRUCTION_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/proc/exit.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_TASK_EXIT_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The VMA interval tree and split/merge transaction are Rust on every profile
# that builds mm/vma.c.
ifneq ($(filter $(KERNEL_DIR)/mm/vma.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VMA_REGION_OBJECT)
endif
# Mapping creation is Rust on every profile that builds mm/mmap.c.  The MCU
# trim does not include it, and armv7m has no Rust kernel target, so the guard
# is on the source rather than on an architecture list.
ifneq ($(filter $(KERNEL_DIR)/mm/mmap.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MMAP_POLICY_OBJECT)
endif
# The swap slot lifecycle is Rust on every profile that builds mm/swap.c with
# swap support.  mm/swap.c compiles to nothing when CONFIG_SWAP=0, so its
# bridges would not exist and the object must not be linked either.
ifneq ($(filter $(KERNEL_DIR)/mm/swap.c,$(KERNEL_SRC)),)
ifneq ($(filter y,$(CONFIG_SWAP)),)
KERNEL_OBJ += $(RUST_SWAP_SLOTS_OBJECT)
endif
endif
# The OOM victim/reclaim policy is Rust on every profile that builds mm/oom.c.
ifneq ($(filter $(KERNEL_DIR)/mm/oom.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_OOM_POLICY_OBJECT)
endif
# The COW write-isolation transaction is Rust on every MMU profile that builds
# mm/fault.c; NOMMU has no COW path, so the object is not linked there.
ifneq ($(filter $(KERNEL_DIR)/mm/fault.c,$(KERNEL_SRC)),)
ifneq ($(NOMMU),1)
KERNEL_OBJ += $(RUST_COW_FAULT_OBJECT)
# The status-led COW break and fault engine need the per-PTE status sidecar;
# arm32's short-descriptor backend has none, and fault.c compiles their C
# counterparts out for that arch, so linking the objects would leave every one
# of their entry points undefined.  The plain COW fault path (cow_fault.rs)
# does work there and stays linked.
ifneq ($(ARCH),arm32)
KERNEL_OBJ += $(RUST_COW_STATUS_OBJECT)
KERNEL_OBJ += $(RUST_FAULT_STATUS_OBJECT)
endif
endif
endif
# The flat-space policy is Rust wherever the flat user memory model is compiled
# in, which is the NOMMU non-mcu profiles.  mm/flat_user.c is in KERNEL_SRC on
# every profile but compiles to nothing without that capability, so the object
# must follow the capability rather than the source list.
ifeq ($(CONFIG_FLAT_USER_MEMORY),1)
KERNEL_OBJ += $(RUST_FLAT_POLICY_OBJECT)
endif
# The fork/COW clone transaction is Rust on every MMU profile that builds
# mm/cow.c.  NOMMU has no page tables to clone: cow.c's C stubs are that
# profile's whole implementation, so the Rust object (and the bridges it needs)
# is not linked there.
ifneq ($(filter $(KERNEL_DIR)/mm/cow.c,$(KERNEL_SRC)),)
ifneq ($(NOMMU),1)
KERNEL_OBJ += $(RUST_COW_CLONE_OBJECT)
endif
endif
# The reservation engine is Rust on every profile that builds mm/vmar.c.
ifneq ($(filter $(KERNEL_DIR)/mm/vmar.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VMAR_POLICY_OBJECT)
endif
# The protection-change transaction is Rust on every profile that builds
# mm/mprotect.c.
ifneq ($(filter $(KERNEL_DIR)/mm/mprotect.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MPROTECT_TXN_OBJECT)
endif
# The advice policy and its two mapping transactions are Rust on every profile
# that builds mm/madvise.c, NOMMU included: that profile's bridges are stubs,
# so the object links wherever madvise.c does.
ifneq ($(filter $(KERNEL_DIR)/mm/madvise.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MADVISE_POLICY_OBJECT)
endif
# The sealing transaction is Rust on every profile that builds mm/mseal.c.
ifneq ($(filter $(KERNEL_DIR)/mm/mseal.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MSEAL_POLICY_OBJECT)
endif
# The NUMA policy state is Rust on every profile that builds mm/mempolicy.c.
ifneq ($(filter $(KERNEL_DIR)/mm/mempolicy.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MEMPOLICY_STATE_OBJECT)
endif
# The unmap transaction is Rust on every profile that builds mm/munmap.c; the
# MCU trim excludes kernel/mm entirely, so the guard is on the source.
ifneq ($(filter $(KERNEL_DIR)/mm/munmap.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_MUNMAP_TXN_OBJECT)
endif

# The layout-randomisation and W^X policies are Rust on every profile that
# builds their files; the MCU trim excludes both, so the guard is on the source.
ifneq ($(filter $(KERNEL_DIR)/mm/aslr.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_ASLR_POLICY_OBJECT)
endif
ifneq ($(filter $(KERNEL_DIR)/mm/wx.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_WX_POLICY_OBJECT)
endif
# The VMO page and lifecycle state machines are Rust on every profile that
# builds mm/vmo.c; the pager keeps its own selector, because its Rust source and
# its C half live in ipc/ (another module's files).
ifneq ($(filter $(KERNEL_DIR)/mm/vmo.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VMO_PAGE_OBJECT)
KERNEL_OBJ += $(RUST_VMO_LIFECYCLE_OBJECT)
endif
ifeq ($(VMO_PAGER_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/ipc/a20_pager.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VMO_PAGER_OBJECT)
endif
endif
ifeq ($(DRIVER_REGISTRY_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/drivers/core/driver_core.c,$(KERNEL_SRC)),)
$(error DRIVER_REGISTRY_IMPL=rust requires kernel/drivers/core/driver_core.c in this profile)
endif
KERNEL_OBJ += $(RUST_DRIVER_REGISTRY_OBJECT)
endif
ifeq ($(VIRTIO_BLK_CORE_IMPL),rust)
ifneq ($(filter kernel-only dev-build,$(MAKECMDGOALS)),)
ifeq ($(filter $(KERNEL_DIR)/drivers/block/virtio_blk.c,$(KERNEL_SRC)),)
$(error VIRTIO_BLK_CORE_IMPL=rust requires embedded driver deployment with virtio_blk.c)
endif
KERNEL_OBJ += $(RUST_VIRTIO_BLK_CORE_OBJECT)
endif
endif
ifeq ($(ENVELOPE_POLICY_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/ipc/envelope.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_ENVELOPE_POLICY_OBJECT)
endif
endif
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the Rust module has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/fs/fdtable.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_FDTABLE_STATE_OBJECT)
endif
ifeq ($(EVENTPOLL_STATE_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error EVENTPOLL_STATE_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/fs/eventpoll.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_EVENTPOLL_STATE_OBJECT)
endif
endif
ifeq ($(EVENT_DESCRIPTOR_STATE_IMPL),rust)
ifneq ($(filter $(KERNEL_DIR)/ipc/eventfd.c $(KERNEL_DIR)/ipc/timerfd.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error EVENT_DESCRIPTOR_STATE_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_EVENT_DESCRIPTOR_STATE_OBJECT)
endif
endif
ifeq ($(SYSV_SEM_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error SYSV_SEM_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/sysv_sem.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_SYSV_SEM_OBJECT)
endif
endif
ifeq ($(POSIX_MQ_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error POSIX_MQ_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/posix_mq.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_POSIX_MQ_OBJECT)
endif
endif
ifeq ($(SYSV_MSG_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error SYSV_MSG_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/sysv_msg.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_SYSV_MSG_OBJECT)
endif
endif
ifeq ($(SYSV_SHM_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error SYSV_SHM_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/sysv_shm.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_SYSV_SHM_OBJECT)
endif
endif
ifeq ($(USERFAULTFD_STATE_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error USERFAULTFD_STATE_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/userfaultfd.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_USERFAULTFD_STATE_OBJECT)
endif
endif
ifeq ($(FUTEX_WAIT_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error FUTEX_WAIT_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifneq ($(filter $(KERNEL_DIR)/ipc/futex.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_FUTEX_WAIT_OBJECT)
endif
endif
ifeq ($(LINUX_SCM_RIGHTS_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error LINUX_SCM_RIGHTS_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_LINUX_SCM_RIGHTS_OBJECT)
endif
ifeq ($(LINUX_MMSG_TXN_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error LINUX_MMSG_TXN_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_LINUX_MMSG_TXN_OBJECT)
endif
ifeq ($(NET_SOCKET_QUEUE_IMPL),rust)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error NET_SOCKET_QUEUE_IMPL=rust has no kernel Rust target for ARCH=$(ARCH))
endif
ifeq ($(filter $(KERNEL_DIR)/net/socket/socket_queue.c,$(KERNEL_SRC)),)
$(error NET_SOCKET_QUEUE_IMPL=rust requires kernel/net/socket/socket_queue.c in this profile)
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_QUEUE_OBJECT)
endif
# The poll readiness computation is Rust and has no C fallback; the object is
# part of the kernel wherever socket_control.c is.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_control.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket poll Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_POLL_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The accept transaction is Rust and has no C fallback; the object is part of the
# kernel wherever socket_control.c is, and it brings the Rust runtime archive.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_control.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket accept Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_ACCEPT_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The sendto transaction is Rust and has no C fallback; the object is part of
# the kernel wherever socket.c is, and it brings the Rust runtime archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket sendto Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_SENDTO_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The blocking receive transaction is Rust and has no C fallback; the object is
# part of the kernel wherever socket.c is, and it brings the Rust runtime archive
# with it.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket recv Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_RECV_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The INET option and buffer machinery is Rust and has no C fallback; the object
# is part of the kernel wherever socket_inet.c is, and it brings the Rust runtime
# archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet opt Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_OPT_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The INET socket lifecycle is Rust and has no C fallback; the object is part of
# the kernel wherever socket_inet.c is, and it brings the Rust runtime archive
# with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet life Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_LIFE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The TCP stream send transaction is Rust and has no C fallback; the object is
# part of the kernel wherever socket_inet.c is, and it brings the Rust runtime
# archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet send-tcp Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_SEND_TCP_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The INET bind transaction is Rust and has no C fallback; the object is part of
# the kernel wherever socket_inet.c is, and it brings the Rust runtime archive
# with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet bind Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_BIND_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The netif configuration and link-event machine is Rust and has no C fallback;
# the object is part of the kernel wherever lwip_stack.c is, and it brings the
# Rust runtime archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/lwip_stack.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the lwip-stack Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_LWIP_STACK_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# AF_NETLINK is Rust and has no C fallback; the object is part of the kernel
# wherever socket_netlink.c is, and it brings the Rust runtime archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/control/socket_netlink.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the netlink Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_NETLINK_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The UDP and raw send transactions are Rust and have no C fallback; the object
# is part of the kernel wherever socket_inet.c is, and it brings the Rust runtime
# archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet send Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_SEND_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The local connect pairing transaction is Rust and has no C fallback; the object
# is part of the kernel wherever socket_inet.c is, and it brings the Rust runtime
# archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet connect Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_CONNECT_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The bottom-half event ring and its consumer are Rust and have no C fallback;
# the object is part of the kernel wherever socket_inet.c is, and it brings the
# Rust runtime archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet bottom-half Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_BH_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The staged-accept state machine is Rust and has no C fallback; the object is
# part of the kernel wherever socket_inet.c is, and it brings the Rust runtime
# archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/backends/lwip/socket_inet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-inet Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_INET_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The AF_PACKET state machine is Rust and has no C fallback; the object is part
# of the kernel wherever socket_packet.c is, and it brings the Rust runtime
# archive with it.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_packet.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the packet Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_PACKET_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The AF_UNIX state machine is Rust and has no C fallback; the object is part of
# the kernel wherever socket_unix.c is, and it brings the Rust runtime archive.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_unix.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-unix Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_UNIX_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The socket-vfile lifecycle is Rust and has no C fallback; the object is part of
# the kernel wherever socket_file.c is, and it brings the Rust runtime archive.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_file.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket-file Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_FILE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The socket registry/reference-ledger and option state machines are Rust, and
# there is no C fallback left to select: each object is part of the kernel
# wherever the C file that calls into it is, and it brings the Rust runtime
# archive (core plus the panic handler) with it.  A profile that trims those
# files out (the MCU trim) reaches none of it.
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_registry.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket registry Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_REGISTRY_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/net/socket/socket_control.c,$(KERNEL_SRC)),)
ifeq ($(filter $(ARCH),x86_64 aarch64 loongarch64 riscv64 riscv32 arm32 armv7m),)
$(error the socket options Rust object has no target for ARCH=$(ARCH))
endif
KERNEL_OBJ += $(RUST_NET_SOCKET_OPTIONS_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifeq ($(EXT4_EXTENT_TXN_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/fs/diskfs/ext4.c,$(KERNEL_SRC)),)
$(error EXT4_EXTENT_TXN_IMPL=rust requires kernel/fs/diskfs/ext4.c in this profile)
endif
KERNEL_OBJ += $(RUST_EXT4_EXTENT_TXN_OBJECT)
endif
ifeq ($(KEYRING_IMPL),rust)
ifeq ($(filter $(KERNEL_DIR)/ipc/keyring_rust_bridge.c,$(KERNEL_SRC)),)
$(error KEYRING_IMPL=rust requires kernel/ipc/keyring_rust_bridge.c in this profile)
endif
KERNEL_OBJ += $(RUST_KEYRING_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifeq ($(filter $(KERNEL_DIR)/proc/signal.c,$(KERNEL_SRC)),)
$(error signal core requires kernel/proc/signal.c in this profile)
endif
KERNEL_OBJ += $(RUST_SIGNAL_CORE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
ifeq ($(RUST_VFS_LIFECYCLE_ACTIVE),rust)
KERNEL_OBJ += $(RUST_VFS_LIFECYCLE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# Block/page cache state engines exist only where their C adapters are in the
# profile; the MCU trim has no block cache and stays Rust-free for them.
ifneq ($(filter $(KERNEL_DIR)/fs/block_cache.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_BLOCK_CACHE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/page_cache.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_PAGE_CACHE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs/dcache.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VFS_DCACHE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# procfs cursor/offset state exists only where its C adapter is in the profile;
# the MCU trim has no procfs and stays Rust-free for it.
ifneq ($(filter $(KERNEL_DIR)/fs/procfs/procfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_PROCFS_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/procfs/procfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_PROCFS_NODE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs/path.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VFS_PATH_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs/path_resolution.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VFS_PATH_WALK_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VFS_MUTATE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs/mntns.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VFS_MNTNS_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs/mount_ops.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_VFS_MOUNT_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/vfs/file.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_FILE_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The identity index object exists only where its C adapter (uxfs.c) is part of
# the profile; the MCU trim excludes uxfs.c and needs no Rust target even though
# the selector defaults to c.
ifneq ($(filter $(KERNEL_DIR)/fs/uxfs/uxfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_UXFS_VNODE_IDENTITY_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# The RPC engine object lives beside the adapter it drives.
ifneq ($(filter $(KERNEL_DIR)/fs/uxfs/uxfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_UXFS_RPC_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter $(KERNEL_DIR)/fs/uxfs/uxfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_UXFS_OPS_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
# Lifetime/token negotiation is unconditionally Rust-owned; the state struct
# and all transitions live in kernel/fs/uxfs/lifetime_state.rs.
ifneq ($(filter $(KERNEL_DIR)/fs/uxfs/uxfs.c,$(KERNEL_SRC)),)
KERNEL_OBJ += $(RUST_UXFS_LIFETIME_STATE_OBJECT)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif
ifneq ($(filter rust, $(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_SEND_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(DONTUNMAP_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL) $(USER_COPY_IMPL) $(PROCESS_VM_IMPL) $(PARK_WAIT_IMPL) $(WAIT_TIMER_IMPL) $(WAIT_QUEUE_IMPL) $(RW_MUTEX_IMPL) $(MUTEX_STATE_IMPL) $(COMPLETION_IMPL) $(PID_IMPL) $(VMA_IMPL) $(VMO_PAGE_IMPL) $(VMO_LIFECYCLE_IMPL) $(VMO_PAGER_IMPL) $(DRIVER_REGISTRY_IMPL) $(VIRTIO_BLK_CORE_IMPL) $(ENVELOPE_POLICY_IMPL) $(FDTABLE_STATE_IMPL) $(EVENTPOLL_STATE_IMPL) $(EVENT_DESCRIPTOR_STATE_IMPL) $(SYSV_SEM_IMPL) $(LINUX_SCM_RIGHTS_IMPL) $(LINUX_MMSG_TXN_IMPL) $(NET_SOCKET_QUEUE_IMPL) $(EXT4_EXTENT_TXN_IMPL) $(KEYRING_IMPL) $(MM_SHUTDOWN_WRITE_IMPL) $(USERFAULTFD_STATE_IMPL) $(FUTEX_WAIT_IMPL) $(POSIX_MQ_IMPL) $(SYSV_MSG_IMPL) $(SYSV_SHM_IMPL) $(RUST_FS_STATE_ACTIVE) $(RUST_SIGNAL_CORE_ACTIVE)),)
endif
ifneq ($(filter rust,$(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL) $(PROCESS_VM_IMPL) $(PARK_WAIT_IMPL) $(WAIT_TIMER_IMPL) $(WAIT_QUEUE_IMPL) $(RW_MUTEX_IMPL) $(MUTEX_STATE_IMPL) $(COMPLETION_IMPL) $(TASK_LIFECYCLE_IMPL) $(TASK_EXIT_IMPL) $(WAIT4_IMPL) $(TASK_PLACEMENT_IMPL) $(EEVDF_MATH_IMPL) $(RT_SCHED_IMPL) $(TASK_CONSTRUCTION_IMPL) $(VMO_PAGER_IMPL) $(DRIVER_REGISTRY_IMPL) $(VIRTIO_BLK_CORE_IMPL) $(ENVELOPE_POLICY_IMPL) $(FDTABLE_STATE_IMPL) $(EVENTPOLL_STATE_IMPL) $(EVENT_DESCRIPTOR_STATE_IMPL) $(LINUX_SCM_RIGHTS_IMPL) $(LINUX_MMSG_TXN_IMPL) $(NET_SOCKET_QUEUE_IMPL) $(EXT4_EXTENT_TXN_IMPL) $(KEYRING_IMPL)),)
RUST_KERNEL_RUNTIME_LINK := $(RUST_KERNEL_RUNTIME_ARCHIVE)
endif

# Optional self-contained userspace for physical-board bring-up.  Each static
# ELF is linked as read-only kernel data, then copied into the root ramfs by
# kernel/fs/rootfs_user.c.  Keep this opt-in so normal disk-backed images do
# not grow and existing submission artifacts remain unchanged.
RAMFS_USER_PROGRAMS := init mksh help ls cat ps sleep timer_preempt timer_idle \
                      echo uname mkdir rm cp sync poweroff reboot nvme_stress
ifeq ($(RAMFS_FASTFETCH),1)
RAMFS_USER_PROGRAMS += fastfetch
CFLAGS += -DCONFIG_RAMFS_FASTFETCH
endif
ifeq ($(WIFI_RTW89),1)
RAMFS_USER_PROGRAMS += rtw89d wifi wifi-dhcp rtw89-notices wifi-gui wifi_backend.py wifi_portal.py a20-wifi.desktop
CFLAGS += -DCONFIG_WIFI_RTW89
endif
ifeq ($(RAMFS_DESKTOP_TOOLS),1)
RAMFS_USER_PROGRAMS += input-probe.py 90-libinput-fuzz-override.rules session-log.py session-power.py 90-a20-ibus.gschema.override
CFLAGS += -DCONFIG_RAMFS_DESKTOP_TOOLS
endif
ifeq ($(RAMFS_USER):$(NET_STACK):$(NET_ROLE),1:bsd:router)
RAMFS_USER_PROGRAMS += wanctl netctl pppoe pd-lan
CFLAGS += -DCONFIG_RAMFS_ROUTER
else ifeq ($(RAMFS_NET_TESTS),1)
RAMFS_USER_PROGRAMS += netctl pppoe pd-lan
endif
ifeq ($(RAMFS_NET_TESTS),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_NET_TESTS=1 requires RAMFS_USER=1)
endif
RAMFS_USER_PROGRAMS += tcp_loopback_test udp_loopback_test ipv6_loopback_test \
                      net_lane_hostfwd_test udp_hostfwd_test bsd_scale_test bsd_tun_router_test
CFLAGS += -DCONFIG_RAMFS_NET_TESTS
endif
ifeq ($(RAMFS_FPU_TESTS),1)
RAMFS_USER_PROGRAMS += fork_fpu_probe
CFLAGS += -DCONFIG_RAMFS_FPU_TESTS
endif
ifeq ($(RAMFS_MEDIA),1)
RAMFS_USER_PROGRAMS += videoplay audioplay
CFLAGS += -DCONFIG_RAMFS_MEDIA
endif
ifeq ($(RAMFS_DISTRO),1)
CFLAGS += -DCONFIG_RAMFS_DISTRO
endif
# ---- structured/persistent logging policy ---------------------------------
# Instances set these through [log]; the Makefile owns the defaults and the
# enum-to-number mapping.  Runtime a20.log.* cmdline arguments can override
# level/console/persistent/mode/max_bytes for one boot.
LOG_ENABLED      ?= 1
LOG_LEVEL        ?= info
LOG_CONSOLE      ?= 1
LOG_PERSISTENT   ?= 1
LOG_MODE         ?= auto
LOG_PARTITION_MB ?= 64
LOG_MAX_BYTES    ?= 8388608
LOG_SESSION      ?= 1
LOG_LOADER       ?= 1

ifeq ($(LOG_LEVEL),debug)
KLOG_LEVEL_NUM := 0
else ifeq ($(LOG_LEVEL),info)
KLOG_LEVEL_NUM := 1
else ifeq ($(LOG_LEVEL),warn)
KLOG_LEVEL_NUM := 2
else ifeq ($(LOG_LEVEL),err)
KLOG_LEVEL_NUM := 3
else
$(error LOG_LEVEL must be debug|info|warn|err, got '$(LOG_LEVEL)')
endif

ifeq ($(LOG_MODE),auto)
KLOG_MODE_NUM := 0
else ifeq ($(LOG_MODE),partition)
KLOG_MODE_NUM := 1
else ifeq ($(LOG_MODE),rootfs)
KLOG_MODE_NUM := 2
else ifeq ($(LOG_MODE),ram)
KLOG_MODE_NUM := 3
else ifeq ($(LOG_MODE),esp)
KLOG_MODE_NUM := 4
else
$(error LOG_MODE must be auto|partition|rootfs|ram|esp, got '$(LOG_MODE)')
endif

ifeq ($(LOG_ENABLED),0)
KLOG_SESSION_NUM := 0
KLOG_PERSIST_NUM := 0
else
KLOG_SESSION_NUM := $(LOG_SESSION)
KLOG_PERSIST_NUM := $(LOG_PERSISTENT)
endif

CFLAGS += -DCONFIG_KLOG_ENABLED=$(LOG_ENABLED)
CFLAGS += -DCONFIG_KLOG_DEFAULT_LEVEL=$(KLOG_LEVEL_NUM)
CFLAGS += -DCONFIG_KLOG_CONSOLE=$(LOG_CONSOLE)
CFLAGS += -DCONFIG_KLOG_MODE=$(KLOG_MODE_NUM)
CFLAGS += -DCONFIG_KLOG_MAX_BYTES=$(LOG_MAX_BYTES)ULL
CFLAGS += -DCONFIG_KLOG_SESSION=$(KLOG_SESSION_NUM)
# Every full (non-MCU) profile compiles the writer; MCU profiles keep the weak
# no-op stubs and their console-only log.
ifeq ($(MCU_PROFILE_SELECTED),)
CFLAGS += -DCONFIG_KLOG_PERSISTENT=$(KLOG_PERSIST_NUM)
endif
ifeq ($(STORAGE_READ_ONLY),1)
RAMFS_USER_PROGRAMS += storage_read_test
endif
ifeq ($(RAMFS_NATIVE_TESTS),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_NATIVE_TESTS requires RAMFS_USER=1)
endif
ifneq ($(ABI),both)
$(error RAMFS_NATIVE_TESTS requires ABI=both for the Linux shell and Native tests)
endif
RAMFS_USER_PROGRAMS += native_futex native_spawn native_input native_latency cluster_threads
CFLAGS += -DCONFIG_RAMFS_NATIVE_TESTS
endif
ifeq ($(RAMFS_RT_TESTS),1)
ifneq ($(RAMFS_USER),1)
$(error RAMFS_RT_TESTS requires RAMFS_USER=1)
endif
RAMFS_USER_PROGRAMS += sched_stress
CFLAGS += -DCONFIG_RAMFS_RT_TESTS
endif
RAMFS_USER_BLOB_DIR := $(BUILD_DIR)/rootfs-user
RAMFS_USER_BLOBS := $(addprefix $(RAMFS_USER_BLOB_DIR)/,$(addsuffix .o,$(RAMFS_USER_PROGRAMS)))
RAMFS_USER_OBJCOPY_loongarch64 := -O elf64-loongarch -B loongarch
RAMFS_USER_OBJCOPY_riscv64   := -O elf64-littleriscv -B riscv:rv64
RAMFS_USER_OBJCOPY_x86_64    := -O elf64-x86-64 -B i386:x86-64
RAMFS_USER_OBJCOPY_aarch64   := -O elf64-littleaarch64 -B aarch64
RAMFS_USER_OBJCOPY_arm32     := -O elf32-littlearm -B arm
ifeq ($(RAMFS_USER),1)
# Gate on the objcopy entry rather than on an arch list, so adding an
# architecture is one table line instead of two places that can disagree.  The
# previous check was an explicit $(error) naming loongarch64, which meant a
# small-memory board on any other architecture had no way to run a RAMFS
# userland at all -- the Milk-V Duo's 64 MiB is the case that needs it.
ifeq ($(RAMFS_USER_OBJCOPY_$(ARCH)),)
$(error RAMFS_USER=1 has no objcopy recipe for ARCH=$(ARCH); add RAMFS_USER_OBJCOPY_$(ARCH) near line 1013)
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
VDSO_CC               ?= $(if $(filter llvm,$(TOOLCHAIN)),$(LLVM_CC) $(LLVM_LINK),$(VDSO_CC_$(ARCH)))
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
ASM_SRC = $(ARCH_KERNEL_ASM_SRC) \
          $(wildcard $(KERNEL_DIR)/arch/$(ARCH)/boards/$(BOARD)/*.S)
ASM_OBJ = $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC))
DEP_FILES = $(filter-out $(RAMFS_USER_BLOBS:.o=.d),$(KERNEL_OBJ:.o=.d)) \
            $(ASM_OBJ:.o=.d)

# Kernel image
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
VBOX_AARCH64_EFI = $(BUILD_DIR)/BOOTAA64.EFI
VBOX_AARCH64_IMG = $(BUILD_DIR)/a20os-vbox-aarch64.img
VBOX_AARCH64_TEXT_IMG = $(BUILD_DIR)/a20os-vbox-aarch64-text.img
# The aarch64 UEFI loader places the flat image at a fixed address, so that
# address has to be the one the board's linker script actually linked it at.
# Restating it here let the two drift apart silently: the loader demanded
# 0x08080000 (VirtualBox, RAM at 0x08000000) while the qemu-virt board links at
# 0x40080000, so an AAVMF boot failed in allocate_pages before any kernel code
# ran.  Read it out of the script instead, and keep it overridable.
VBOX_AARCH64_LOAD_ADDRESS ?= $(shell sed -n 's/^PROVIDE(PHYS_BASE = \(0x[0-9a-fA-F]*\));.*/\1/p' $(LDSCRIPT) | head -1)
VBOX_X86_64_EFI = $(BUILD_DIR)/BOOTX64.EFI
VBOX_X86_64_IMG = $(BUILD_DIR)/a20os-vbox-x86_64-uefi.img

# ================================================================
# Targets (split into tools/targets-*.mk)
# ================================================================

include tools/mk/targets-base.mk
include tools/mk/targets-build.mk
include tools/mk/targets-gates.mk
include tools/mk/targets-smoke.mk
include tools/mk/targets-steps.mk
include tools/mk/targets-dev.mk
include tools/mk/stm32.mk
include tools/mk/m0sense.mk
include tools/mk/hi3861.mk
include tools/mk/pine-a64.mk
include tools/mk/licheerv-nano.mk
include tools/mk/quartz64-zero.mk
include tools/mk/lichee-pi-zero.mk
include tools/mk/ox64.mk
include tools/mk/raspberrypi4.mk
include tools/mk/execution.mk
include tools/mk/target-console.mk
include tools/mk/run-targets.mk
include tools/mk/targets-images.mk
include tools/mk/targets-rootfs.mk
include tools/mk/targets-pkg.mk
include tools/mk/targets-distro.mk
include tools/mk/targets-extra.mk
include tools/mk/targets-native.mk
include tools/mk/targets-native-smoke.mk
include tools/mk/rust-user.mk
include tools/mk/targets-mlibc.mk
include tools/mk/targets-xlator.mk
include tools/mk/targets-bench.mk

# ================================================================
# Documentation
# ================================================================
# Build standard-reference.pdf from every tracked Markdown file under docs/.
# The generator validates required tools and fonts and reports platform-specific
# installation commands when a dependency is missing.
.PHONY: docs check-docs
docs:
	@bash tools/docs/gen_docs_pdf.sh

check-docs:
	@python3 tools/docs/format_markdown.py --check docs README.md
	@python3 tools/docs/check_doc_citations.py
	@python3 tools/docs/check_doc_nav.py

# The binaries the native stamp rule requires.  make owns this list so the gate
# and the build cannot disagree about what "built" means.  This is exactly
# the set the shell rule tested; the Makefile defines more NATIVE_*_BIN vars
# for other purposes and those are deliberately not part of the gate.
# The stamp existence check must cover exactly what native-programs builds,
# otherwise binaries can go missing from the image while the stamp still passes.
NATIVE_BINS := $(NATIVE_OUTPUTS)

# LS2K0300 RAM bring-up (vendor firmware is preserved).
include tools/mk/ls2k0300.mk
include tools/mk/esp32c3.mk

ifeq ($(TOOLCHAIN),llvm)
include tools/mk/llvm-runtime.mk
$(KERNEL_NOSYMS_ELF) $(KERNEL_ELF) $(BUILD_DIR)/mcu-test-program.elf $(BUILD_DIR)/mcu-spawn-child.elf: $(LLVM_KERNEL_BUILTINS)
$(NATIVE_BINS): $(LLVM_USER_BUILTINS)
endif

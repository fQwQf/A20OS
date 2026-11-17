# ----------------------------------------------------------------
# Release build.  `make all` produces the dual-architecture release artifacts
# kernel-rv, kernel-la, disk.img, and disk-la.img in the repository root.
# ----------------------------------------------------------------
# Several targets launch recursive builds that share the default RISC-V image.
# GNU make's .NOTPARALLEL is not transitive, so mark the runtime aggregate whose
# sibling smokes can concurrently rebuild the same FAT image.
.NOTPARALLEL: check-upgrade-userland-smokes

all:
	$(MAKE) release-rv
	$(MAKE) release-la
	@echo "=== release build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

check-kernel-build: $(DEFAULT_KERNEL_CHECK_TARGETS)

check-kernel-build-all: $(foreach a,$(SUPPORTED_HOSTED_ARCHES),check-$(a)-bringup) check-visionfive2-build check-ls2k1000-build check-lao64-board-builds check-arm-board-builds check-x86_64-pc-build

# Physical-board build gates: keep the VisionFive 2 and LS2K1000 sources
# building on every commit.  The generic profile is the boot substrate; the
# embedded profile additionally compiles the board GMAC/SDIO drivers.
check-visionfive2-build:
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=$(ABI) BRINGUP=1 kernel-only
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only

# Build gates for the boards added by the portable-bringup work.  Their docs and
# board.c headers claim "build-verified", and until a target ran them that claim
# was a statement about one afternoon rather than a property of the tree: a new
# board could stop compiling and no gate would notice.  These keep it true.
#
# They are build-only on purpose.  None of these boards has been powered up, so
# a gate asserting a boot would be asserting something unverified.
.PHONY: check-lao64-board-builds check-arm-board-builds check-x86_64-pc-build
check-lao64-board-builds:
	$(MAKE) ARCH=riscv64 BOARD=licheerv-nano ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only
	$(MAKE) ARCH=riscv64 BOARD=milk-v-duo ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded NOMMU=1 RAMFS_USER=1 CONFIG_SWAP=0 kernel-only

check-arm-board-builds:
	$(MAKE) ARCH=aarch64 BOARD=rk3328 ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only
	$(MAKE) ARCH=aarch64 BOARD=sun50i-h616 ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only

# x86_64-pc was the fifth board whose doc claims "build-verified" while no
# target rebuilt it, which is exactly the drift the comment above says these
# gates exist to prevent.  Build-only for the same reason: no physical PC has
# run this, so a gate asserting a boot would assert something unverified.
# The GRUB rescue ISO it packages is covered separately by the x86_64-pc
# packaging target, not here.
check-x86_64-pc-build:
	$(MAKE) ARCH=x86_64 BOARD=x86_64-pc ABI=$(ABI) BRINGUP=1 kernel-only

# VisionFive 2 on-hardware boot artifacts (see docs/zh/platforms/visionfive2-boot.md).
# vf2-firmware builds OpenSBI + U-Boot SPL from pinned upstream sources;
# vf2-image wraps the kernel and FAT32 userspace as a direct-boot FIT
# (BootROM -> SPL -> OpenSBI -> A20OS) plus a raw SD-card image.
.PHONY: vf2-firmware vf2-extra-sources vf2-image vf2-world-image
# Ask the Makefile for the directory this configuration uses rather than
# spelling it out: a fully integrated Rust profile makes BUILD_VARIANT long
# enough that the Makefile hashes it (see BUILD_VARIANT_FULL there), so a
# hardcoded descriptive path silently names a directory no build ever creates.
VF2_IMAGE_BUILD_DIR = $(shell $(MAKE) --no-print-directory ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=0 NOMMU=1 DRIVER_DEPLOYMENT=embedded KERNEL_WERROR=0 LDSCRIPT=kernel/arch/riscv64/boards/visionfive2/ldscript-nommu.ld print-build-dir)
# The board's /extra partition is filled by an apk world image rather than by
# compiling software out of user/external/.  mkrootfs.py already emits a
# partitionless raw ext4, which is exactly what make-boot-image.sh expects for
# that slot, and the kernel mounts it the same way.  PKG_ALPINE stays on because
# the whole point is to get the upstream packages.  PKG_IMAGE_DIR is set in
# tools/mk/targets-pkg.mk, which is included later; recursive expansion defers the
# lookup until the recipe runs.
VF2_WORLD ?= devel
VF2_WORLD_IMAGE = $(PKG_IMAGE_DIR)/$(VF2_WORLD)-riscv64.img
vf2-firmware:
	tools/vf2/build-firmware.sh

# Only fastfetch remains a gitlink: it is the one program compiled into the
# FAT32 root, which no Alpine world replaces.
vf2-extra-sources:
	tools/vf2/fetch-extra-sources.sh

vf2-world-image:
	$(MAKE) ARCH=riscv64 PKG_WORLD=$(VF2_WORLD) PKG_ALPINE=1 image-world

vf2-image: vf2-extra-sources vf2-world-image
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=0 NOMMU=1 \
		DRIVER_DEPLOYMENT=embedded \
		KERNEL_WERROR=0 \
		LDSCRIPT=kernel/arch/riscv64/boards/visionfive2/ldscript-nommu.ld \
		$(VF2_IMAGE_BUILD_DIR)/fat32.img kernel-only
	@if [ -f user/external/apps/fastfetch/src/fastfetch.c ]; then \
		$(MAKE) -C user ARCH=riscv64 NOMMU=1 OPT="-O3" PROFILE=full \
			BUILD_DIR=build/riscv64-nommu fastfetch; \
	fi
	tools/vf2/make-boot-image.sh \
		$(VF2_IMAGE_BUILD_DIR)/kernel.bin \
		$(VF2_IMAGE_BUILD_DIR)/fat32.img \
		$(VF2_WORLD_IMAGE)

# VisionFive 2 SD-card images (two quick-start variants, development profile).
#
#   `make vf2-sdcard`   kernel + FAT32 userspace + an apk world image as the
#                       extra ext4 partition (mounted at /extra).
#   `make vf2-minimal`  kernel + FAT32 userspace (+ fastfetch), NO extra
#                       partition: the smallest bootable card.
#
# `make vf2-extra` is gone: it existed only to differ by carrying a
# source-built extra.img, and that build path has been replaced by worlds.
# See the deprecation stub in tools/mk/targets-extra.mk.
#
# Both use the regular MMU dev kernel + userspace, the same profile as
# `make run-riscv64`.  Build with VF2_MMU=0 to swap in the validated NOMMU
# bring-up kernel instead.  Neither involves any vendored source build;
# fastfetch is the only gitlink and vf2-minimal is the only variant that
# needs it.
#
# Prerequisites:
#   * build/vf2-firmware/ has been produced once by `make vf2-firmware`
#   * vf2-sdcard: the world image needs the Alpine mirror (VF2_WORLD selects
#     it, default 'devel').  Drop an sdcard-rv.img in the repo root, or set
#     VF2_EXTRA_IMAGE=, to use a prebuilt partition instead.
#
# Output: build/vf2-firmware/a20os-sd.img (each invocation rewrites it).
.PHONY: vf2-sdcard vf2-minimal \
	_vf2_check_firmware _vf2_build_base _vf2_build_fastfetch
VF2_MMU ?= 1
ifeq ($(VF2_MMU),1)
VF2_ABI             := both
VF2_NOMMU           := 0
VF2_LDSCRIPT        :=
VF2_USER_BUILD_DIR  := user/build/riscv64
# The MMU (dev) kernel compiles warning-clean; keep -Werror as-is.
VF2_KERNEL_WERROR   :=
else
VF2_ABI             := linux
VF2_NOMMU           := 1
VF2_LDSCRIPT        := LDSCRIPT=kernel/arch/riscv64/boards/visionfive2/ldscript-nommu.ld
VF2_USER_BUILD_DIR  := user/build/riscv64-nommu
# NOMMU leaves MMU-only code (elf seg_start, pte_to_vm_flags, aslr) unused;
# the validated VF2 build already relaxes -Werror for the same reason.
VF2_KERNEL_WERROR   := KERNEL_WERROR=0
endif
# Same reason as VF2_IMAGE_BUILD_DIR above: the recipes below hand this path to a
# sub-make as a goal, so it has to be the directory that sub-make actually uses.
VF2_BUILD_DIR = $(shell $(MAKE) --no-print-directory ARCH=riscv64 BOARD=visionfive2 ABI=$(VF2_ABI) BRINGUP=0 NOMMU=$(VF2_NOMMU) DRIVER_DEPLOYMENT=embedded $(VF2_LDSCRIPT) $(VF2_KERNEL_WERROR) print-build-dir)
# Prefer a caller-supplied partition image (a prebuilt sdcard-rv.img dropped in
# the repo root, or an explicit VF2_EXTRA_IMAGE=).  Otherwise assemble the apk
# world for the slot.  A command-line VF2_EXTRA_IMAGE means the caller brought
# their own image, so the world must not be built -- note that $(origin) is
# "file" for the ?= below and only "command line" when the user passed one.
VF2_EXTRA_IMAGE ?= $(or $(wildcard sdcard-rv.img),$(VF2_WORLD_IMAGE))
VF2_SDCARD_EXTRA_PREREQ := $(if $(filter command line,$(origin VF2_EXTRA_IMAGE)),,vf2-world-image)

_vf2_check_firmware:
	@test -f build/vf2-firmware/u-boot-spl.bin.normal.out || \
	    { echo "error: boot firmware missing in build/vf2-firmware/; run 'make vf2-firmware' once" >&2; exit 1; }
	@test -f build/vf2-firmware/fw_dynamic.bin || \
	    { echo "error: OpenSBI firmware missing in build/vf2-firmware/; run 'make vf2-firmware' once" >&2; exit 1; }

_vf2_build_base:
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=$(VF2_ABI) BRINGUP=0 \
		NOMMU=$(VF2_NOMMU) DRIVER_DEPLOYMENT=embedded \
		$(VF2_LDSCRIPT) $(VF2_KERNEL_WERROR) \
		$(VF2_BUILD_DIR)/fat32.img kernel-only

_vf2_build_fastfetch: _vf2_build_base
	@test -f user/external/apps/fastfetch/src/fastfetch.c || \
	    { echo "error: fastfetch source missing; run 'make vf2-extra-sources' or restore user/external/apps/fastfetch" >&2; exit 1; }
	$(MAKE) -C user ARCH=riscv64 NOMMU=$(VF2_NOMMU) OPT="-O3" PROFILE=full \
		BUILD_DIR=$(VF2_USER_BUILD_DIR) fastfetch

vf2-sdcard: _vf2_check_firmware _vf2_build_base $(VF2_SDCARD_EXTRA_PREREQ)
	@test -n "$(VF2_EXTRA_IMAGE)" || \
	    { echo "error: no extra image; place sdcard-rv.img in the repo root or set VF2_EXTRA_IMAGE=/path/to.img" >&2; exit 1; }
	@test -f "$(VF2_EXTRA_IMAGE)" || \
	    { echo "error: extra image not found: $(VF2_EXTRA_IMAGE)" >&2; exit 1; }
	tools/vf2/make-boot-image.sh \
		$(VF2_BUILD_DIR)/kernel.bin \
		$(VF2_BUILD_DIR)/fat32.img \
		$(VF2_EXTRA_IMAGE)

# Minimal bootable card: FAT32 rootfs carries the extra fastfetch binary.
vf2-minimal: _vf2_check_firmware _vf2_build_base _vf2_build_fastfetch
	@$(PYTHON) tools/img.py vf2-minimal \
		--src "$(VF2_BUILD_DIR)/fat32.img" \
		--dst "$(VF2_BUILD_DIR)/fat32-ff.img" \
		--payload "$(VF2_USER_BUILD_DIR)/fastfetch"
	tools/vf2/make-boot-image.sh \
		$(VF2_BUILD_DIR)/kernel.bin \
		$(VF2_BUILD_DIR)/fat32-ff.img

# `vf2-extra` used to live here.  It is now a deprecation stub in
# tools/mk/targets-extra.mk: the full card was defined by its source-built
# extra.img, and vf2-sdcard fills that slot with an apk world instead
# (VF2_WORLD, default 'devel').

check-ls2k1000-build:
	$(MAKE) ARCH=loongarch64 BOARD=ls2k1000 ABI=$(ABI) BRINGUP=1 kernel-only
	$(MAKE) ARCH=loongarch64 BOARD=ls2k1000 ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only

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

check-user-build: $(DEFAULT_USER_CHECK_TARGETS)

check-user-build-all: $(foreach a,$(SUPPORTED_HOSTED_ARCHES),check-$(a)-user)

check-build-matrix: check-kernel-build check-user-build
	@echo "check-build-matrix: PASS"

check-build-matrix-all: check-kernel-build-all check-user-build-all
	@echo "check-build-matrix-all: PASS"

# EXTERNAL_USERLAND_UPGRADE_CHECKLIST: run every gate group that must pass
# before an upgraded musl/sbase/mksh (or other imported userland) is
# accepted -- see docs/zh/dev/external-dependencies.md.
.PHONY: check-upgrade-userland-smokes
check-upgrade-userland-smokes: smoke-abi-linux smoke-mlibc smoke-mlibc-sbase smoke-mlibc-mksh

.PHONY: check-boot-report
check-boot-report:
	$(PYTHON) -m unittest tools.tests.test_boot_report tools.tests.test_a20log
	EVIDENCE_DIR="$(CURDIR)/.kernel-build/smoke/boot-report-procfs" \
	    RUST_TEST_TARGETS=riscv64imac-unknown-none-elf BOOT_REPORT_SUPPORT=1 sh tools/host-tests/test-procfs-node-state-host.sh
	$(PYTHON) tools/qemu/check_boot_report.py --x86

# The kernel bring-up targets may also build per-arch user driver blobs.  Route
# matrix user builds through USER_BUILD_STAMP so both paths take stamps.py's
# lock for the shared user/build/<arch> root before touching it.
.PHONY: check-user-build-stamp
check-user-build-stamp: $(USER_BUILD_STAMP)

check-riscv64-user:
	$(MAKE) ARCH=riscv64 USER_OPT="$(USER_OPT)" check-user-build-stamp

check-loongarch64-user:
	$(MAKE) ARCH=loongarch64 USER_OPT="$(USER_OPT)" check-user-build-stamp

check-aarch64-user:
	$(MAKE) ARCH=aarch64 USER_OPT="$(USER_OPT)" check-user-build-stamp

check-x86_64-user:
	$(MAKE) ARCH=x86_64 USER_OPT="$(USER_OPT)" check-user-build-stamp

check-arm32-user:
	$(MAKE) ARCH=arm32 USER_OPT="$(USER_OPT)" check-user-build-stamp

check-riscv32-user:
	$(MAKE) ARCH=riscv32 USER_OPT="$(USER_OPT)" check-user-build-stamp

check-dev-build:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=0 dev-build

check-release-build:
	$(MAKE) all


# End-to-end observability gate. These paths isolate images from daily runs.
OBSERVABILITY_GATE_DIR ?= .kernel-build/smoke/observability-gate
.PHONY: check-observability-host check-observability
check-observability-host:
	$(PYTHON) -m unittest tools.tests.test_perf_windows tools.tests.test_diagnostics tools.tests.test_lock_metrics tools.tests.test_observe tools.tests.test_procfs_diag_snapshot tools.tests.test_boot_report
	EVIDENCE_DIR=$(OBSERVABILITY_GATE_DIR)/procfs DIAGNOSTICS_SUPPORT=1 BOOT_REPORT_SUPPORT=1 RUST_TEST_TARGETS=riscv64imac-unknown-none-elf sh tools/host-tests/test-procfs-node-state-host.sh

check-observability: check-observability-host
	$(MAKE) ARCH=riscv64 BUILD_DIR=$(OBSERVABILITY_GATE_DIR)/dev RAMFS_USER=1 LOG_BOOT_REPORT=1 DIAG_BOOT=1 DIAG_TRACE=1 DIAG_TRACE_BOOT=1 DIAG_TRACE_MASK=3 kernel-only
	$(PYTHON) tools/qemu/check_observability_elf.py --mode enabled --kernel $(OBSERVABILITY_GATE_DIR)/dev/kernel.elf
	$(PYTHON) tools/qemu/check_observability.py --arch riscv64 --diagnostics --kernel $(OBSERVABILITY_GATE_DIR)/dev/kernel.elf --output $(OBSERVABILITY_GATE_DIR)/riscv64-guest
	$(MAKE) ARCH=x86_64 BUILD_DIR=$(OBSERVABILITY_GATE_DIR)/x86 RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded LOG_BOOT_REPORT=1 DIAG_BOOT=1 DIAG_TRACE=1 DIAG_TRACE_BOOT=1 DIAG_TRACE_MASK=3 kernel-only
	$(PYTHON) tools/qemu/check_observability_elf.py --mode enabled --kernel $(OBSERVABILITY_GATE_DIR)/x86/kernel.elf
	$(PYTHON) tools/qemu/check_observability.py --arch x86_64 --diagnostics --kernel $(OBSERVABILITY_GATE_DIR)/x86/kernel.elf --output $(OBSERVABILITY_GATE_DIR)/x86_64-guest
	$(MAKE) ARCH=riscv64 BUILD_DIR=$(OBSERVABILITY_GATE_DIR)/cut RAMFS_USER=1 LOG_ENABLED=0 LOG_PERSISTENT=0 LOG_BOOT_REPORT=0 METRICS_ENABLED=0 DIAG_BOOT=0 DIAG_TRACE=0 kernel-only
	$(PYTHON) tools/qemu/check_observability_elf.py --mode off --kernel $(OBSERVABILITY_GATE_DIR)/cut/kernel.elf
	$(PYTHON) tools/qemu/check_observability.py --arch riscv64 --cut --kernel $(OBSERVABILITY_GATE_DIR)/cut/kernel.elf --output $(OBSERVABILITY_GATE_DIR)/off-guest

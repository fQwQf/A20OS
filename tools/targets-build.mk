# ----------------------------------------------------------------
# Release build.  `make all` produces the dual-architecture release artifacts
# kernel-rv, kernel-la, disk.img, and disk-la.img in the repository root.
# ----------------------------------------------------------------
.PHONY: check-smp-platform-boundary check-io-progress-model

# Several gates launch recursive builds that share the default RISC-V image.
# Serialize the aggregate so `make -j check-doc-test-gates` cannot rebuild the
# same FAT image concurrently from independent prerequisite branches.
.NOTPARALLEL: check-doc-test-gates check-final-definition

all:
	$(MAKE) release-rv
	$(MAKE) release-la
	@echo "=== release build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

check-kernel-build: $(DEFAULT_KERNEL_CHECK_TARGETS)

check-kernel-build-all: $(foreach a,$(SUPPORTED_HOSTED_ARCHES),check-$(a)-bringup) check-loongarch32-bringup check-visionfive2-build check-ls2k1000-build

# Physical-board build gates: keep the VisionFive 2 and LS2K1000 sources
# building on every commit.  The generic profile is the boot substrate; the
# embedded profile additionally compiles the board GMAC/SDIO drivers.
check-visionfive2-build:
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=$(ABI) BRINGUP=1 kernel-only
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only

# VisionFive 2 on-hardware boot artifacts (see docs/platforms/visionfive2-boot.md).
# vf2-firmware builds OpenSBI + U-Boot SPL from pinned upstream sources;
# vf2-image wraps the kernel and FAT32 userspace as a direct-boot FIT
# (BootROM -> SPL -> OpenSBI -> A20OS) plus a raw SD-card image.
.PHONY: vf2-firmware vf2-extra-sources vf2-image
VF2_IMAGE_BUILD_DIR := .kernel-build/riscv64-visionfive2-linux-dev-embedded-nommu
vf2-firmware:
	tools/vf2/build-firmware.sh

vf2-extra-sources:
	tools/vf2/fetch-extra-sources.sh

vf2-image: vf2-extra-sources
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=0 NOMMU=1 \
		DRIVER_DEPLOYMENT=embedded \
		KERNEL_WERROR=0 \
		LDSCRIPT=kernel/platform/visionfive2/ldscript-nommu.ld \
		$(VF2_IMAGE_BUILD_DIR)/fat32.img kernel-only
	@if [ -f user/external/apps/fastfetch/src/fastfetch.c ]; then \
		$(MAKE) -C user ARCH=riscv64 NOMMU=1 OPT="-O3" PROFILE=full \
			BUILD_DIR=build/riscv64-nommu fastfetch; \
	fi
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=0 NOMMU=1 \
		DRIVER_DEPLOYMENT=embedded \
		KERNEL_WERROR=0 \
		LDSCRIPT=kernel/platform/visionfive2/ldscript-nommu.ld \
		EXTRA_PACKAGES="$(EXTRA_PACKAGES)" extra-img
	tools/vf2/make-boot-image.sh \
		$(VF2_IMAGE_BUILD_DIR)/kernel.bin \
		$(VF2_IMAGE_BUILD_DIR)/fat32.img \
		$(VF2_IMAGE_BUILD_DIR)/extra.img

# VisionFive 2 SD-card images (three quick-start variants, development profile).
#
#   `make vf2-sdcard`   kernel + FAT32 userspace + repo's sdcard-rv.img as the
#                       extra ext4 partition (mounted at /extra).
#   `make vf2-minimal`  kernel + FAT32 userspace (+ fastfetch), NO extra
#                       partition: the smallest bootable card.
#   `make vf2-extra`    kernel + FAT32 userspace + full extra.img built from
#                       sources (vim / git / gcc / rust ... -> /extra).
#
# All three use the regular MMU dev kernel + userspace, the same profile as
# `make run-riscv64`.  Build with VF2_MMU=0 to swap in the validated NOMMU
# bring-up kernel instead.  No extra-package sources or builds are involved in
# vf2-sdcard / vf2-minimal; vf2-extra needs `make vf2-extra-sources` and the
# EXTRA_PACKAGES build (see tools/targets-extra.mk).
#
# Prerequisites:
#   * build/vf2-firmware/ has been produced once by `make vf2-firmware`
#   * vf2-sdcard: sdcard-rv.img in the repo root (override VF2_EXTRA_IMAGE=)
#   * vf2-extra:  EXTRA_PACKAGES (default "vim git gcc") sources available
#
# Output: build/vf2-firmware/a20os-sd.img (each invocation rewrites it).
.PHONY: vf2-sdcard vf2-minimal vf2-extra \
	_vf2_check_firmware _vf2_build_base _vf2_build_fastfetch
VF2_MMU ?= 1
ifeq ($(VF2_MMU),1)
VF2_ABI             := both
VF2_NOMMU           := 0
VF2_BUILD_DIR       := .kernel-build/riscv64-visionfive2-both-dev-embedded
VF2_LDSCRIPT        :=
VF2_USER_BUILD_DIR  := user/build/riscv64
# The MMU (dev) kernel compiles warning-clean; keep -Werror as-is.
VF2_KERNEL_WERROR   :=
else
VF2_ABI             := linux
VF2_NOMMU           := 1
VF2_BUILD_DIR       := .kernel-build/riscv64-visionfive2-linux-dev-embedded-nommu
VF2_LDSCRIPT        := LDSCRIPT=kernel/platform/visionfive2/ldscript-nommu.ld
VF2_USER_BUILD_DIR  := user/build/riscv64-nommu
# NOMMU leaves MMU-only code (elf seg_start, pte_to_vm_flags, aslr) unused;
# the validated VF2 build already relaxes -Werror for the same reason.
VF2_KERNEL_WERROR   := KERNEL_WERROR=0
endif
# Prefer a user-supplied legacy image; otherwise build the normal extra image
# so `make vf2-sdcard` is useful in a fresh checkout.
VF2_EXTRA_IMAGE ?= $(or $(wildcard sdcard-rv.img),$(EXTRA_IMG))
VF2_SDCARD_EXTRA_PREREQ := $(if $(filter file,$(origin VF2_EXTRA_IMAGE)),extra-img)

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

# Full card: build the extra-package ext4 image (vim/git/gcc/... plus the
# user tools and fastfetch already staged in $(VF2_USER_BUILD_DIR)).
vf2-extra: _vf2_check_firmware _vf2_build_base _vf2_build_fastfetch
	$(MAKE) ARCH=riscv64 BOARD=visionfive2 ABI=$(VF2_ABI) BRINGUP=0 \
		NOMMU=$(VF2_NOMMU) DRIVER_DEPLOYMENT=embedded \
		$(VF2_LDSCRIPT) \
		EXTRA_PACKAGES="$(EXTRA_PACKAGES)" EXTRA_IMAGE_MB=$(EXTRA_IMAGE_MB) extra-img
	tools/vf2/make-boot-image.sh \
		$(VF2_BUILD_DIR)/kernel.bin \
		$(VF2_BUILD_DIR)/fat32.img \
		$(VF2_BUILD_DIR)/extra.img

check-ls2k1000-build:
	$(MAKE) ARCH=loongarch64 BOARD=ls2k1000 ABI=$(ABI) BRINGUP=1 kernel-only
	$(MAKE) ARCH=loongarch64 BOARD=ls2k1000 ABI=$(ABI) BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only

check-riscv64-bringup:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only

check-loongarch64-bringup:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-loongarch32-bringup:
	$(MAKE) ARCH=loongarch32 BOARD=nailoong ABI=$(ABI) BRINGUP=1 kernel-only

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

check-user-build-all: $(foreach a,$(SUPPORTED_HOSTED_ARCHES),check-$(a)-user)

check-build-matrix: check-kernel-build check-user-build
	@$(PYTHON) tools/gates.py check-build-matrix
	@echo "check-build-matrix: PASS"

check-build-matrix-all: check-kernel-build-all check-user-build-all
	@$(PYTHON) tools/gates.py check-build-matrix-all
	@echo "check-build-matrix-all: PASS"

check-arch-boundary: smoke-arch-mmu-matrix
	@$(PYTHON) tools/gates.py check-arch-boundary
	@echo "check-arch-boundary: PASS"

check-task-state-boundary:
	@$(PYTHON) tools/gates.py check-task-state-boundary
	@echo "check-task-state-boundary: PASS"

check-smp-platform-boundary:
	@$(PYTHON) tools/gates.py smp-platform-boundary
	@echo "check-smp-platform-boundary: PASS"

check-abi-smoke-gate:
	@$(PYTHON) tools/gates.py check-abi-smoke-gate
	@echo "check-abi-smoke-gate: PASS"

# EXTERNAL_USERLAND_UPGRADE_CHECKLIST: run every gate group that must pass
# before an upgraded musl/sbase/mksh (or other imported userland) is
# accepted -- see docs/external-dependencies.md.
.PHONY: check-upgrade-userland-smokes
check-upgrade-userland-smokes: smoke-abi-linux smoke-mlibc smoke-mlibc-sbase smoke-mlibc-mksh

check-doc-drift:
	@$(PYTHON) tools/gates.py check-doc-drift
	@$(PYTHON) tools/gen_linux_syscall_coverage.py
	@$(PYTHON) tools/gates.py check-doc-drift --segment 1
	@echo "check-doc-drift: PASS"

check-task-lifetime-boundary:
	@$(PYTHON) tools/gates.py check-task-lifetime-boundary
	@echo "check-task-lifetime-boundary: PASS"

check-blocking-point-boundary: smoke-proc-stress smoke-futex-stress
	@$(PYTHON) tools/gates.py check-blocking-point-boundary
	@echo "check-blocking-point-boundary: PASS"

check-signal-exit-boundary: smoke-proc-stress
	@$(PYTHON) tools/gates.py check-signal-exit-boundary
	@echo "check-signal-exit-boundary: PASS"

check-timeout-ownership-boundary: smoke-futex-stress smoke-timeout-test
	@$(PYTHON) tools/gates.py check-timeout-ownership-boundary
	@echo "check-timeout-ownership-boundary: PASS"

check-smp-runqueue-boundary: smoke-sched-stress
	@$(PYTHON) tools/gates.py check-smp-runqueue-boundary
	@echo "check-smp-runqueue-boundary: PASS"

check-process-lock-split-boundary: smoke-sched-stress
	@$(PYTHON) tools/gates.py check-process-lock-split-boundary
	@echo "check-process-lock-split-boundary: PASS"

check-doc-test-gates: check-concurrency-foundation check-smp-platform-boundary check-task-state-boundary check-task-lifetime-boundary check-blocking-point-boundary check-signal-exit-boundary check-timeout-ownership-boundary check-smp-runqueue-boundary check-process-lock-split-boundary check-mm-lock-model check-io-progress-model check-vfs-abstraction check-abi-boundary check-driver-core-model check-external-dependency-boundary check-abi-smoke-gate check-doc-drift
	@$(PYTHON) tools/gates.py check-doc-test-gates
	@echo "check-doc-test-gates: PASS"

check-final-definition: check-doc-test-gates
	@$(PYTHON) tools/gates.py check-final-definition
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

check-release-build:
	$(MAKE) all

check-concurrency-foundation: smoke-smp-bringup
	@$(PYTHON) tools/gates.py check-concurrency-foundation
	@$(MAKE) ARCH=$(ARCH) NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 BRINGUP=1 kernel-only >/dev/null
	@echo "check-concurrency-foundation: PASS"

check-mm-lock-model: smoke-mm-stress smoke-mm-fork-exec-race
	@$(PYTHON) tools/gates.py check-mm-lock-model
	@echo "check-mm-lock-model: PASS"

check-io-progress-model:
	@$(PYTHON) tools/gates.py check-io-progress-model
	@echo "check-io-progress-model: PASS"

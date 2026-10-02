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
.PHONY: vf2-firmware vf2-extra-sources vf2-image vf2-world-image
VF2_IMAGE_BUILD_DIR := .kernel-build/riscv64-visionfive2-linux-dev-embedded-nommu
# The board's /extra partition is filled by an apk world image rather than by
# compiling software out of user/external/.  mkrootfs.py already emits a
# partitionless raw ext4, which is exactly what make-boot-image.sh expects for
# that slot, and the kernel mounts it the same way.  PKG_ALPINE stays on because
# the whole point is to get the upstream packages.  PKG_IMAGE_DIR is set in
# tools/targets-pkg.mk, which is included later; recursive expansion defers the
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
		LDSCRIPT=kernel/platform/visionfive2/ldscript-nommu.ld \
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
# See the deprecation stub in tools/targets-extra.mk.
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
# tools/targets-extra.mk: the full card was defined by its source-built
# extra.img, and vf2-sdcard fills that slot with an apk world instead
# (VF2_WORLD, default 'devel').

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
	@$(PYTHON) tools/gen_linux_syscall_coverage.py --check
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

check-doc-test-gates: check-concurrency-foundation check-smp-platform-boundary check-task-state-boundary check-task-lifetime-boundary check-blocking-point-boundary check-signal-exit-boundary check-timeout-ownership-boundary check-smp-runqueue-boundary check-process-lock-split-boundary check-mm-lock-model check-io-progress-model check-vfs-abstraction check-abi-boundary check-driver-core-model check-drm-store-locking check-drm-abi check-external-dependency-boundary check-abi-smoke-gate check-doc-drift
	@$(PYTHON) tools/gates.py check-doc-test-gates
	@echo "check-doc-test-gates: PASS"

# Not a distinct gate, and deliberately so.
#
# Every one of the 11 assertions in tools/gates.toml's check-final-definition
# entry is already asserted by a gate check-doc-test-gates runs: MM_LOCK_MODEL
# by check-mm-lock-model, TASK_STATE_MUTATION_CONTRACT by
# check-concurrency-foundation, TASK_REFERENCE_LIFETIME by
# check-task-state-boundary, VFS_REFCOUNT_HELPER_CONTRACT and
# VFS_OPEN_DISPATCH_CONTRACT by check-vfs-abstraction, the three
# NATIVE_*/LINUX_ABI_* markers by check-abi-boundary,
# KERNEL_PROGRESS_SERVICE_CONTRACT by check-io-progress-model,
# DRIVER_CORE_CONCURRENCY_MODEL by check-driver-core-model and
# EXTERNAL_USERLAND_UPGRADE_CHECKLIST by
# check-external-dependency-boundary.  So the target adds no coverage, and CI
# runs it through its strict superset check-doc-test-gates only: a second CI job
# would replay the same nine QEMU smokes to re-check eleven markers that the
# first job already checked.
#
# The rule stays because it is load-bearing twice over, neither of which is
# visible from this recipe: Makefile's SMP_VALIDATION_GOALS lists it, so naming
# this target is one of three ways to tell the NR_CPUS guard that an
# unverified NR_CPUS>1 build is deliberate; and tools/gates.toml owns the
# assertion list, which this surface may not edit.
#
# The previous PASS line read "SMP smoke tracked separately by TODO section 10".
# That was false: SMP *is* covered here, through check-concurrency-foundation,
# which runs smoke-smp-bringup and builds a 2-CPU configuration.  The line was
# also the only thing the target added over its prerequisite, so it was the one
# part of this target that misdescribed it.
check-final-definition: check-doc-test-gates
	@$(PYTHON) tools/gates.py check-final-definition
	@echo "check-final-definition: PASS (SMP covered via check-concurrency-foundation; every assertion here is already asserted by a gate check-doc-test-gates runs)"

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

# Honesty policy: an unimplemented facility must report absence, never
# fabricate output.  A stub that returns success is strictly worse than a
# missing one, because the caller cannot tell and proceeds on a false belief.
#
# This gate is mostly *negative*: it asserts the specific anti-patterns that
# were removed stay removed.  Positive marker checks elsewhere in this file
# only prove a string exists; these prove a bug cannot come back unnoticed.
check-honesty-policy:
	@echo "check-honesty-policy: auditing fail-closed behaviour"
	@if rg -q '", 1 \},' kernel/net/socket_alg.c; then \
	  echo "  FAIL AF_ALG advertises an algorithm it cannot compute"; exit 1; fi
	@if rg -Uq '(?s)level == SOL_SOCKET\)\s*\n\s*return 0;' kernel/net/socket_control.c; then \
	  echo "  FAIL setsockopt still reports success for discarded SOL_SOCKET options"; exit 1; fi
	@if rg -q 'congestion\[\] = "cubic"' kernel/net/socket_control.c; then \
	  echo "  FAIL TCP_CONGESTION hardcodes an algorithm the stack lacks"; exit 1; fi
	@if rg -q '0\.00 0\.00 0\.00 1/64' kernel/fs/procfs/procfs_render.c; then \
	  echo "  FAIL /proc/loadavg is a constant again"; exit 1; fi
	@if rg -Uq '(?s)PF_PID_IO.{0,400}total_time' kernel/fs/procfs/procfs_render.c; then \
	  echo "  FAIL /proc/pid/io reports CPU ticks as byte counts"; exit 1; fi
	@if rg -q 'avg60=0\.00 avg300=0\.00 total=%llu' kernel/core/psi.c; then \
	  echo "  FAIL PSI discards its computed avg60/avg300"; exit 1; fi
	@if rg -Uq '(?s)sys_getrusage.{0,700}total_time' kernel/abi/linux/sys_proc.c; then \
	  echo "  FAIL getrusage still conflates utime and stime"; exit 1; fi
	@if ! rg -Uq '(?s)PF_PID_PAGEMAP\).{0,300}proc_task_may_access' kernel/fs/procfs/procfs.c; then \
	  echo "  FAIL /proc/pid/pagemap is not gated on task ownership"; exit 1; fi
	@if ! rg -Uq '(?s)int64_t sys_reboot.{0,700}CAP_SYS_BOOT' kernel/abi/linux/sys_proc.c; then \
	  echo "  FAIL sys_reboot is not gated on CAP_SYS_BOOT"; exit 1; fi
	@if ! rg -q 'int \(\*flush\)\(struct block_dev \*dev\);' kernel/include/drivers/block/block_dev.h; then \
	  echo "  FAIL block_dev_t lost its flush primitive"; exit 1; fi
	@if ! rg -q 'partition\.block\.flush\s*=\s*partition_flush' kernel/fs/mount_setup.c; then \
	  echo "  FAIL partition wrapper stopped forwarding flush"; exit 1; fi
	@if ! rg -q 'class_block->block\.flush\s*=\s*class_block_flush' kernel/fs/mount_setup.c; then \
	  echo "  FAIL class wrapper stopped forwarding flush"; exit 1; fi
	@if ! rg -q 'bc->dev->flush' kernel/fs/block_cache.c; then \
	  echo "  FAIL block cache sync no longer reaches the device flush"; exit 1; fi
	@if ! rg -q 'VIRTIO_BLK_T_FLUSH' kernel/drivers/block/virtio_blk.c; then \
	  echo "  FAIL virtio-blk no longer issues a flush request"; exit 1; fi
	@if ! rg -q 'ATA_CMD_FLUSH_CACHE_EXT' kernel/drivers/block/ahci.c; then \
	  echo "  FAIL AHCI no longer issues FLUSH CACHE"; exit 1; fi
	@# New subsystems must stay wired, not merely present.
	@if ! rg -q 'netfilter_input\(st->rx_frame' kernel/net/lwip_stack.c; then \
	  echo "  FAIL netfilter input hook is not wired"; exit 1; fi
	@if ! rg -q 'netfilter_output\(st->tx_frame' kernel/net/lwip_stack.c; then \
	  echo "  FAIL netfilter output hook is not wired"; exit 1; fi
	@if ! rg -q 'proc_loadavg_tick' kernel/proc/sched.c; then \
	  echo "  FAIL load average is no longer sampled from the scheduler tick"; exit 1; fi
	@if ! rg -q 'proc_io_account' kernel/fs/vfs/file.c; then \
	  echo "  FAIL read/write no longer charge I/O accounting"; exit 1; fi
	@echo "check-honesty-policy: PASS"

check-mm-lock-model: smoke-mm-stress smoke-mm-fork-exec-race
	@$(PYTHON) tools/gates.py check-mm-lock-model
	@echo "check-mm-lock-model: PASS"

check-mm-pt-lock-order:
	@$(PYTHON) tools/gates.py check-mm-pt-lock-order
	@echo "check-mm-pt-lock-order: PASS"

check-io-progress-model:
	@$(PYTHON) tools/gates.py check-io-progress-model
	@echo "check-io-progress-model: PASS"

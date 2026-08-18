# ----------------------------------------------------------------
# Preliminary-round local evaluation.
#
# `make all` remains the final-round submission build.  The preliminary build
# entry is `make preliminary-all`, while the targets below run those exact
# preliminary artifacts with the device order published for the preliminary
# platform: the writable test filesystem is x0 and A20OS's auxiliary FAT image
# is x1.  Keep the downloaded base read-only and copy it to a per-run raw image
# so the QEMU command still matches the platform without modifying the base.
# ----------------------------------------------------------------
.PHONY: preliminary-eval preliminary-eval-rv preliminary-eval-la \
	eval eval-all eval-rv eval-la eval-dev-build-rv eval-dev-build-la \
	eval-check eval-check-rv eval-check-la \
	final-stage4-rv-buildstorm-1c final-stage4-rv-buildstorm-8c \
	final-stage4-la-buildstorm-1c final-stage4-la-buildstorm-8c \
	final-stage5-rv-buildstorm final-stage5-la-buildstorm \
	final-stage8-rv-nested-qemu final-stage8-la-nested-qemu

EVAL_DIR := .eval-state
EVAL_LOGS := $(EVAL_DIR)/logs
EVAL_TIMEOUT ?= 36000
PRELIMINARY_EVAL_MEMORY ?= 1G
PRELIMINARY_EVAL_SMP ?= 1
PRELIMINARY_EVAL_IMAGE_DIR ?= /tmp/a20os-preliminary-images
PRELIMINARY_EVAL_WORK_ROOT ?= /tmp/a20os-preliminary-runs
PRELIMINARY_QEMU_VERSION ?= 9.2.1

SDCARD_RV_URL := https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-rv.img.xz
SDCARD_LA_URL := https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-la.img.xz
PRELIMINARY_SDCARD_RV := $(PRELIMINARY_EVAL_IMAGE_DIR)/sdcard-rv.img
PRELIMINARY_SDCARD_LA := $(PRELIMINARY_EVAL_IMAGE_DIR)/sdcard-la.img

$(EVAL_DIR) $(EVAL_LOGS) $(PRELIMINARY_EVAL_IMAGE_DIR) $(PRELIMINARY_EVAL_WORK_ROOT):
	mkdir -p $@

# --- Official preliminary test images ---------------------------------------
# A project-root image is accepted only when it is a real image.  This avoids
# treating Windows interix-link placeholders (roughly 100 bytes) as disks.
$(PRELIMINARY_SDCARD_RV): | $(PRELIMINARY_EVAL_IMAGE_DIR)
	@set -e; \
		tmp="$@.tmp"; archive="$@.xz"; archive_tmp="$@.xz.tmp"; \
		rm -f "$$tmp" "$$archive_tmp"; \
		if [ -f sdcard-rv.img ] && [ "$$(stat -c %s sdcard-rv.img)" -gt 1048576 ]; then \
			echo "[preliminary-eval] copying project-root sdcard-rv.img"; \
			cp --reflink=auto sdcard-rv.img "$$tmp"; \
		else \
			if [ ! -f "$$archive" ]; then \
				echo "[preliminary-eval] downloading sdcard-rv.img.xz"; \
				if command -v curl >/dev/null 2>&1; then \
					curl -fL --retry 3 -o "$$archive_tmp" $(SDCARD_RV_URL); \
				elif command -v wget >/dev/null 2>&1; then \
					wget -q -O "$$archive_tmp" $(SDCARD_RV_URL); \
				else \
					echo "[preliminary-eval] curl or wget is required" >&2; exit 1; \
				fi; \
				xz -t "$$archive_tmp"; \
				mv "$$archive_tmp" "$$archive"; \
			fi; \
			xz -t "$$archive"; \
			xz -dc "$$archive" > "$$tmp"; \
		fi; \
		test "$$(stat -c %s "$$tmp")" -gt 1048576; \
		chmod 0444 "$$tmp"; \
		mv "$$tmp" $@

$(PRELIMINARY_SDCARD_LA): | $(PRELIMINARY_EVAL_IMAGE_DIR)
	@set -e; \
		tmp="$@.tmp"; archive="$@.xz"; archive_tmp="$@.xz.tmp"; \
		rm -f "$$tmp" "$$archive_tmp"; \
		if [ -f sdcard-la.img ] && [ "$$(stat -c %s sdcard-la.img)" -gt 1048576 ]; then \
			echo "[preliminary-eval] copying project-root sdcard-la.img"; \
			cp --reflink=auto sdcard-la.img "$$tmp"; \
		else \
			if [ ! -f "$$archive" ]; then \
				echo "[preliminary-eval] downloading sdcard-la.img.xz"; \
				if command -v curl >/dev/null 2>&1; then \
					curl -fL --retry 3 -o "$$archive_tmp" $(SDCARD_LA_URL); \
				elif command -v wget >/dev/null 2>&1; then \
					wget -q -O "$$archive_tmp" $(SDCARD_LA_URL); \
				else \
					echo "[preliminary-eval] curl or wget is required" >&2; exit 1; \
				fi; \
				xz -t "$$archive_tmp"; \
				mv "$$archive_tmp" "$$archive"; \
			fi; \
			xz -t "$$archive"; \
			xz -dc "$$archive" > "$$tmp"; \
		fi; \
		test "$$(stat -c %s "$$tmp")" -gt 1048576; \
		chmod 0444 "$$tmp"; \
		mv "$$tmp" $@

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
	$(MAKE) ARCH=riscv64 FAT32_IMAGE_MB=128 USER_BUILD_DESKTOP=0 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_RV) - ::/etc/contest-mode

eval-dev-build-la:
	$(MAKE) ARCH=loongarch64 FAT32_IMAGE_MB=128 USER_BUILD_DESKTOP=0 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_LA) - ::/etc/contest-mode

# --- QEMU launch ------------------------------------------------------------
# The published LoongArch command assigns virtio-pci devices to
# virtio-mmio-bus.*.  QEMU rejects that mixed-bus combination, so let PCI
# assign slots automatically while retaining the official x0/x1 drive order.
define RUN_PRELIMINARY_QEMU_RV
	@set -eu; \
		actual_version="$$(qemu-system-riscv64 --version | sed -n '1s/.*version \([^ ]*\).*/\1/p')"; \
		if [ "$$actual_version" != "$(PRELIMINARY_QEMU_VERSION)" ]; then \
			echo "[preliminary-eval][rv] warning: QEMU $$actual_version, platform uses $(PRELIMINARY_QEMU_VERSION)"; \
		fi; \
		run_dir="$$(mktemp -d '$(PRELIMINARY_EVAL_WORK_ROOT)/riscv64.XXXXXX')"; \
		trap 'rm -rf -- "$$run_dir"' EXIT INT TERM; \
		cp --reflink=auto $(PRELIMINARY_SDCARD_RV) "$$run_dir/fs.img"; \
		chmod 0644 "$$run_dir/fs.img"; \
		cp --reflink=auto disk.img "$$run_dir/disk.img"; \
		echo "[preliminary-eval][rv] fs=x0 disk.img=x1 raw timeout=$(EVAL_TIMEOUT)s"; \
		$(TIMEOUT) --foreground $(EVAL_TIMEOUT) \
		qemu-system-riscv64 -machine virt -kernel kernel-rv \
			-m $(PRELIMINARY_EVAL_MEMORY) -nographic -smp $(PRELIMINARY_EVAL_SMP) -bios default \
			-drive "file=$$run_dir/fs.img,if=none,format=raw,id=x0" \
			-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
			-no-reboot -device virtio-net-device,netdev=net -netdev user,id=net \
			-rtc base=utc \
			-drive "file=$$run_dir/disk.img,if=none,format=raw,id=x1" \
			-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		2>&1 | tee $(EVAL_LOGS)/serial-rv.txt
endef

define RUN_PRELIMINARY_QEMU_LA
	@set -eu; \
		actual_version="$$(qemu-system-loongarch64 --version | sed -n '1s/.*version \([^ ]*\).*/\1/p')"; \
		if [ "$$actual_version" != "$(PRELIMINARY_QEMU_VERSION)" ]; then \
			echo "[preliminary-eval][la] warning: QEMU $$actual_version, platform uses $(PRELIMINARY_QEMU_VERSION)"; \
		fi; \
		run_dir="$$(mktemp -d '$(PRELIMINARY_EVAL_WORK_ROOT)/loongarch64.XXXXXX')"; \
		trap 'rm -rf -- "$$run_dir"' EXIT INT TERM; \
		cp --reflink=auto $(PRELIMINARY_SDCARD_LA) "$$run_dir/fs.img"; \
		chmod 0644 "$$run_dir/fs.img"; \
		cp --reflink=auto disk-la.img "$$run_dir/disk-la.img"; \
		echo "[preliminary-eval][la] fs=x0 disk-la.img=x1 raw timeout=$(EVAL_TIMEOUT)s"; \
		$(TIMEOUT) --foreground $(EVAL_TIMEOUT) \
		qemu-system-loongarch64 -kernel kernel-la \
			-m $(PRELIMINARY_EVAL_MEMORY) -nographic -smp $(PRELIMINARY_EVAL_SMP) \
			-drive "file=$$run_dir/fs.img,if=none,format=raw,id=x0" \
			-device virtio-blk-pci,drive=x0 \
			-no-reboot -device virtio-net-pci,netdev=net0 \
			-netdev user,id=net0,hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555 \
			-rtc base=utc \
			-drive "file=$$run_dir/disk-la.img,if=none,format=raw,id=x1" \
			-device virtio-blk-pci,drive=x1 \
		2>&1 | tee $(EVAL_LOGS)/serial-la.txt
endef

# --- Top-level preliminary evaluation targets -------------------------------
preliminary-eval-rv: contest-rv $(PRELIMINARY_SDCARD_RV) | $(EVAL_LOGS) $(PRELIMINARY_EVAL_WORK_ROOT)
	@echo "[preliminary-eval] launching RISC-V QEMU"
	$(RUN_PRELIMINARY_QEMU_RV)
	$(MAKE) eval-check-rv

preliminary-eval-la: contest-la $(PRELIMINARY_SDCARD_LA) | $(EVAL_LOGS) $(PRELIMINARY_EVAL_WORK_ROOT)
	@echo "[preliminary-eval] launching LoongArch QEMU"
	$(RUN_PRELIMINARY_QEMU_LA)
	$(MAKE) eval-check-la

preliminary-eval:
	@set -e; for target in $(DEFAULT_EVAL_TARGETS); do $(MAKE) $$target; done
	@echo "[preliminary-eval] complete"

eval-rv: preliminary-eval-rv
eval-la: preliminary-eval-la
eval: preliminary-eval

eval-all:
	$(MAKE) preliminary-eval-rv
	$(MAKE) preliminary-eval-la
	@echo "[preliminary-eval] full architecture evaluation complete"

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
	$(if $(strip $(FINAL_EVAL_TIMEOUT)),FINAL_EVAL_TIMEOUT="$(FINAL_EVAL_TIMEOUT)",$(if $(strip $(5)),FINAL_EVAL_TIMEOUT="$(strip $(5))")) \
	bash ./tools/run_final_eval.sh $(1) $(2) $(3) $(4)
endef

final-eval-rv-cagent:
	$(call RUN_FINAL_EVAL,riscv64,cagent)

final-eval-la-cagent:
	$(call RUN_FINAL_EVAL,loongarch64,cagent)

final-eval-rv-buildstorm:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm)

final-eval-la-buildstorm:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm)

final-probe-rv-buildstorm-1c:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,1)

final-probe-rv-buildstorm-8c:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8)

final-probe-la-buildstorm-1c:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,1)

final-probe-la-buildstorm-8c:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8)

final-stage4-rv-buildstorm-1c:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,1,stage4-cargo-minibuild)

final-stage4-rv-buildstorm-8c:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage4-cargo-minibuild)

final-stage4-la-buildstorm-1c:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,1,stage4-cargo-minibuild)

final-stage4-la-buildstorm-8c:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage4-cargo-minibuild)

final-stage5-rv-buildstorm:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage5-official-minibuild)

final-stage5-la-buildstorm:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage5-official-minibuild)

final-stage6-rv-ext4-dir-tail:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage6-ext4-dir-tail)

final-stage6-la-ext4-dir-tail:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage6-ext4-dir-tail)

final-stage6-rv-helper:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage6-precompiled-helper)

final-stage6-la-helper:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage6-precompiled-helper)

final-stage7-rv-shebang:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,1,stage7-shebang-exec,900)

final-stage7-la-shebang:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,1,stage7-shebang-exec,900)

final-stage7-rv-rustc-j8:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-rustc-j8,3600)

# Diagnostic only: put the 32-rustc output directory on guest tmpfs to
# isolate process/MM cost from ext4 allocation and writeback serialization.
final-stage7-rv-rustc-j8-tmpfs:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-rustc-j8-tmpfs,3600)

final-stage7-rv-rustc-j8-profile:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-rustc-j8-profile,1200)

.PHONY: final-stage7-rv-buildstorm-rustc-j8-cache-profile
final-stage7-rv-buildstorm-rustc-j8-cache-profile:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-rustc-j8-cache-profile,1800)

.PHONY: final-stage7-rv-writeback-256m
final-stage7-rv-writeback-256m:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-writeback-256m,1200)

final-stage7-la-rustc-j8:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-rustc-j8,3600)

final-stage7-rv-rustc-llvm-j8:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-rustc-llvm-j8,3600)

final-stage7-la-rustc-llvm-j8:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-rustc-llvm-j8,3600)

final-stage7-rv-1c-j1:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,1,stage7-full-j1,28800)

final-stage7-la-1c-j1:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,1,stage7-full-j1,28800)

final-stage7-rv-8c-j1:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-full-j1,28800)

final-stage7-la-8c-j1:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-full-j1,28800)

final-stage7-rv-8c-j2:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-full-j2,28800)

final-stage7-la-8c-j2:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-full-j2,28800)

final-stage7-rv-8c-j4:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-full-j4,28800)

final-stage7-la-8c-j4:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-full-j4,28800)

final-stage7-rv-8c-j8:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-full-j8,28800)

final-stage7-la-8c-j8:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-full-j8,28800)

final-stage7-rv-8c-default:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-full-default,3000)

final-stage7-la-8c-default:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage7-full-default,3000)

# Diagnostic only: keep the official source/toolchain and full cargo command,
# but place the architecture-specific output tree on tmpfs to separate kernel
# compute/MM cost from ext4 writeback cost.  This is never a formal judge path.
final-stage7-rv-8c-default-tmpfs:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage7-full-default-tmpfs,3000)

# Exercise the online-only nested-QEMU boundary without editing official
# tests.  RISC-V boots the already-built ArceOS artifact; LoongArch uses a
# direct serial payload at the official 2 GiB RAM size to isolate QEMU startup,
# mmap and TCG from the separate tg-xtask UEFI/FAT preparation layer.
final-stage8-rv-nested-qemu:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage8-nested-qemu,900)

final-stage8-la-nested-qemu:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage8-nested-qemu,900)

final-stage9-rv-1c-perf:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,1,stage9-perf-feedback,1200)

final-stage9-rv-8c-perf:
	$(call RUN_FINAL_EVAL,riscv64,buildstorm-probe,8,stage9-perf-feedback,1200)

final-stage9-la-1c-perf:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,1,stage9-perf-feedback,1200)

final-stage9-la-8c-perf:
	$(call RUN_FINAL_EVAL,loongarch64,buildstorm-probe,8,stage9-perf-feedback,1200)

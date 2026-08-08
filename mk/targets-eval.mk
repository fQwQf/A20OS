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
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.3 \
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
	$(if $(strip $(5)),FINAL_EVAL_TIMEOUT="$(strip $(5))",$(if $(strip $(FINAL_EVAL_TIMEOUT)),FINAL_EVAL_TIMEOUT="$(FINAL_EVAL_TIMEOUT)")) \
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

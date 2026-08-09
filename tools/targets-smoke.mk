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

check-a20-idl: user/svc/a20_services_idl.h
	@tmp="$$(mktemp)"; \
	trap 'rm -f "$$tmp"' EXIT; \
	$(A20_IDL_PYTHON) tools/a20idl.py user/svc/a20_services.idl "$$tmp"; \
	cmp -s "$$tmp" user/svc/a20_services_idl.h || { \
		echo "check-a20-idl: generated header is stale"; exit 1; }; \
	echo "check-a20-idl: PASS"

smoke-iommu-discovery:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/iommu-discovery-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-device riscv-iommu-pci,bus=pcie.0 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '\[IOMMU\] hardware initialized' "$$log" && \
	   grep -q 'translation domain verified' "$$log" && \
	   grep -q 'unmapped iova=0x20000000 -> fault=1' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-iommu-discovery: PASS (hardware initialized, translation verified); log saved to $$log"; \
	else \
		echo "smoke-iommu-discovery: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
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
	$(PYTHON) tools/smoke_qemu_gui.py \
		--arch x86_64 \
		--qemu qemu-system-x86_64 \
		--kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf \
		--disk .kernel-build/x86_64-qemu-virt-x86_64-both-dev/gui-fat32.img \
		--timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-riscv64:
	$(MAKE) ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=both BRINGUP=0 \
		.kernel-build/riscv64-qemu-virt-riscv64-both-dev/gui-fat32.img
	$(PYTHON) tools/smoke_qemu_gui.py \
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
	$(PYTHON) tools/smoke_qemu_gui.py --arch aarch64 --qemu qemu-system-aarch64 \
		--kernel .kernel-build/aarch64-qemu-virt-aarch64-both-dev/kernel.elf \
		--disk .kernel-build/aarch64-qemu-virt-aarch64-both-dev/gui-fat32.img \
		--artifacts .kernel-build/smoke/qemu-gui-aarch64 --timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-arm32:
	$(MAKE) ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 \
		.kernel-build/arm32-qemu-virt-arm32-both-dev/gui-fat32.img
	$(PYTHON) tools/smoke_qemu_gui.py --arch arm32 --qemu qemu-system-arm \
		--kernel .kernel-build/arm32-qemu-virt-arm32-both-dev/kernel.elf \
		--disk .kernel-build/arm32-qemu-virt-arm32-both-dev/gui-fat32.img \
		--artifacts .kernel-build/smoke/qemu-gui-arm32 --timeout $(SMOKE_TIMEOUT)

smoke-qemu-gui-loongarch64:
	$(MAKE) ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=both BRINGUP=0 \
		.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/gui-fat32.img
	$(PYTHON) tools/smoke_qemu_gui.py --arch loongarch64 --qemu qemu-system-loongarch64 \
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

smoke-a20-channel:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/a20-channel-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'a20_channel_test\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'A20_CHANNEL: PASS' "$$log"; then \
		echo "smoke-a20-channel: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-a20-channel: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-a20-channel: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-ptrace:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/ptrace-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'ptrace_smoke\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'PTRACE_SMOKE: PASS' "$$log"; then \
		echo "smoke-ptrace: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-ptrace: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-ptrace: failed with status $$status; tail of $$log:"; \
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
	   grep -q 'PROC_STRESS: signal-mask-park PASS' "$$log" && \
	   grep -q 'PROC_STRESS: thread-exec-cloexec PASS' "$$log"; then \
		echo "smoke-proc-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-proc-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-procfs-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/procfs-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'procfs_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'PROCFS_STRESS: PASS' "$$log"; then \
		echo "smoke-procfs-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-procfs-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-mm-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mm-stress-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect 'mksh main starting!' --expect '# ' \
		--send-line 'mm_stress' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT_MM_ST) qemu-system-riscv64 \
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

smoke-mm-fork-exec-race:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mm-fork-exec-race-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'mm_stress --vma-fork-exec-only\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_MM_FORK_EXEC) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 8 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'MM_VMA_FORK_EXEC: PASS' "$$log"; then \
		echo "smoke-mm-fork-exec-race: PASS; log saved to $$log"; \
	else \
		echo "smoke-mm-fork-exec-race: failed with status $$status; tail of $$log:"; \
		tail -n 100 "$$log"; \
		exit 1; \
	fi

smoke-vfs-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	$(MAKE) -s ARCH=riscv64 ABI=linux BRINGUP=0 .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/isofs.img
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
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1 \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/isofs.img,if=none,format=raw,id=x2 \
		-device virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2 \
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

smoke-scm-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/scm-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'scm_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SCM_STRESS: PASS' "$$log"; then \
		echo "smoke-scm-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-scm-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-evdev-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/evdev-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'evdev_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'EVDEV_STRESS: PASS' "$$log"; then \
		echo "smoke-evdev-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-evdev-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-signalfd-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/signalfd-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'signalfd_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SIGNALFD_STRESS: PASS' "$$log"; then \
		echo "smoke-signalfd-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-signalfd-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

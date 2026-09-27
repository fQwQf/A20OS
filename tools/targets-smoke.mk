# 冒烟门禁的资源预检。
#
# 下面绝大多数 smoke-* 目标直接起 qemu-system-*，绕过了 tools/a20，因此没有实例
# TOML 可供门禁，也就完全没有资源检查。这恰恰是 tools/a20_resource.py 存在的理由：
# QEMU 申请到宿主机给不出的内存时，不会得到一个非零退出码，而是宿主 OOM killer
# 挑一个进程杀掉——被杀的那个通常不是你在调试的东西。所以这里复用同一套 A20_*
# 策略，在起 QEMU 之前等一次。
#
# 参数必须与该目标 qemu 命令行里的 -m/-smp 一致；否则门禁在保护另一件事，比没有
# 门禁更糟。默认值取自脚本里逐个目标的实际取值（全部 -m 1G）。
#
# 与 `tools/a20 run` 的区别：a20 的 A20_WAIT_TIMEOUT 默认 0（一直等），因为交互式
# 跑实例时"等资源释放"正是期望行为；CI 门禁不能永远等下去（宿主机磁盘真的满了
# 时会挂死而不是失败），所以这里给一个可覆盖的有界默认 900s。
define smoke-gate
A20_WAIT_TIMEOUT=$${A20_WAIT_TIMEOUT:-900} $(PYTHON) tools/a20_resource.py -m $(1) -c $(2)
endef

# Thin wrapper: the smoke definition lives in instances/smoke-riscv64.toml.
smoke-riscv64:
	tools/a20 test smoke-riscv64

check-a20-idl: user/svc/a20_services_idl.h
	@tmp="$$(mktemp)"; \
	trap 'rm -f "$$tmp"' EXIT; \
	$(A20_IDL_PYTHON) tools/a20idl.py user/svc/a20_services.idl "$$tmp"; \
	cmp -s "$$tmp" user/svc/a20_services_idl.h || { \
		echo "check-a20-idl: generated header is stale"; exit 1; }; \
	echo "check-a20-idl: PASS"

# Thin wrapper: the smoke definition lives in instances/smoke-iommu-discovery.toml.
smoke-iommu-discovery:
	tools/a20 test smoke-iommu-discovery

smoke-iommu-udriver-isolation:
	$(call smoke-gate,1G,1)
	$(MAKE) -j1 ARCH=riscv64 ABI=both BRINGUP=0 PROFILE=benchmark dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/iommu-udriver-isolation-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'cat /proc/a20/iommu\npoweroff\n'; } | \
	$(TIMEOUT) 30s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-device riscv-iommu-pci,bus=pcie.0,addr=1.0 \
		-device edu,bus=pcie.0,addr=2.0,dma_mask=0xffffffffffffffff \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'UEDUD: mapped DMA ok' "$$log" && \
	   grep -q 'UEDUD: unmapped DMA fault' "$$log" && \
	   grep -q '\[IOMMU\] DMA fault blocked did=16 cause=15' "$$log" && \
	   grep -q 'UEDUD: recovered' "$$log" && \
	   grep -q 'UEDUD: PASS' "$$log" && \
	   grep -q 'enabled: 1' "$$log" && \
	   grep -Eq 'domains_claimed: [1-9]' "$$log" && \
	   grep -Eq 'maps: [1-9]' "$$log" && \
	   grep -Eq 'unmaps: [1-9]' "$$log" && \
	   grep -Eq 'faults: [1-9]' "$$log" && \
	   grep -Eq 'blocked_events: [1-9]' "$$log" && \
	   grep -Eq 'last_fault_cause: 15' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-iommu-udriver-isolation: PASS; log saved to $$log"; \
	else \
		echo "smoke-iommu-udriver-isolation: failed with status $$status; tail of $$log:"; \
		tail -n 120 "$$log"; exit 1; \
	fi

# Thin wrapper: the smoke definition lives in instances/smoke-loongarch64.toml.
smoke-loongarch64:
	tools/a20 test smoke-loongarch64

# Thin wrapper: the smoke definition lives in instances/smoke-aarch64.toml.
smoke-aarch64:
	tools/a20 test smoke-aarch64

# Thin wrapper: the smoke definition lives in instances/smoke-x86_64.toml.
smoke-x86_64:
	tools/a20 test smoke-x86_64

# Behavioral SMP gate: boot a NR_CPUS=2 BRINGUP kernel and require it to
# complete bring-up and power off, exercising SMP init on real secondaries.
smoke-smp-bringup:
	$(call smoke-gate,1G,2)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=1 NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/riscv64-smp2-bringup.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 2 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-bringup-smp2/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'part ok' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-smp-bringup: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-smp-bringup: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-smp-bringup: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

# Thin wrapper: the smoke definition lives in instances/smoke-arm32.toml.
smoke-arm32:
	tools/a20 test smoke-arm32

# Thin wrapper: the smoke definition lives in instances/smoke-riscv32.toml.
smoke-riscv32:
	tools/a20 test smoke-riscv32

smoke-arch-mmu-matrix:
	$(call smoke-gate,1G,1)
	@set -e; \
	for spec in arm32:0 aarch64:0 riscv64:0 riscv32:0 \
		    arm32:1 aarch64:1 riscv64:1 riscv32:1; do \
		status=0; \
		arch=$${spec%%:*}; nommu=$${spec##*:}; \
		variant="$$arch"; [ "$$nommu" = 1 ] && variant="$$variant-nommu"; \
		case "$$arch" in \
		arm32) board=qemu-virt-arm32 ;; \
		aarch64) board=qemu-virt-aarch64 ;; \
		riscv64) board=qemu-virt-riscv64 ;; \
		riscv32) board=qemu-virt-riscv32 ;; \
		esac; \
		build=".kernel-build/$$arch-$$board-both-dev"; \
		case "$$arch" in arm32|riscv32) build="$$build-embedded" ;; esac; \
		[ "$$nommu" = 1 ] && build="$$build-nommu"; \
		log=".kernel-build/smoke/$$variant-shell.log"; \
		mkdir -p .kernel-build/smoke; \
		echo "=== smoke-arch-mmu-matrix: $$variant ==="; \
		$(MAKE) ARCH=$$arch ABI=both BRINGUP=0 NOMMU=$$nommu dev-build >/dev/null; \
		if [ "$$nommu" = 1 ]; then \
			echo "smoke-arch-mmu-matrix: $$variant build-only PASS (NOMMU runtime is platform-specific)"; \
			continue; \
		fi; \
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

# Thin wrapper: the smoke definition lives in instances/smoke-ppc64le.toml.
smoke-ppc64le:
	tools/a20 test smoke-ppc64le

# Thin wrapper: the smoke definition lives in instances/smoke-abi-linux.toml.
smoke-abi-linux:
	tools/a20 test smoke-abi-linux

smoke-net-iface:
	tools/a20 test smoke-net-iface

smoke-envelope:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/envelope-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'envelope_smoke\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_ENVELOPE) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
		if grep -q 'ENVELOPE_SMOKE: PASS' "$$log"; then \
			echo "smoke-envelope: PASS; log saved to $$log"; \
		else \
			echo "smoke-envelope: failed with status $$status; tail of $$log:"; \
			tail -n 80 "$$log"; \
			exit 1; \
		fi

smoke-envelope-pilot:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/envelope-pilot-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'envelope_pilot\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_ENVELOPE_PILOT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
		if grep -q 'ENVELOPE_PILOT: PASS' "$$log"; then \
			echo "smoke-envelope-pilot: PASS; log saved to $$log"; \
		else \
			echo "smoke-envelope-pilot: failed with status $$status; tail of $$log:"; \
			tail -n 80 "$$log"; \
			exit 1; \
		fi

smoke-envelope-bench:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/envelope-bench-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'envelope_bench\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_ENVELOPE_BENCH) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
		if grep -q 'ENVELOPE_BENCH: PASS' "$$log"; then \
			echo "smoke-envelope-bench: PASS; log saved to $$log"; \
			grep 'ENVELOPE_BENCH:' "$$log"; \
		else \
			echo "smoke-envelope-bench: failed with status $$status; tail of $$log:"; \
			tail -n 60 "$$log"; \
			exit 1; \
		fi

smoke-envelope-corpus:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/envelope-corpus-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'envelope_corpus\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_ENVELOPE_CORPUS) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
		if grep -q 'ENVELOPE_CORPUS: PASS' "$$log"; then \
			echo "smoke-envelope-corpus: PASS; log saved to $$log"; \
			grep 'ENVELOPE_CORPUS:' "$$log"; \
		else \
			echo "smoke-envelope-corpus: failed with status $$status; tail of $$log:"; \
			tail -n 60 "$$log"; \
			exit 1; \
		fi

smoke-a20-channel:
	$(call smoke-gate,1G,1)
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
		exit 1; \
	fi

smoke-ptrace:
	$(call smoke-gate,1G,1)
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
		exit 1; \
	fi

smoke-network-suite:
	$(call smoke-gate,1G,1)
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
		exit 1; \
	fi

# netctl only inspects local kernel state; it never accepts an inbound
# connection, so it must not depend on a free host port 5555.
smoke-netctl: NET_HOSTFWD=
smoke-netctl:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/netctl-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'netctl\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NETCTL: PASS' "$$log"; then \
		echo "smoke-netctl: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-netctl: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-netctl: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-network-suite-aarch64:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/network-suite-aarch64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'network_suite\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-aarch64 \
		-machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/aarch64-qemu-virt-aarch64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/aarch64-qemu-virt-aarch64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NETWORK_SUITE: PASS' "$$log"; then \
		echo "smoke-network-suite-aarch64: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-network-suite-aarch64: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-network-suite-aarch64: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-proc-a20:
	$(call smoke-gate,1G,1)
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
	$(call smoke-gate,1G,1)
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
	$(call smoke-gate,1G,1)
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
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mm-stress-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
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

# Regression gate for the V8/Node.js hint-fallback fix: mmap with a
# hint above USER_VA_LIMIT must fall back, not fail with ENOMEM.
smoke-mmprobe:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mmprobe-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'mmprobe\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'MMPROBE: PASS' "$$log"; then \
		echo "smoke-mmprobe: PASS; log saved to $$log"; \
	else \
		echo "smoke-mmprobe: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-oom-stress:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/oom-stress-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'oom_stress' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT_OOM) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'OOM_STRESS: PASS' "$$log"; then \
		echo "smoke-oom-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-oom-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# Swap runtime gate: swap_test binds a ramfs-backed loop device, then runs
# mkswap -> swapon -> duplicate-swapon(EBUSY) -> swapoff, asserting
# /proc/swaps and sysinfo totalswap transitions along the way.
smoke-swap:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/swap-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'swap_test' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT_SWAP) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SWAP_TEST: PASS' "$$log"; then \
		echo "smoke-swap: PASS; log saved to $$log"; \
	else \
		echo "smoke-swap: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-mm-fork-exec-race:
	$(call smoke-gate,1G,8)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mm-fork-exec-race-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'mm_stress --vma-fork-exec-only' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT_MM_FORK_EXEC) qemu-system-riscv64 \
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
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	$(MAKE) -s ARCH=riscv64 ABI=linux BRINGUP=0 .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/isofs.img
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/vfs-stress-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'vfs_stress' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT) qemu-system-riscv64 \
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
	$(call smoke-gate,1G,1)
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
		exit 1; \
	fi

smoke-syscall-ext:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/syscall-ext-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'syscall_ext\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SYSCALL_EXT: PASS' "$$log"; then \
		echo "smoke-syscall-ext: PASS; log saved to $$log"; \
	else \
		echo "smoke-syscall-ext: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-sched-stress:
	$(call smoke-gate,1G,1)
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
	$(call smoke-gate,1G,1)
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

smoke-futex-stress-aarch64:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/futex-stress-aarch64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'futex_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-aarch64 \
		-machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/aarch64-qemu-virt-aarch64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/aarch64-qemu-virt-aarch64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'FUTEX_STRESS: PASS' "$$log"; then \
		echo "smoke-futex-stress-aarch64: PASS; log saved to $$log"; \
	else \
		echo "smoke-futex-stress-aarch64: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-scm-stress:
	$(call smoke-gate,1G,1)
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
	$(call smoke-gate,1G,1)
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
	$(call smoke-gate,1G,1)
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

smoke-pty-stress:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/pty-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'pty_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'pty_stress: PASS' "$$log"; then \
		echo "smoke-pty-stress: PASS; log saved to $$log"; \
	else \
		echo "smoke-pty-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-timeout-test:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/timeout-test-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'timeout_test\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'TIMEOUT_TEST: PASS' "$$log"; then \
		echo "smoke-timeout-test: PASS; log saved to $$log"; \
	else \
		echo "smoke-timeout-test: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# Coredump smoke (fatal-signal ELF core dump generation)
# ================================================================
smoke-coredump:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/coredump-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'coredump_test' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'COREDUMP_TEST: PASS' "$$log"; then \
		echo "smoke-coredump: PASS; log saved to $$log"; \
	else \
		echo "smoke-coredump: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# Poll/timer edge-semantics smokes (Linux ABI poll + timer areas)
# ================================================================
smoke-poll-edge:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/poll-edge-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poll_edge\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'POLL_EDGE: PASS' "$$log"; then \
		echo "smoke-poll-edge: PASS; log saved to $$log"; \
	else \
		echo "smoke-poll-edge: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-timer-edge:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/timer-edge-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'timer_edge\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'TIMER_EDGE: PASS' "$$log"; then \
		echo "smoke-timer-edge: PASS; log saved to $$log"; \
	else \
		echo "smoke-timer-edge: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# Mount namespace smoke (unshare/setns CLONE_NEWNS + /proc/<pid>/ns/mnt)
# ================================================================
smoke-mntns:
	$(call smoke-gate,1G,1)
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mntns-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'mntns_test\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'MNTNS_TEST: PASS' "$$log"; then \
		echo "smoke-mntns: PASS; log saved to $$log"; \
	else \
		echo "smoke-mntns: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

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
# 例外：tools/stm32.mk 的 run-stm32f103-qemu 不走这个门禁。它的 machine 是
# stm32vldiscovery，内存是芯片固定的 20 KiB SRAM，没有 -m/-smp 可读；填一个
# "看起来合理"的数字等于凭空发明一个事实，而且 MCU 仿真也根本不具备把宿主机
# 撑爆的能力——门禁在这里保护不了任何东西，只会在读代码时误导人。
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
	$(PYTHON) tools/smoke.py smoke-iommu-udriver-isolation

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
	$(PYTHON) tools/smoke.py smoke-envelope

smoke-envelope-pilot:
	$(PYTHON) tools/smoke.py smoke-envelope-pilot

smoke-envelope-bench:
	$(PYTHON) tools/smoke.py smoke-envelope-bench

smoke-envelope-corpus:
	$(PYTHON) tools/smoke.py smoke-envelope-corpus

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
# connection, so it must not depend on a free host port 5555.  That is now
# baked into the case argv in tools/smoke_cases.py (no hostfwd token), so the
# old `NET_HOSTFWD=` target-specific override has nothing left to do.
smoke-netctl: NET_HOSTFWD=
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
	$(PYTHON) tools/smoke.py smoke-proc-a20

smoke-proc-stress:
	$(PYTHON) tools/smoke.py smoke-proc-stress

smoke-procfs-stress:
	$(PYTHON) tools/smoke.py smoke-procfs-stress

smoke-mm-stress:
	$(PYTHON) tools/smoke.py smoke-mm-stress

# Regression gate for the V8/Node.js hint-fallback fix: mmap with a
# hint above USER_VA_LIMIT must fall back, not fail with ENOMEM.
smoke-mmprobe:
	$(PYTHON) tools/smoke.py smoke-mmprobe

smoke-oom-stress:
	$(PYTHON) tools/smoke.py smoke-oom-stress

# Swap runtime gate: swap_test binds a ramfs-backed loop device, then runs
# mkswap -> swapon -> duplicate-swapon(EBUSY) -> swapoff, asserting
# /proc/swaps and sysinfo totalswap transitions along the way.
smoke-swap:
	$(PYTHON) tools/smoke.py smoke-swap

smoke-mm-fork-exec-race:
	$(PYTHON) tools/smoke.py smoke-mm-fork-exec-race

smoke-vfs-stress:
	$(PYTHON) tools/smoke.py smoke-vfs-stress

smoke-vfs-edge:
	$(PYTHON) tools/smoke.py smoke-vfs-edge

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
	$(PYTHON) tools/smoke.py smoke-syscall-ext

smoke-sched-stress:
	$(PYTHON) tools/smoke.py smoke-sched-stress

smoke-futex-stress:
	$(PYTHON) tools/smoke.py smoke-futex-stress

smoke-futex-stress-aarch64:
	$(PYTHON) tools/smoke.py smoke-futex-stress-aarch64

smoke-scm-stress:
	$(PYTHON) tools/smoke.py smoke-scm-stress

smoke-evdev-stress:
	$(PYTHON) tools/smoke.py smoke-evdev-stress

smoke-signalfd-stress:
	$(PYTHON) tools/smoke.py smoke-signalfd-stress

smoke-pty-stress:
	$(PYTHON) tools/smoke.py smoke-pty-stress

smoke-timeout-test:
	$(PYTHON) tools/smoke.py smoke-timeout-test

# ================================================================
# Coredump smoke (fatal-signal ELF core dump generation)
# ================================================================
smoke-coredump:
	$(PYTHON) tools/smoke.py smoke-coredump

# ================================================================
# Poll/timer edge-semantics smokes (Linux ABI poll + timer areas)
# ================================================================
smoke-poll-edge:
	$(PYTHON) tools/smoke.py smoke-poll-edge

smoke-timer-edge:
	$(PYTHON) tools/smoke.py smoke-timer-edge

# ================================================================
# Mount namespace smoke (unshare/setns CLONE_NEWNS + /proc/<pid>/ns/mnt)
# ================================================================
smoke-mntns:
	$(PYTHON) tools/smoke.py smoke-mntns

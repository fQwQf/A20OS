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
	@$(PYTHON) tools/gensync.py a20-idl \
		--idl user/svc/a20_services.idl \
		--committed user/svc/a20_services_idl.h \
		--out-name a20_services_idl.h \
		-- $(A20_IDL_PYTHON) tools/a20idl.py user/svc/a20_services.idl \
		@TMP@/a20_services_idl.h

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
	$(PYTHON) tools/smoke.py smoke-smp-bringup

# Thin wrapper: the smoke definition lives in instances/smoke-arm32.toml.
smoke-arm32:
	tools/a20 test smoke-arm32

# Thin wrapper: the smoke definition lives in instances/smoke-riscv32.toml.
smoke-riscv32:
	tools/a20 test smoke-riscv32

smoke-arch-mmu-matrix:
	@$(PYTHON) tools/smoke.py arch-mmu-matrix \
		--input-delay "$(SMOKE_INPUT_DELAY)" \
		--timeout "$(SMOKE_TIMEOUT)"
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
	$(PYTHON) tools/smoke.py smoke-a20-channel

smoke-ptrace:
	$(PYTHON) tools/smoke.py smoke-ptrace

smoke-network-suite:
	$(PYTHON) tools/smoke.py smoke-network-suite

# netctl only inspects local kernel state; it never accepts an inbound
# connection, so it must not depend on a free host port 5555.  That is now
# baked into the case argv in tools/smoke_cases.py (no hostfwd token), so the
# old `NET_HOSTFWD=` target-specific override has nothing left to do.
smoke-netctl:
	$(PYTHON) tools/smoke.py smoke-netctl
smoke-network-suite-aarch64:
	$(PYTHON) tools/smoke.py smoke-network-suite-aarch64

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

smoke-fsync-durability:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	$(MAKE) -s ARCH=riscv64 ABI=linux BRINGUP=0 .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/fsync-durability-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'fsync_durability_test' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1 \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
			-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
			> "$$log" 2>&1 || status=$$?; \
	if grep -q 'FSYNC_TEST: PASS' "$$log"; then \
		echo "smoke-fsync-durability: PASS; log saved to $$log"; \
	else \
		echo "smoke-fsync-durability: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-vfs-edge:
	$(PYTHON) tools/smoke.py smoke-vfs-edge

smoke-io-event:
	$(PYTHON) tools/smoke.py smoke-io-event

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

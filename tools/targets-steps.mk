.PHONY: check-task-lifetime-boundary check-blocking-point-boundary \
	check-signal-exit-boundary check-timeout-ownership-boundary \
	check-smp-runqueue-boundary check-process-lock-split-boundary \
	check-proc-step35 \
	check-proc-step4 check-proc-step4-local \
	check-proc-step5 check-proc-step5-local \
	check-proc-step6 check-proc-step6-local \
	check-proc-step7 check-proc-step7-local \
	check-proc-step8 check-proc-step8-local \
	check-proc-step35-local step35-rv-debug-1c step35-la-debug-1c \
	step35-rv-release-8c step35-la-release-8c \
	step6-rv-debug-1c step6-la-debug-1c \
	step6-rv-release-8c step6-la-release-8c \
	step7-rv-debug-1c step7-la-debug-1c \
	step7-rv-release-8c step7-la-release-8c \
	step8-rv-debug-1c step8-la-debug-1c \
	step8-rv-release-8c step8-la-release-8c _step35_smoke
.PHONY: _reset_obj _release_build _release_disk smoke-usb-x86_64

step35-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		BUILD_DIR=.kernel-build/step35/riscv64-debug-1c \
		STEP35_LABEL=riscv64-debug-1c _step35_smoke

step35-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		BUILD_DIR=.kernel-build/step35/loongarch64-debug-1c \
		STEP35_LABEL=loongarch64-debug-1c _step35_smoke

step35-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		BUILD_DIR=.kernel-build/step35/riscv64-release-8c \
		STEP35_LABEL=riscv64-release-8c _step35_smoke

step35-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		BUILD_DIR=.kernel-build/step35/loongarch64-release-8c \
		STEP35_LABEL=loongarch64-release-8c _step35_smoke

step6-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/riscv64-debug-1c \
		STEP35_LABEL=step6-riscv64-debug-1c _step35_smoke

step6-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/loongarch64-debug-1c \
		STEP35_LABEL=step6-loongarch64-debug-1c _step35_smoke

step6-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/riscv64-release-8c \
		STEP35_LABEL=step6-riscv64-release-8c _step35_smoke

step6-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		WAIT_TIMER_HEAP_MAX=64 REQUIRE_TIMEOUT_CAPACITY=1 \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step6/loongarch64-release-8c \
		STEP35_LABEL=step6-loongarch64-release-8c _step35_smoke

step7-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/riscv64-debug-1c \
		STEP35_LABEL=step7-riscv64-debug-1c _step35_smoke

step7-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/loongarch64-debug-1c \
		STEP35_LABEL=step7-loongarch64-debug-1c _step35_smoke

step7-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/riscv64-release-8c \
		STEP35_LABEL=step7-riscv64-release-8c _step35_smoke

step7-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step7/loongarch64-release-8c \
		STEP35_LABEL=step7-loongarch64-release-8c _step35_smoke

step8-rv-debug-1c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/riscv64-debug-1c \
		STEP35_LABEL=step8-riscv64-debug-1c _step35_smoke

step8-la-debug-1c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=1 \
		OPT="-O0 -g -DDEBUG" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=1G \
		REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/loongarch64-debug-1c \
		STEP35_LABEL=step8-loongarch64-debug-1c _step35_smoke

step8-rv-release-8c:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/riscv64-release-8c \
		STEP35_LABEL=step8-riscv64-release-8c _step35_smoke

step8-la-release-8c:
	$(MAKE) ARCH=loongarch64 ABI=linux BRINGUP=0 NR_CPUS=8 \
		OPT="-O3" USER_OPT="-O3" PROFILE=benchmark QEMU_MEMORY=8G \
		REQUIRE_SMP_RUNQUEUE=1 REQUIRE_LOCK_SPLIT=1 NET_HOSTFWD= \
		BUILD_DIR=.kernel-build/step8/loongarch64-release-8c \
		STEP35_LABEL=step8-loongarch64-release-8c _step35_smoke

_step35_smoke: dev-build
	@mkdir -p $(STEP35_LOG_DIR)
	@set -e; \
	stamp=$$(date -u +%Y%m%dT%H%M%SZ); \
	log="$(STEP35_LOG_DIR)/step35-$(STEP35_LABEL)-$$stamp.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'lifetime_stress' \
		--send-line 'cat /proc/a20/task_lifetime' \
		--send-line 'poweroff' \
		$(STEP35_TIMEOUT) $(QEMU) $(QEMU_FLAGS_NO_SDCARD) \
		-kernel $(KERNEL_ELF) \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -ne 0 ]; then \
		echo "_step35_smoke: QEMU failed status=$$status log=$$log"; \
		tail -n 120 "$$log"; \
		exit 1; \
	fi; \
	for marker in \
		'SCHED_STRESS: PASS' \
		'FUTEX_STRESS: PASS' \
		'FUTEX_STRESS: unrelated-wake-isolation PASS' \
		'FUTEX_STRESS: stale-timeout-isolation PASS' \
		'PROC_STRESS: PASS' \
		'PROC_STRESS: vfork-auto-reap PASS' \
		'PROC_STRESS: signal-stop-exit PASS' \
		'PROC_STRESS: signal-mask-park PASS' \
		'IO_EVENT_TEST: PASS' \
		'VFS_STRESS: PASS' \
		'SOCKET_STRESS: PASS' \
		'LIFETIME_STRESS: PASS' \
		'lifetime_errors: 0' \
		'System is going down for power-off NOW.'; do \
		if ! grep -q "$$marker" "$$log"; then \
			echo "_step35_smoke: missing '$$marker' log=$$log"; \
			tail -n 120 "$$log"; \
			exit 1; \
		fi; \
	done; \
	if [ "$(REQUIRE_TIMEOUT_CAPACITY)" = "1" ]; then \
		for marker in \
			'LIFETIME_STRESS: timeout-capacity-1 PASS' \
			'LIFETIME_STRESS: timeout-capacity PASS entries=' \
			'LIFETIME_STRESS: timeout-capacity+1 PASS' \
			'LIFETIME_STRESS: timeout-capacity PASS capacity='; do \
			if ! grep -q "$$marker" "$$log"; then \
				echo "_step35_smoke: missing '$$marker' log=$$log"; \
				tail -n 120 "$$log"; \
				exit 1; \
			fi; \
		done; \
	fi; \
	if [ "$(REQUIRE_SMP_RUNQUEUE)" = "1" ]; then \
		for marker in \
			'SCHED_STRESS: smp-runqueue PASS' \
			'scheduler_violations: 0'; do \
			if ! grep -q "$$marker" "$$log"; then \
				echo "_step35_smoke: missing '$$marker' log=$$log"; \
				tail -n 120 "$$log"; \
				exit 1; \
			fi; \
		done; \
	fi; \
	if [ "$(REQUIRE_LOCK_SPLIT)" = "1" ]; then \
		for marker in \
			'SCHED_STRESS: lock-split PASS' \
			'runqueue_local_picks:' \
			'runqueue_lock_acquires:' \
			'runqueue_parallel_pick_peak:' \
			'scheduler_violations: 0'; do \
			if ! grep -q "$$marker" "$$log"; then \
				echo "_step35_smoke: missing '$$marker' log=$$log"; \
				tail -n 120 "$$log"; \
				exit 1; \
			fi; \
		done; \
	fi; \
	if [ "$(NR_CPUS)" -gt 1 ] && \
	   ! grep -Fq '[SMP] $(NR_CPUS)/$(NR_CPUS) configured CPUs online' "$$log"; then \
		echo "_step35_smoke: not all $(NR_CPUS) CPUs came online log=$$log"; \
		tail -n 120 "$$log"; \
		exit 1; \
	fi; \
	if grep -Eq 'PANIC|sched invariant|reference underflow|use-after-free|\[LOCK\]' "$$log"; then \
		echo "_step35_smoke: lifecycle diagnostic failure log=$$log"; \
		grep -E 'PANIC|sched invariant|reference underflow|use-after-free|\[LOCK\]' "$$log" | head -n 20; \
		exit 1; \
	fi; \
	echo "_step35_smoke: PASS ($(STEP35_LABEL)); log saved to $$log"

check-proc-step35-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	step35-rv-debug-1c step35-la-debug-1c \
	step35-rv-release-8c step35-la-release-8c
	@$(PYTHON) tools/gates.py whitespace --label check-proc-step35-local
check-proc-step6-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary \
	step6-rv-debug-1c step6-la-debug-1c \
	step6-rv-release-8c step6-la-release-8c
	@$(PYTHON) tools/gates.py whitespace --label check-proc-step6-local
check-proc-step7-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary check-smp-runqueue-boundary \
	step7-rv-debug-1c step7-la-debug-1c \
	step7-rv-release-8c step7-la-release-8c
	@$(PYTHON) tools/gates.py whitespace --label check-proc-step7-local
check-proc-step8-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary check-smp-runqueue-boundary \
	check-process-lock-split-boundary \
	step8-rv-debug-1c step8-la-debug-1c \
	step8-rv-release-8c step8-la-release-8c
	@$(PYTHON) tools/gates.py whitespace --label check-proc-step8-local
check-proc-step35: check-proc-step35-local
	@echo "check-proc-step35: PASS"

check-proc-step6: check-proc-step6-local
	@echo "check-proc-step6: PASS"

check-proc-step7: check-proc-step7-local
	@echo "check-proc-step7: PASS"

check-proc-step8: check-proc-step8-local
	@echo "check-proc-step8: PASS"

check-proc-step4-local: check-blocking-point-boundary check-proc-step35-local
	@$(PYTHON) tools/gates.py whitespace --label check-proc-step4-local
check-proc-step4: check-proc-step4-local
	@echo "check-proc-step4: PASS"

check-proc-step5-local: check-signal-exit-boundary check-proc-step4-local
	@$(PYTHON) tools/gates.py whitespace --label check-proc-step5-local
check-proc-step5: check-proc-step5-local
	@echo "check-proc-step5: PASS"

smoke-socket-stress:
	$(PYTHON) tools/smoke.py smoke-socket-stress

smoke-driver-lifecycle:
	$(PYTHON) tools/smoke.py smoke-driver-lifecycle

smoke-hda:
	$(PYTHON) tools/smoke.py smoke-hda

smoke-audio-userspace:
	$(PYTHON) tools/smoke.py smoke-audio-userspace

smoke-usb-x86_64:
	$(PYTHON) tools/smoke.py smoke-usb-x86_64

smoke-virtio-sound:
	$(PYTHON) tools/smoke.py smoke-virtio-sound

smoke-pci-portability:
	$(PYTHON) tools/smoke.py smoke-pci-portability

smoke-native-handle:
	$(PYTHON) tools/smoke.py smoke-native-handle

# Thin wrappers: release configurations live in instances/release-*.toml.
release-rv:
	tools/a20 package release-riscv64

release-la:
	tools/a20 package release-loongarch64

_reset_obj:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	$(MAKE) -C user clean

_release_build: $(KERNEL_ELF) $(USER_BUILD_STAMP)
	$(MAKE) ARCH=$(ARCH) ABI=$(ABI) PROFILE=$(PROFILE) NR_CPUS=$(NR_CPUS) \
		EXTERNAL_ROOT=$(EXTERNAL_ROOT) _release_disk DISK_OUT=$(DISK_OUT)
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_release_disk: $(USER_BUILD_STAMP)
	@$(PYTHON) tools/img.py release-disk \
		--disk-out "$(DISK_OUT)" \
		--mkfs-fat "$(MKFS_FAT)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--libgcc "$(LIBGCC_S_ARCH)" \
		--protocols "$(PROTOCOLS_LINES)"

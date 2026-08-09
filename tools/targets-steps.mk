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
	{ sleep $(STEP35_INPUT_DELAY); \
	  printf 'lifetime_stress\ncat /proc/a20/task_lifetime\npoweroff\n'; } | \
	$(TIMEOUT) $(STEP35_TIMEOUT) $(QEMU) $(QEMU_FLAGS_NO_SDCARD) \
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
	@git diff --check
	@echo "check-proc-step35-local: PASS"

check-proc-step6-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary \
	step6-rv-debug-1c step6-la-debug-1c \
	step6-rv-release-8c step6-la-release-8c
	@git diff --check
	@echo "check-proc-step6-local: PASS"

check-proc-step7-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary check-smp-runqueue-boundary \
	step7-rv-debug-1c step7-la-debug-1c \
	step7-rv-release-8c step7-la-release-8c
	@git diff --check
	@echo "check-proc-step7-local: PASS"

check-proc-step8-local: check-task-state-boundary \
	check-concurrency-foundation check-task-lifetime-boundary \
	check-blocking-point-boundary check-signal-exit-boundary \
	check-timeout-ownership-boundary check-smp-runqueue-boundary \
	check-process-lock-split-boundary \
	step8-rv-debug-1c step8-la-debug-1c \
	step8-rv-release-8c step8-la-release-8c
	@git diff --check
	@echo "check-proc-step8-local: PASS"

check-proc-step35: check-proc-step35-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step35: PASS"

check-proc-step6: check-proc-step6-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step6: PASS"

check-proc-step7: check-proc-step7-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step7: PASS"

check-proc-step8: check-proc-step8-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step8: PASS"

check-proc-step4-local: check-blocking-point-boundary check-proc-step35-local
	@git diff --check
	@echo "check-proc-step4-local: PASS"

check-proc-step4: check-proc-step4-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step4: PASS"

check-proc-step5-local: check-signal-exit-boundary check-proc-step4-local
	@git diff --check
	@echo "check-proc-step5-local: PASS"

check-proc-step5: check-proc-step5-local \
	final-eval-rv-cagent final-eval-la-cagent
	@echo "check-proc-step5: PASS"

smoke-socket-stress:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/socket-stress-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'socket_stress\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SOCKET_STRESS: PASS' "$$log" && ! grep -q '\[LOCK\]' "$$log"; then \
		echo "smoke-socket-stress: PASS; log saved to $$log"; \
	elif grep -q '\[LOCK\]' "$$log"; then \
		echo "smoke-socket-stress: FAIL [LOCK] warning detected; log saved to $$log"; \
		grep '\[LOCK\]' "$$log" | head -n 5; \
		exit 1; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-socket-stress: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-socket-stress: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-driver-lifecycle:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=1 CONFIG_DRIVER_LIFECYCLE_TEST=y kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/driver-lifecycle-riscv64.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-bringup-driver-lifecycle/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'DRIVER_LIFECYCLE: PASS' "$$log"; then \
		echo "smoke-driver-lifecycle: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-driver-lifecycle: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-driver-lifecycle: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-hda:
	rm -f $(USER_BUILD_DIR)/hda.a20drv
	$(MAKE) ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both BRINGUP=0 \
		CONFIG_HDA_SMOKE_TEST=y DRVMOD_SMOKE=1 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/hda-x86_64.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-audiodev driver=none,id=audio0 \
		-device intel-hda -device hda-duplex,audiodev=audio0 \
		-drive file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev-hda-smoke/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev-hda-smoke/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'HDA_STREAM_SMOKE: PASS' "$$log" && \
	   grep -q "bound to driver 'hda'" "$$log" && \
	   grep -q '\[HDA\] driver registered in core: 0' "$$log" && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-hda: PASS; log saved to $$log"; \
	else \
		echo "smoke-hda: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-audio-userspace:
	$(MAKE) ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/audio-userspace-x86_64.log"; \
	wav="$(SMOKE_LOG_DIR)/audio-userspace-x86_64.wav"; \
	rm -f "$$wav"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/audioplay --tone 440 --duration 5000\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot -snapshot \
		-drive file=.kernel-build/x86_64-qemu-virt-x86_64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-audiodev driver=wav,id=audio0,path="$$wav" \
		-device intel-hda -device hda-duplex,audiodev=audio0 \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'audioplay: 440 Hz for 5000 ms -> /dev/audio' "$$log" && \
	   grep -q 'audioplay: playback complete' "$$log" && \
	   grep -q '\[HDA\] playback starts=1 underruns=0' "$$log" && \
	   grep -q "bound to driver 'hda'" "$$log" && \
	   grep -q 'System is going down for power-off NOW' "$$log" && \
	   ! grep -q 'audioplay: playback failed' "$$log" && \
	   ! grep -qi 'panic' "$$log" && \
	   python3 tools/check_wav_pcm.py --max-delta 1000 \
	       --min-frames 200000 "$$wav"; then \
		echo "smoke-audio-userspace: PASS; log=$$log wav=$$wav"; \
	else \
		echo "smoke-audio-userspace: failed with status $$status; tail of $$log:"; \
		tail -n 100 "$$log"; \
		exit 1; \
	fi

smoke-usb-x86_64:
	$(MAKE) ARCH=x86_64 ABI=both BRINGUP=0 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/usb-x86_64.log"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-device qemu-xhci,id=xhci \
		-device usb-kbd \
		-device usb-mouse \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '\[USB-HID\] keyboard ready' "$$log" && \
	   grep -q '\[USB-HID\] mouse ready' "$$log" && \
	   grep -q "bound to driver 'usb-hid'" "$$log" && \
	   ! grep -q '\[USB\] port.*enumeration failed' "$$log" && \
	   ! grep -q '\[XHCI\].*failed' "$$log"; then \
		echo "smoke-usb-x86_64: PASS; log saved to $$log"; \
	else \
		echo "smoke-usb-x86_64: failed with status $$status; tail of $$log:"; \
		tail -n 60 "$$log"; \
		exit 1; \
	fi

smoke-virtio-sound:
	$(MAKE) ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/virtio-sound-x86_64.log"; \
	wav="$(SMOKE_LOG_DIR)/virtio-sound-x86_64.wav"; \
	rm -f "$$wav"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/audioplay --tone 440 --duration 5000\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot -snapshot \
		-drive file=.kernel-build/x86_64-qemu-virt-x86_64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-audiodev driver=wav,id=audio0,path="$$wav" \
		-device virtio-sound-pci,audiodev=audio0 \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'audioplay: 440 Hz for 5000 ms -> /dev/audio' "$$log" && \
	   grep -q 'audioplay: playback complete' "$$log" && \
	   grep -q "bound to driver 'virtio-snd'" "$$log" && \
	   grep -q 'System is going down for power-off NOW' "$$log" && \
	   ! grep -q 'audioplay: playback failed' "$$log" && \
	   ! grep -qi 'panic' "$$log" && \
	   $(PYTHON) tools/check_wav_pcm.py --max-delta 1000 \
	       --min-frames 200000 "$$wav"; then \
		echo "smoke-virtio-sound: PASS; log=$$log wav=$$wav"; \
	else \
		echo "smoke-virtio-sound: failed with status $$status; tail of $$log:"; \
		tail -n 100 "$$log"; \
		exit 1; \
	fi

smoke-pci-portability:
	rm -f $(USER_BUILD_DIR)/nvme.a20drv
	$(MAKE) ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=both BRINGUP=0 \
		CONFIG_HDA_SMOKE_TEST=y DRVMOD_SMOKE=1 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/pci-portability-loongarch64.log"; \
	image="$(SMOKE_LOG_DIR)/pci-portability-nvme.img"; \
	truncate -s 128M "$$image"; \
	status=0; \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-loongarch64 \
		-machine virt -m 1G -nographic -smp 1 -no-reboot -snapshot \
		-audiodev driver=none,id=audio0 \
		-device intel-hda -device hda-duplex,audiodev=audio0 \
		-drive file="$$image",if=none,format=raw,id=nvme0 \
		-device nvme,drive=nvme0,serial=A20NVME \
		-drive file=.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev-hda-smoke/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-kernel .kernel-build/loongarch64-qemu-virt-loongarch64-both-dev-hda-smoke/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'HDA_STREAM_SMOKE: PASS' "$$log" && \
	   grep -q 'NVME_CAP_SMOKE: PASS' "$$log" && \
	   grep -q 'NVME_IO_SMOKE: PASS' "$$log" && \
	   grep -q "bound to driver 'hda'" "$$log" && \
	   grep -q "bound to driver 'nvme'" "$$log" && \
	   grep -q '\[NVME\] driver registered in core: 0' "$$log" && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-pci-portability: PASS; log saved to $$log"; \
	else \
		echo "smoke-pci-portability: failed with status $$status; tail of $$log:"; \
		tail -n 100 "$$log"; \
		exit 1; \
	fi

smoke-native-handle:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-handle-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-handle-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'part ok' "$$log" && grep -q 'tchan ok' "$$log" && \
	   grep -q 'bch ok' "$$log" && grep -q 'evq ok' "$$log" && \
	   grep -q 'opc ok' "$$log" && grep -q 'ac ok' "$$log" && \
	   grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-handle: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-handle: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

final-submit-rv:
	@echo "--- Building RISC-V 64 (2026 final submission) ---"
	$(MAKE) ARCH=riscv64 ABI=both MODE=release PROFILE=benchmark NR_CPUS=8 \
		_final_submit_build KERNEL_OUT=kernel-rv DISK_OUT=disk.img

final-submit-la:
	@echo "--- Building LoongArch 64 (2026 final submission) ---"
	$(MAKE) ARCH=loongarch64 ABI=both MODE=release PROFILE=benchmark NR_CPUS=8 \
		_final_submit_build KERNEL_OUT=kernel-la DISK_OUT=disk-la.img

contest-rv:
	@echo "--- Building RISC-V 64 (contest) ---"
	$(MAKE) ARCH=riscv64 _contest_build KERNEL_OUT=kernel-rv DISK_OUT=disk.img

contest-la:
	@echo "--- Building LoongArch 64 (contest) ---"
	$(MAKE) ARCH=loongarch64 _contest_build KERNEL_OUT=kernel-la DISK_OUT=disk-la.img

_reset_obj:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	$(MAKE) -C user clean

_contest_build: $(KERNEL_ELF) $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	$(MAKE) ARCH=$(ARCH) ABI=$(ABI) _contest_disk
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_contest_disk: $(USER_BUILD_STAMP) \
		user/contest_init/contest.sh \
		user/contest_init/run_ltp_resume.sh \
		user/contest_init/ltp_blacklist.txt
	rm -f $(DISK_OUT)
	$(MKFS_FAT) -C -F 32 $(DISK_OUT) 131072
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(DISK_OUT) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(DISK_OUT) $(USER_BUILD_DIR)/mksh ::/sh
	mcopy -o -i $(DISK_OUT) $(USER_BUILD_DIR)/mksh ::/bash
	-mmd -i $(DISK_OUT) ::/etc >/dev/null 2>&1
	-mmd -i $(DISK_OUT) ::/lib >/dev/null 2>&1
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(DISK_OUT) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(DISK_OUT) - ::/etc/protocols
	mcopy -o -i $(DISK_OUT) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt
	mcopy -o -i $(DISK_OUT) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(DISK_OUT) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	@printf 'auto\n' | mcopy -o -i $(DISK_OUT) - ::/etc/contest-mode

_final_submit_build: $(KERNEL_ELF) $(USER_BUILD_STAMP)
	$(MAKE) ARCH=$(ARCH) ABI=$(ABI) PROFILE=$(PROFILE) NR_CPUS=$(NR_CPUS) \
		_final_submit_disk DISK_OUT=$(DISK_OUT)
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_final_submit_disk: $(USER_BUILD_STAMP) user/contest_init/final_contest.sh
	rm -f $(DISK_OUT)
	$(MKFS_FAT) -C -F 32 $(DISK_OUT) 131072
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(DISK_OUT) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(DISK_OUT) $(USER_BUILD_DIR)/mksh ::/sh
	mcopy -o -i $(DISK_OUT) $(USER_BUILD_DIR)/mksh ::/bash
	-mmd -i $(DISK_OUT) ::/etc >/dev/null 2>&1
	-mmd -i $(DISK_OUT) ::/lib >/dev/null 2>&1
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(DISK_OUT) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(DISK_OUT) - ::/etc/protocols
	mcopy -o -i $(DISK_OUT) user/contest_init/final_contest.sh ::/final_contest.sh
	@printf 'all\n' | mcopy -o -i $(DISK_OUT) - ::/etc/final-eval-group

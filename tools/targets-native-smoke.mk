# ---- netd: userspace lwIP service (removed).
# The TCP/IP stack lives in the kernel (kernel/external/lwip + kernel/net);
# the user-space netd frame-ring/socket-proxy plane was abandoned.

native-rtcd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-rtcd-arch

$(NATIVE_REGISTRY_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_registry.c user/liba20rt/a20_registry.h user/svc/rtcd_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_registry.c,$@)

$(NATIVE_SVCMGR_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/svcmgr.c user/liba20rt/a20_registry.h user/svc/rtcd_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/svcmgr.c,$@)

native-registry-arch: $(NATIVE_REGISTRY_BIN) $(NATIVE_SVCMGR_BIN)

native-registry-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-registry-arch

$(NATIVE_ISOLATION_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_isolation.c user/svc/svc_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_isolation.c,$@)

native-isolation-arch: $(NATIVE_ISOLATION_BIN)

native-isolation-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-isolation-arch

$(NATIVE_UBDD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/ubd.c user/svc/ubd_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h kernel/include/drivers/driver_descriptor.h
	$(call NATIVE_RTCD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/ubd.c,$@)

$(NATIVE_UINPUTD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/uinputd.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		kernel/include/drivers/driver_descriptor.h kernel/include/drivers/dual/drv_env.h kernel/include/drivers/dual/virtio_mmio.h kernel/include/drivers/dual/virtio_input.h kernel/include/drivers/dual/virtq.h
	$(call NATIVE_RTCD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/uinputd.c,$@)

native-uinputd-arch: $(NATIVE_UINPUTD_BIN)

native-uinputd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-uinputd-arch

define NATIVE_PERSONALITY_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) -Iuser -Iuser/liba20rt -T$(NATIVE_LD) \
    $(3) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
    user/tests/test_native_personality.c $(NATIVE_LIBS) -o $(4)
endef

$(NATIVE_PERSONALITY_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		user/tests/test_native_personality.c user/liba20rt/a20_personality.h
	$(call NATIVE_PERSONALITY_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-personality-arch: $(NATIVE_PERSONALITY_BIN)

native-personality-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-personality-arch

define NATIVE_LINUX_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) -Iuser -Iuser/liba20rt -T$(NATIVE_LD) \
    $(3) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
    user/tests/test_native_linux.c $(NATIVE_LIBS) -o $(4)
endef

$(NATIVE_LINUX_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		user/tests/test_native_linux.c user/liba20rt/a20_linux.h user/liba20rt/a20_personality.h
	$(call NATIVE_LINUX_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-linux-arch: $(NATIVE_LINUX_BIN)

native-linux-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-linux-arch

native-ubd-arch: $(NATIVE_UBDD_BIN)

native-ubd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-ubd-arch

UBD_SCRATCH_IMG := $(BUILD_DIR)/ubd-scratch.img
UBD_SCRATCH_BIG := $(BUILD_DIR)/ubd-big.bin

$(UBD_SCRATCH_BIG): FORCE
	@rm -f $@; \
	dd if=/dev/zero of=$@ bs=1M count=4 2>/dev/null

$(UBD_SCRATCH_IMG): $(UBD_SCRATCH_BIG)
	@rm -f $@; \
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null; \
	$(MKFS_FAT) -F 32 $@; \
	printf 'A20OS-UBD-MARKER' | dd of=$@ bs=1 seek=4096 conv=notrunc 2>/dev/null; \
	mcopy -o -i $@ $(UBD_SCRATCH_BIG) ::/big.bin

smoke-dual-input:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/dual-input-riscv64.log"; \
	monsock="$(SMOKE_LOG_DIR)/dual-input-monitor.sock"; \
	rm -f "$$monsock"; \
	status=0; \
	{ sleep 8; python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); [(s.sendall(b"sendkey a\n"), time.sleep(1)) for _ in range(24)]; s.close()' "$$monsock" 2>/dev/null || true; } & \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
		-monitor unix:$$monsock,server,nowait \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'UINPUT] kernel-placement probe: id=18 version=2 name=QEMU Virtio Keyboard' "$$log" && \
	   grep -q 'UINPUTD: name=QEMU Virtio Keyboard' "$$log" && \
	   grep -q 'UINPUTD: ready' "$$log" && \
	   grep -q 'UINPUTD: ev type=1 code=30 value=1' "$$log" && \
	   grep -q 'UINPUTD: claimed' "$$log" && \
	   grep -q 'UINPUTD: PASS' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-dual-input: PASS; log saved to $$log"; \
	else \
		echo "smoke-dual-input: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-ubd:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 $(UBD_SCRATCH_IMG)
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-ubd-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/ubd_fs_test\npoweroff\n'; } | \
	$(TIMEOUT) 120s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-drive file=$(UBD_SCRATCH_IMG),if=none,format=raw,id=xubd \
		-device virtio-blk-device,drive=xubd,bus=virtio-mmio-bus.3 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'UBD_FS: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-ubd: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-ubd: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-isolation:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-isolation-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-isolation-rv\npoweroff\n'; } | \
	$(TIMEOUT) 90s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_ISOLATION: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-isolation: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-isolation: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-registry:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-registry-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/svcmgr-rv &\nsleep 1\n/bin/native-registry-rv\npoweroff\n'; } | \
	$(TIMEOUT) 60s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_REGISTRY: PASS' "$$log" && grep -q 'SVC_MGR: ready' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-registry: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-registry: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-rtcd:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-rtcd-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-rtcd-rv\npoweroff\n'; } | \
	$(TIMEOUT) 60s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_RTCD: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-rtcd: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-rtcd: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-shmring:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-shmring-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-shmring-rv\npoweroff\n'; } | \
	$(TIMEOUT) 60s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_SHMRING: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-shmring: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-shmring: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-clock-vdso:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/clock-vdso-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/clock_bench\npoweroff\n'; } | \
	$(TIMEOUT) 60s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'CLOCK_BENCH: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-clock-vdso: PASS; log saved to $$log"; \
	else \
		echo "smoke-clock-vdso: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-svc:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-svc-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/svcman-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_SVC: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-svc: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-svc: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-contract:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-contract-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-contract-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'ralg ok' "$$log" && grep -q 'bp ok' "$$log" && \
	   grep -q 'evqc ok' "$$log" && grep -q 'vmol ok' "$$log" && \
	   grep -q 'dma ok' "$$log" && \
	   grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-contract: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-contract: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-personality:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-personality-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-personality-rv\n/bin/pipe_ref\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_PERSONALITY: PASS' "$$log" && \
	   [ "$$(grep -c 'PIPE_REF: partial=6 rest=5 joined=hello world level=ok' "$$log")" = "2" ] && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-native-personality: PASS (native + Linux ABI reference agree); log saved to $$log"; \
	else \
		echo "smoke-native-personality: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
	fi

smoke-native-linux:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-linux-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-linux-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'linux fd ok' "$$log" && grep -q 'linux mmap ok' "$$log" && \
	   grep -q 'linux pipe ok' "$$log" && grep -q 'linux sockpair ok' "$$log" && \
	   grep -q 'linux futex ok' "$$log" && grep -q 'linux epoll ok' "$$log" && \
	   grep -q 'NATIVE_LINUX: PASS' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-native-linux: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-linux: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
	fi

smoke-native-ipc:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-ipc-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-ipc-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_IPC: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-ipc: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-ipc: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-signal:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-signal-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-signal-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_SIGNAL: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-signal: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-signal: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-mm:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-mm-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-mm-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_MM: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-mm: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-mm: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-futex:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-futex-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-futex-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_FUTEX: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-futex: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-futex: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-ext:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-ext-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-ext-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_EXT: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-ext: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-ext: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-native-debug:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-debug-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-debug-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_DEBUG: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-debug: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-debug: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi


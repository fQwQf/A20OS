# ----------------------------------------------------------------
# mlibc port (sysdeps/a20) — full libc on the native ABI
# ----------------------------------------------------------------
MLIBC_DIR      := user/external/mlibc
MLIBC_BUILD    := $(MLIBC_DIR)/build-a20-riscv64
MLIBC_SYSROOT  := user/build/mlibc-sysroot
MLIBC_HELLO_BIN := $(NATIVE_BUILD_DIR)/mlibc-hello-$(NATIVE_TAG)
MLIBC_CHILD_BIN := $(NATIVE_BUILD_DIR)/mlibc-child-$(NATIVE_TAG)
MLIBC_FAST_TYPE_FLAGS := \
	-D__INT_FAST8_TYPE__="signed char" -D__INT_FAST16_TYPE__=long -D__INT_FAST32_TYPE__=long \
	-D__UINT_FAST8_TYPE__="unsigned char" -D__UINT_FAST16_TYPE__="unsigned long" -D__UINT_FAST32_TYPE__="unsigned long"

$(MLIBC_SYSROOT)/lib/libc.a: $(wildcard $(MLIBC_DIR)/sysdeps/a20/*) \
	$(MLIBC_DIR)/options/posix/include/spawn.h \
	$(MLIBC_DIR)/ci/a20-riscv64.cross-file
	@test -d "$(MLIBC_BUILD)" || meson setup $(MLIBC_BUILD) $(MLIBC_DIR) \
		--cross-file $(MLIBC_DIR)/ci/a20-riscv64.cross-file \
		-Ddefault_library=static -Dbuild_tests=false --prefix=$(abspath $(MLIBC_SYSROOT))
	ninja -C $(MLIBC_BUILD)
	meson install -C $(MLIBC_BUILD) --no-rebuild --quiet

mlibc-sysroot: $(MLIBC_SYSROOT)/lib/libc.a

define MLIBC_LINK_RECIPE
@mkdir -p $(dir $(3))
$(RISCV_ELF_PREFIX)gcc -march=rv64gc -mabi=lp64d -mcmodel=medany -static -nostdlib \
	-D_GNU_SOURCE $(MLIBC_FAST_TYPE_FLAGS) \
	-isystem $(MLIBC_SYSROOT)/include \
	-T user/mlibc/a20-mlibc.ld \
	$(MLIBC_SYSROOT)/lib/crt1.o $(MLIBC_SYSROOT)/lib/a20_thread_entry.o \
	$(1) \
	-L$(MLIBC_SYSROOT)/lib $(2) -lgcc \
	-o $(3)
endef

$(MLIBC_HELLO_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_hello.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_hello.c,-lc -lpthread -lm -lrt,$@)

$(MLIBC_CHILD_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_child.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_child.c,-lc,$@)

mlibc-hello-rv: $(MLIBC_HELLO_BIN) $(MLIBC_CHILD_BIN)

smoke-mlibc:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	$(MAKE) ARCH=riscv64 NOMMU=0 mlibc-hello-rv
	mcopy -o -i $(FAT32_IMG) $(MLIBC_HELLO_BIN) ::/mlibc-hello-rv
	mcopy -o -i $(FAT32_IMG) $(MLIBC_CHILD_BIN) ::/mlibc-child-rv
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/mlibc-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/mlibc-hello-rv\npoweroff\n'; } | \
	$(TIMEOUT) 40s qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=$(FAT32_IMG),if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'MLIBC_A20: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-mlibc: PASS; log saved to $$log"; \
	else \
		echo "smoke-mlibc: failed with status $$status; tail of $$log:"; \
		tail -n 120 "$$log"; \
		exit 1; \
	fi

smoke-native-libc:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/native-libc-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '/bin/native-libc-rv\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NATIVE_LIBC: PASS' "$$log" && grep -q 'System is going down for power-off NOW' "$$log"; then \
		echo "smoke-native-libc: PASS; log saved to $$log"; \
	else \
		echo "smoke-native-libc: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi


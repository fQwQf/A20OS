ifneq ($(ARCH),aarch64)
$(error BOARD=raspberrypi4 requires ARCH=aarch64)
endif
ifeq ($(filter $(NR_CPUS),1 2 3 4),)
$(error BOARD=raspberrypi4 requires NR_CPUS=1..4)
endif
ifeq ($(NOMMU),1)
$(error BOARD=raspberrypi4 currently requires MMU)
endif
ifeq ($(COOPERATIVE_BOOT),1)
$(error BOARD=raspberrypi4 requires architectural timer IRQs)
endif

# Fixed QEMU raspi4b model: 2 GiB, four physical cores even when the kernel
# capacity is one. Image handoff supplies a firmware DTB in x0.
QEMU_FLAGS_BASE_raspberrypi4 := -machine raspi4b
QEMU_MACHINE_CPUS := 4
ifeq ($(origin QEMU_MEMORY),file)
QEMU_MEMORY := 2G
endif
QEMU_KERNEL = $(KERNEL_BIN)
QEMU_BOOT_FLAGS = -dtb $(BUILD_DIR)/qemu-fixture.dtb

$(BUILD_DIR)/qemu-fixture.dtb: tools/raspberrypi4_fixture.py
	@mkdir -p $(dir $@)
	$(PYTHON) $< --output $@ $(if $(filter-out 1,$(NR_CPUS)),--selftest,)

kernel-only: $(BUILD_DIR)/qemu-fixture.dtb

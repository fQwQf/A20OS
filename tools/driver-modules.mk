# Loadable driver package rules for the generic deployment profile.
#
# All packages are copied into /lib/drivers by the FAT32 image rule.  The
# driver manager owns discovery and activation; this file only builds the
# packages and their smoke targets.

ifeq ($(DRIVER_DEPLOYMENT),generic)

DRVMOD_DIR := kernel/drvmod/examples
ifeq ($(ARCH),riscv64)
DRVMOD_GCC := $(CCACHE_PREFIX)$(RISCV_ELF_PREFIX)gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -mcmodel=medany -fPIC -mno-relax \
                 -march=rv64g -mabi=lp64d -DCONFIG_RISCV64 -Ikernel/arch/riscv64/include -Ikernel/include -Ikernel
DRVMOD_MODULES := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv dw-sdio.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv vinput-probe.a20drv vinput.a20drv hda.a20drv
else ifeq ($(ARCH),x86_64)
DRVMOD_GCC := $(CCACHE_PREFIX)x86_64-linux-gnu-gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -mno-red-zone -fno-pic -fno-pie \
                 -mcmodel=large -DCONFIG_X86_64 -Ikernel/arch/x86_64/include -Ikernel/include -Ikernel
DRVMOD_MODULES := pc-spkr.a20drv virtio-blk.a20drv virtio-scsi.a20drv ahci.a20drv ps2.a20drv tpm.a20drv nvme.a20drv e1000.a20drv vmsvga.a20drv xhci.a20drv usb-hid.a20drv usb-storage.a20drv hda.a20drv vinput.a20drv
else ifeq ($(ARCH),aarch64)
DRVMOD_GCC := $(CCACHE_PREFIX)aarch64-linux-gnu-gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -fno-pic -mcmodel=large \
                 -mno-outline-atomics \
                 -DCONFIG_AARCH64 -DCONFIG_NR_CPUS=1 -Ikernel/arch/aarch64/include -Ikernel/include -Ikernel
DRVMOD_MODULES := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv xhci.a20drv usb-hid.a20drv usb-storage.a20drv vinput-probe.a20drv vinput.a20drv hda.a20drv
else ifeq ($(ARCH),loongarch64)
DRVMOD_GCC := $(CCACHE_PREFIX)loongarch64-linux-gnu-gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -fno-pic -mcmodel=medium \
                 -DCONFIG_LOONGARCH64 -Ikernel/arch/loongarch64/include -Ikernel/include -Ikernel
DRVMOD_MODULES := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv nvme.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv xhci.a20drv usb-hid.a20drv usb-storage.a20drv vinput.a20drv hda.a20drv
else
DRVMOD_MODULES :=
endif
DRVMOD_CFLAGS += -std=gnu99

$(addprefix $(USER_BUILD_DIR)/,$(DRVMOD_MODULES)): tools/driver-modules.mk

# Early DriverStore packages: linked into the kernel root ramfs and loaded
# before the real root disk is mounted.  Every driver that can own the root
# block device (virtio-blk, virtio-scsi, AHCI, dw-sdio) plus the base RTC
# must be present here; anything else may live in the Runtime DriverStore.
ifeq ($(DRIVER_DEPLOYMENT),generic)
ifeq ($(ARCH),x86_64)
EARLY_DRVMOD_MODULES := pc-spkr.a20drv virtio-blk.a20drv virtio-scsi.a20drv ahci.a20drv
EARLY_DRIVER_BFD := elf64-x86-64 -B i386:x86-64
else ifneq ($(filter $(ARCH),riscv64 aarch64 loongarch64),)
EARLY_DRVMOD_MODULES := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv
ifeq ($(ARCH),riscv64)
EARLY_DRVMOD_MODULES += dw-sdio.a20drv
endif
EARLY_DRIVER_BFD_riscv64 := elf64-littleriscv -B riscv:rv64
EARLY_DRIVER_BFD_aarch64 := elf64-littleaarch64 -B aarch64
EARLY_DRIVER_BFD_loongarch64 := elf64-loongarch -B loongarch
EARLY_DRIVER_BFD := $(EARLY_DRIVER_BFD_$(ARCH))
else
EARLY_DRVMOD_MODULES :=
endif
else
EARLY_DRVMOD_MODULES :=
endif
EARLY_DRIVER_BLOBS := $(addprefix $(BUILD_DIR)/rootfs-drivers/,$(EARLY_DRVMOD_MODULES:.a20drv=.o))
RUNTIME_DRVMOD_MODULES := $(filter-out $(EARLY_DRVMOD_MODULES),$(DRVMOD_MODULES))

$(BUILD_DIR)/rootfs-drivers/%.o: $(USER_BUILD_DIR)/%.a20drv
	@mkdir -p $(dir $@)
	cd $(USER_BUILD_DIR) && $(OBJCOPY) -I binary -O $(word 1,$(EARLY_DRIVER_BFD)) -B $(word 3,$(EARLY_DRIVER_BFD)) \
		--rename-section .data=.rodata.rootfs_drivers,alloc,load,readonly,data,contents $*.a20drv $(abspath $@)

$(USER_BUILD_DIR)/rtc.a20drv: $(DRVMOD_DIR)/goldfish_rtc.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/pc-spkr.a20drv: $(DRVMOD_DIR)/pc_spkr.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/vinput.a20drv: $(DRVMOD_DIR)/vinput.c \
		kernel/include/drvmod/drvmod.h kernel/include/drivers/input/virtio_input.h \
		kernel/include/drivers/dual/virtio_mmio.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/hda.a20drv: $(DRVMOD_DIR)/hda.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) $(if $(filter 1,$(DRVMOD_SMOKE)),-DCONFIG_HDA_SMOKE_TEST,) -c $< -o $@

$(USER_BUILD_DIR)/nvme.a20drv: $(DRVMOD_DIR)/nvme.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) $(if $(filter 1,$(DRVMOD_SMOKE)),-DCONFIG_NVME_SMOKE_TEST,) -c $< -o $@

$(USER_BUILD_DIR)/tpm.a20drv: $(DRVMOD_DIR)/tpm.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/ps2.a20drv: $(DRVMOD_DIR)/ps2.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/vinput-probe.a20drv: $(DRVMOD_DIR)/vinput_probe.c \
		kernel/include/drvmod/drvmod.h kernel/include/drivers/dual/drv_env.h \
		kernel/include/drivers/dual/virtio_input.h kernel/include/drivers/dual/virtio_mmio.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/virtio-blk.a20drv: $(DRVMOD_DIR)/virtio_blk.c \
		kernel/drivers/block/virtio_blk.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/virtio-scsi.a20drv: $(DRVMOD_DIR)/virtio_scsi.c \
		kernel/drivers/block/virtio_scsi.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/ahci.a20drv: $(DRVMOD_DIR)/ahci.c \
		kernel/drivers/block/ahci.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -DCONFIG_AHCI -c $< -o $@

$(USER_BUILD_DIR)/dw-sdio.a20drv: $(DRVMOD_DIR)/dw_sdio.c \
		kernel/drivers/block/dw_sdio.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/virtio-net.a20drv: $(DRVMOD_DIR)/virtio_net.c kernel/drivers/net/virtio_net.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/virtio-gpu.a20drv: $(DRVMOD_DIR)/virtio_gpu.c kernel/drivers/gpu/virtio_gpu.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/vmsvga.a20drv: $(DRVMOD_DIR)/vmsvga.c kernel/drivers/gpu/vmsvga.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/virtio-snd.a20drv: $(DRVMOD_DIR)/virtio_snd.c kernel/drivers/audio/virtio_snd.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/e1000.a20drv: $(DRVMOD_DIR)/e1000.c kernel/drivers/net/e1000.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/xhci.a20drv: $(DRVMOD_DIR)/xhci.c kernel/drivers/usb/host/xhci.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/usb-hid.a20drv: $(DRVMOD_DIR)/usb_hid.c kernel/drivers/usb/class/usb_hid.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/usb-storage.a20drv: $(DRVMOD_DIR)/usb_storage.c kernel/drivers/usb/class/usb_storage.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

virtio-blk-module: $(USER_BUILD_DIR)/virtio-blk.a20drv

drvmod-examples: $(addprefix $(USER_BUILD_DIR)/,$(DRVMOD_MODULES))

$(FAT32_IMG): drvmod-examples $(addprefix $(USER_BUILD_DIR)/,$(RUNTIME_DRVMOD_MODULES))

DRIVER_STORE_USER_PACKAGES = $(notdir $(NATIVE_RTCDD_BIN)) \
                             $(notdir $(NATIVE_UBDD_BIN)) \
                             $(notdir $(NATIVE_UINPUTD_BIN))

smoke-drvmod-riscv64:
	$(MAKE) ARCH=riscv64 ABI=both BRINGUP=0 DRIVER_DEPLOYMENT=generic dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/drvmod-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'drvctl list\nsyscall_smoke\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '\[GOLDFISH-RTC\] probe ok' "$$log" && \
	   grep -q 'SYSCALL_SMOKE: PASS' "$$log" && \
	   grep -q 'rtc.a20drv' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-drvmod-riscv64: PASS (rtc.a20drv loaded and bound); log saved to $$log"; \
	else \
		echo "smoke-drvmod-riscv64: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
	fi

smoke-drvmod-aarch64:
	$(MAKE) ARCH=aarch64 ABI=both BRINGUP=0 DRIVER_DEPLOYMENT=generic dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/drvmod-aarch64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-aarch64 \
		-machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/aarch64-qemu-virt-aarch64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-kernel .kernel-build/aarch64-qemu-virt-aarch64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '\[GOLDFISH-RTC\] probe ok' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-drvmod-aarch64: PASS (rtc.a20drv loaded and bound); log saved to $$log"; \
	else \
		echo "smoke-drvmod-aarch64: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
	fi

smoke-drvmod-loongarch64:
	$(MAKE) ARCH=loongarch64 ABI=both BRINGUP=0 DRIVER_DEPLOYMENT=generic dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/drvmod-loongarch64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-loongarch64 \
		-machine virt -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-kernel .kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '\[GOLDFISH-RTC\] probe ok' "$$log" && \
	   grep -q 'System is going down for power-off' "$$log"; then \
		echo "smoke-drvmod-loongarch64: PASS (rtc.a20drv loaded and bound); log saved to $$log"; \
	else \
		echo "smoke-drvmod-loongarch64: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
	fi

smoke-drvmod-x86_64:
	$(MAKE) ARCH=x86_64 ABI=both BRINGUP=0 DRIVER_DEPLOYMENT=generic dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/drvmod-x86_64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'drvctl list\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-drive file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '\[PC-SPKR\] driver registered in core: 0' "$$log" && \
	   grep -q "device 'pc-speaker' bound to driver 'pc-speaker'" "$$log" && \
	   grep -q '\[PS2\] module init ok' "$$log"; then \
		echo "smoke-drvmod-x86_64: PASS (pc-spkr.a20drv + ps2.a20drv loaded, registered, bound); log saved to $$log"; \
	else \
		echo "smoke-drvmod-x86_64: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; exit 1; \
	fi

smoke-drvmod: smoke-drvmod-riscv64 smoke-drvmod-x86_64 smoke-drvmod-aarch64 smoke-drvmod-loongarch64

else

DRVMOD_MODULES :=
DRIVER_STORE_USER_PACKAGES :=

drvmod-examples:
	@echo "drvmod-examples: skipped (DRIVER_DEPLOYMENT=embedded)"

smoke-drvmod-riscv64 smoke-drvmod-aarch64 smoke-drvmod-loongarch64 smoke-drvmod-x86_64 smoke-drvmod:
	@echo "driver module smokes require DRIVER_DEPLOYMENT=generic"
	@false

endif

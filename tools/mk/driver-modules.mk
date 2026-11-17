# Loadable driver package rules for the generic deployment profile.
#
# All packages are copied into /lib/drivers by the FAT32 image rule.  The
# driver manager owns discovery and activation; this file only builds the
# packages and their smoke targets.

ifeq ($(DRIVER_DEPLOYMENT),generic)

# DRVMOD_MODULES / EARLY_DRVMOD_MODULES are generated from
# components/drivers.toml -- the only place a driver is declared.  Regenerate
# with `make regen-driver-fragment` after editing the TOML; `make
# check-component-registry` fails if the generated file is stale.
include components/drivers.mk

# BusyBox kernels keep driver objects independent of source-built userspace.
DRVMOD_BUILD_DIR := $(if $(filter 1,$(RAMFS_USER)),$(BUILD_DIR)/driver-modules,$(USER_BUILD_DIR))
DRVMOD_USER_BUILD_PREREQ := $(if $(filter 1,$(RAMFS_USER)),,$(USER_BUILD_STAMP))

DRVMOD_DIR := kernel/drvmod/examples
ifeq ($(ARCH),riscv64)
DRVMOD_GCC := $(CCACHE_PREFIX)$(RISCV_ELF_PREFIX)gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -mcmodel=medany -fPIC -mno-relax \
                 -march=rv64g -mabi=lp64d -DCONFIG_RISCV64 -Ikernel/arch/riscv64/include -Ikernel/include -Ikernel
else ifeq ($(ARCH),x86_64)
DRVMOD_GCC := $(CCACHE_PREFIX)x86_64-linux-gnu-gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -mno-red-zone \
                 -mgeneral-regs-only -fno-pic -fno-pie -mcmodel=large -DCONFIG_X86_64 -Ikernel/arch/x86_64/include -Ikernel/include -Ikernel
else ifeq ($(ARCH),aarch64)
DRVMOD_GCC := $(CCACHE_PREFIX)aarch64-linux-gnu-gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -fno-pic -mcmodel=large \
                 -mno-outline-atomics \
                 -DCONFIG_AARCH64 -DCONFIG_NR_CPUS=1 -Ikernel/arch/aarch64/include -Ikernel/include -Ikernel
else ifeq ($(ARCH),loongarch64)
DRVMOD_GCC := $(CCACHE_PREFIX)loongarch64-linux-gnu-gcc
DRVMOD_CFLAGS := -ffreestanding -nostdlib -fno-pic -mcmodel=medium \
                 -DCONFIG_LOONGARCH64 -Ikernel/arch/loongarch64/include -Ikernel/include -Ikernel
endif
ifeq ($(TOOLCHAIN),llvm)
DRVMOD_GCC := $(LLVM_CC) $(LLVM_LINK)
DRVMOD_CFLAGS += -Wno-typedef-redefinition
endif
DRVMOD_CFLAGS += -std=gnu99
ifeq ($(ARCH),x86_64)
DRVMOD_CFLAGS += -DCONFIG_NR_CPUS=$(NR_CPUS)
endif
ifeq ($(STORAGE_READ_ONLY),1)
DRVMOD_CFLAGS += -DCONFIG_STORAGE_READ_ONLY
endif
ifeq ($(QEMU_SHARED_SMP),1)
DRVMOD_CFLAGS += -DCONFIG_SHARED_DEVICE_CALL -DCONFIG_SHARED_MMIO_OWNER=0
endif
# A drvmod blob embeds a driver_t, so it shares the kernel's struct layout.  These
# rules list their headers by hand, and any header left off the list turns a
# driver_t or device_t change into a silent ABI mismatch: the blob keeps the old
# offsets while the kernel reads the new ones, and the failure surfaces far away
# as a driver registering with the wrong class.  Generate the real dependency
# list instead of maintaining it by hand.
DRVMOD_CFLAGS += -MMD -MP -DCONFIG_DRIVER_MODULE
ifneq ($(filter $(ARCH),riscv64 x86_64 aarch64 loongarch64),)
DRVMOD_CFLAGS += -DCONFIG_64BIT
endif

# Consume the dependency lists those flags generate.  -MP keeps a rule buildable
# after its header is deleted, and -include (rather than a wildcard prerequisite)
# is what lets a freshly generated .d file take effect on the next run.
-include $(wildcard $(DRVMOD_BUILD_DIR)/*.d)

# The user build and generic driver packages share USER_BUILD_DIR.  On a fresh
# parallel build, USER_BUILD_STAMP may run `make -C user clean` while a driver
# compiler is writing its output, removing the directory after the driver's
# mkdir and making the compile fail with ENOENT.  Finish the user build (and
# any required clean) before allowing driver package recipes to enter the
# shared directory.  This must be a normal prerequisite: when a profile change
# rebuilds USER_BUILD_STAMP, its clean step can remove driver files that Make
# had already found during dependency discovery.  A normal dependency makes
# those packages stale and rebuilds them after the clean finishes.
# Ordinary kernels share user/build/<arch> across CPU and driver profiles.
# A changed inline CPU helper or smoke/read-only flag must rebuild the packages
# even when their C sources and the userspace build identity are unchanged.
# Keep one current stamp so switching A -> B -> A also invalidates B's objects.
drvmod_shellquote = '$(subst ','"'"',$(1))'
DRVMOD_BUILD_FLAGS := $(DRVMOD_BUILD_DIR)/.driver-build-flags
.PHONY: FORCE_DRVMOD_FLAGS
FORCE_DRVMOD_FLAGS:
$(DRVMOD_BUILD_FLAGS): FORCE_DRVMOD_FLAGS $(DRVMOD_USER_BUILD_PREREQ)
	@mkdir -p $(dir $@)
	@printf '%s\n' $(call drvmod_shellquote,$(DRVMOD_GCC)) $(call drvmod_shellquote,$(DRVMOD_CFLAGS)) $(call drvmod_shellquote,DRVMOD_SMOKE=$(DRVMOD_SMOKE)) > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi
$(addprefix $(DRVMOD_BUILD_DIR)/,$(DRVMOD_MODULES)): tools/mk/driver-modules.mk $(DRVMOD_USER_BUILD_PREREQ) $(DRVMOD_BUILD_FLAGS)

# Early DriverStore packages: linked into the kernel root ramfs and loaded
# before the real root disk is mounted.  Every driver that can own the root
# block device (virtio-blk, virtio-scsi, AHCI, dw-sdio) plus the base RTC
# must be present here; anything else may live in the Runtime DriverStore.
ifeq ($(DRIVER_DEPLOYMENT),generic)
ifeq ($(ARCH),x86_64)
EARLY_DRIVER_BFD := elf64-x86-64 -B i386:x86-64
else ifneq ($(filter $(ARCH),riscv64 aarch64 loongarch64),)
EARLY_DRIVER_BFD_riscv64 := elf64-littleriscv -B riscv:rv64
EARLY_DRIVER_BFD_aarch64 := elf64-littleaarch64 -B aarch64
EARLY_DRIVER_BFD_loongarch64 := elf64-loongarch -B loongarch
EARLY_DRIVER_BFD := $(EARLY_DRIVER_BFD_$(ARCH))
endif
endif
EARLY_DRIVER_BLOBS := $(addprefix $(BUILD_DIR)/rootfs-drivers/,$(EARLY_DRVMOD_MODULES:.a20drv=.o))
RUNTIME_DRVMOD_MODULES := $(filter-out $(EARLY_DRVMOD_MODULES),$(DRVMOD_MODULES))

# Optional instance-driven runtime driver selection (tools/a20
# [rootfs].drivers; see docs/zh/dev/instances.md).  Restricts which packages land in
# /lib/drivers; early DriverStore members are always built and embedded.
# a20 validates the names against components/drivers.toml before make runs.
ifneq ($(strip $(DRIVER_SELECTION)),)
RUNTIME_DRVMOD_MODULES := $(DRIVER_SELECTION)
endif

$(BUILD_DIR)/rootfs-drivers/%.o: $(DRVMOD_BUILD_DIR)/%.a20drv
	@mkdir -p $(dir $@)
	cd $(DRVMOD_BUILD_DIR) && $(OBJCOPY) -I binary -O $(word 1,$(EARLY_DRIVER_BFD)) -B $(word 3,$(EARLY_DRIVER_BFD)) \
		--rename-section .data=.rodata.rootfs_drivers,alloc,load,readonly,data,contents $*.a20drv $(abspath $@)

$(DRVMOD_BUILD_DIR)/rtc.a20drv: $(DRVMOD_DIR)/goldfish_rtc.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/abi-probe.a20drv: $(DRVMOD_DIR)/abi_probe.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/pc-spkr.a20drv: $(DRVMOD_DIR)/pc_spkr.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/cmos-rtc.a20drv: $(DRVMOD_DIR)/cmos_rtc.c \
		kernel/include/drvmod/drvmod.h kernel/include/drivers/char/cmos_rtc.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/vinput.a20drv: $(DRVMOD_DIR)/vinput.c \
		kernel/include/drvmod/drvmod.h kernel/include/drivers/input/virtio_input.h \
		kernel/include/drivers/dual/virtio_mmio.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/hda.a20drv: $(DRVMOD_DIR)/hda.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) $(if $(filter 1,$(DRVMOD_SMOKE)),-DCONFIG_HDA_SMOKE_TEST,) -c $< -o $@

$(DRVMOD_BUILD_DIR)/nvme.a20drv: $(DRVMOD_DIR)/nvme.c kernel/drivers/block/nvme.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) $(if $(filter 1,$(DRVMOD_SMOKE)),-DCONFIG_NVME_SMOKE_TEST,) -c $< -o $@

$(DRVMOD_BUILD_DIR)/tpm.a20drv: $(DRVMOD_DIR)/tpm.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/ps2.a20drv: $(DRVMOD_DIR)/ps2.c kernel/drivers/input/ps2.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/vinput-probe.a20drv: $(DRVMOD_DIR)/vinput_probe.c \
		kernel/include/drvmod/drvmod.h kernel/include/drivers/dual/drv_env.h \
		kernel/include/drivers/dual/virtio_input.h kernel/include/drivers/dual/virtio_mmio.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/virtio-blk.a20drv: $(DRVMOD_DIR)/virtio_blk.c \
		kernel/drivers/block/virtio_blk.c kernel/include/drvmod/drvmod.h \
		kernel/include/drivers/block/virtio_blk.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/virtio-scsi.a20drv: $(DRVMOD_DIR)/virtio_scsi.c \
		kernel/drivers/block/virtio_scsi.c kernel/include/drvmod/drvmod.h \
		kernel/include/drivers/block/virtio_blk.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/ahci.a20drv: $(DRVMOD_DIR)/ahci.c \
		kernel/drivers/block/ahci.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -DCONFIG_AHCI -c $< -o $@

$(DRVMOD_BUILD_DIR)/dw-sdio.a20drv: $(DRVMOD_DIR)/dw_sdio.c \
		kernel/drivers/block/dw_sdio.c kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/sunxi-mmc.a20drv: $(DRVMOD_DIR)/sunxi_mmc.c \
		kernel/drivers/block/sunxi_mmc.c kernel/drivers/block/sunxi_mmc_core.c \
		kernel/include/drivers/block/sunxi_mmc.h kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/sdhci.a20drv: $(DRVMOD_DIR)/sdhci.c \
		kernel/drivers/block/sdhci.c kernel/drivers/block/sdhci_core.c \
		kernel/include/drivers/block/sdhci.h kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/virtio-net.a20drv: $(DRVMOD_DIR)/virtio_net.c \
		kernel/drivers/net/virtio_net.c kernel/include/drivers/block/virtio_blk.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/virtio-gpu.a20drv: $(DRVMOD_DIR)/virtio_gpu.c \
		kernel/drivers/gpu/virtio_gpu.c kernel/include/drivers/block/virtio_blk.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/vmsvga.a20drv: $(DRVMOD_DIR)/vmsvga.c kernel/drivers/gpu/vmsvga.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/virtio-snd.a20drv: $(DRVMOD_DIR)/virtio_snd.c \
		kernel/drivers/audio/virtio_snd.c kernel/include/drivers/block/virtio_blk.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/sunxi-codec.a20drv: $(DRVMOD_DIR)/sunxi_audio.c \
		kernel/drivers/audio/sunxi_audio.c kernel/drivers/audio/pcm_period_ring.h \
		kernel/include/drivers/audio/sunxi_audio.h kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/rk817-codec.a20drv: $(DRVMOD_DIR)/rk817_audio.c \
		kernel/drivers/audio/rk817_audio.c kernel/drivers/audio/pcm_period_ring.h \
		kernel/drivers/dma/pl330_audio_program.h \
		kernel/include/drivers/audio/rk817_audio.h kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/sg2002-audio.a20drv: $(DRVMOD_DIR)/sg2002_audio.c \
		kernel/drivers/audio/sg2002_audio.c kernel/drivers/audio/sg2002_audio_hw.h \
		kernel/drivers/audio/pcm_period_ring.h \
		kernel/include/drivers/audio/sg2002_audio.h kernel/include/drvmod/drvmod.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

# These settings affect only this driver, not the common userland build ID.
# Recheck the header on each build, but change its timestamp only when the
# resolved values change. Switching presets (including back to defaults)
# therefore rebuilds the module and its dependent images without make clean.
VIRTIO_CONSOLE_PROFILE ?= throughput
VPORT_CONFIG_HEADER := $(DRVMOD_BUILD_DIR)/.virtio-console-config.h
vport_shellquote = '$(subst ','"'"',$(1))'
VPORT_CONFIG_ARGS = --profile $(call vport_shellquote,$(VIRTIO_CONSOLE_PROFILE)) \
    $(if $(strip $(VIRTIO_CONSOLE_RX_SLOTS)),--rx-slots $(call vport_shellquote,$(VIRTIO_CONSOLE_RX_SLOTS))) \
    $(if $(strip $(VIRTIO_CONSOLE_TX_SLOTS)),--tx-slots $(call vport_shellquote,$(VIRTIO_CONSOLE_TX_SLOTS))) \
    $(if $(strip $(VIRTIO_CONSOLE_BUFFER_SIZE)),--buffer-size $(call vport_shellquote,$(VIRTIO_CONSOLE_BUFFER_SIZE))) \
    $(if $(strip $(VIRTIO_CONSOLE_RING_SIZE)),--ring-size $(call vport_shellquote,$(VIRTIO_CONSOLE_RING_SIZE)))
.PHONY: FORCE_VPORT_CONFIG
FORCE_VPORT_CONFIG:
$(VPORT_CONFIG_HEADER): tools/virtio_console_config.py $(DRVMOD_USER_BUILD_PREREQ) FORCE_VPORT_CONFIG
	@$(PYTHON) tools/virtio_console_config.py $(VPORT_CONFIG_ARGS) --output $(call vport_shellquote,$@)

$(DRVMOD_BUILD_DIR)/virtio-console.a20drv: $(DRVMOD_DIR)/virtio_console.c $(VPORT_CONFIG_HEADER) \
		kernel/drivers/char/virtio_console.c \
		kernel/include/drivers/char/virtio_console.h
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -include $(call vport_shellquote,$(VPORT_CONFIG_HEADER)) -c $< -o $@

$(DRVMOD_BUILD_DIR)/virtio-rng.a20drv: $(DRVMOD_DIR)/virtio_rng.c \
		kernel/drivers/char/virtio_rng.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/e1000.a20drv: $(DRVMOD_DIR)/e1000.c kernel/drivers/net/e1000.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/r8169.a20drv: $(DRVMOD_DIR)/r8169.c kernel/drivers/net/r8169.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/rtl8139.a20drv: $(DRVMOD_DIR)/rtl8139.c kernel/drivers/net/rtl8139.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/xhci.a20drv: $(DRVMOD_DIR)/xhci.c kernel/drivers/usb/host/xhci.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/usb-hid.a20drv: $(DRVMOD_DIR)/usb_hid.c kernel/drivers/usb/class/usb_hid.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/usb-hub.a20drv: $(DRVMOD_DIR)/usb_hub.c kernel/drivers/usb/class/usb_hub.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

$(DRVMOD_BUILD_DIR)/usb-storage.a20drv: $(DRVMOD_DIR)/usb_storage.c kernel/drivers/usb/class/usb_storage.c
	@mkdir -p $(dir $@)
	$(DRVMOD_GCC) $(DRVMOD_CFLAGS) -c $< -o $@

.PHONY: virtio-blk-module drvmod-examples check-x86-drvmod-isa

virtio-blk-module: $(DRVMOD_BUILD_DIR)/virtio-blk.a20drv

ifeq ($(ARCH),x86_64)
X86_DRVMOD_ELFS := $(addprefix $(DRVMOD_BUILD_DIR)/,$(DRVMOD_MODULES))
check-x86-drvmod-isa: $(X86_DRVMOD_ELFS) tools/check_x86_drvmod_isa.py
	$(PYTHON) tools/check_x86_drvmod_isa.py $(X86_DRVMOD_ELFS)
drvmod-examples: check-x86-drvmod-isa
endif

drvmod-examples: $(addprefix $(DRVMOD_BUILD_DIR)/,$(DRVMOD_MODULES))

$(FAT32_IMG): drvmod-examples $(addprefix $(DRVMOD_BUILD_DIR)/,$(RUNTIME_DRVMOD_MODULES))

DRIVER_STORE_USER_PACKAGES = $(notdir $(NATIVE_RTCDD_BIN)) \
                             $(notdir $(NATIVE_UBDD_BIN)) \
                             $(notdir $(NATIVE_UINPUTD_BIN)) \
                             $(notdir $(NATIVE_UEDUD_BIN))

smoke-drvmod-riscv64:
	$(PYTHON) tools/smoke.py smoke-drvmod-riscv64

smoke-drvmod-aarch64:
	$(PYTHON) tools/smoke.py smoke-drvmod-aarch64

smoke-drvmod-loongarch64:
	$(PYTHON) tools/smoke.py smoke-drvmod-loongarch64

smoke-drvmod-x86_64:
	$(PYTHON) tools/smoke.py smoke-drvmod-x86_64

smoke-drvmod: smoke-drvmod-riscv64 smoke-drvmod-x86_64 smoke-drvmod-aarch64 smoke-drvmod-loongarch64

else

DRIVER_STORE_USER_PACKAGES :=

drvmod-examples:
	@echo "drvmod-examples: skipped (DRIVER_DEPLOYMENT=embedded)"

smoke-drvmod-riscv64 smoke-drvmod-aarch64 smoke-drvmod-loongarch64 smoke-drvmod-x86_64 smoke-drvmod:
	@echo "driver module smokes require DRIVER_DEPLOYMENT=generic"
	@false

endif

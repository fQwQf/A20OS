# BL702 hardware probe.  Keep it independent of the SBI-based RV32 OS build.
M0SENSE_PROBE_DIR := kernel/arch/riscv32/boards/m0sense-bl702/probe
M0SENSE_PROBE_OUT := build/m0sense-bl702/handoff
M0SENSE_CROSS ?= $(RISCV_ELF_PREFIX)
M0SENSE_FLAGS := -march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany \
    -Os -g -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie \
    -fno-stack-protector -ffunction-sections -fdata-sections -Wall -Wextra -Werror
.PHONY: m0sense-probe
m0sense-probe:
	@mkdir -p $(M0SENSE_PROBE_OUT)
	$(if $(filter llvm,$(TOOLCHAIN)),$(CLANG) --target=riscv32-unknown-elf $(LLVM_LINK),$(M0SENSE_CROSS)gcc) $(M0SENSE_FLAGS) -nostdlib -nostartfiles \
	    -Wl,--gc-sections,--build-id=none,-Map,$(M0SENSE_PROBE_OUT)/probe.map \
	    -T $(M0SENSE_PROBE_DIR)/probe.ld $(M0SENSE_PROBE_DIR)/entry.S \
	    $(M0SENSE_PROBE_DIR)/probe.c -o $(M0SENSE_PROBE_OUT)/probe.elf
	$(if $(filter llvm,$(TOOLCHAIN)),$(LLVM_PREFIX)llvm-objcopy$(LLVM_SUFFIX),$(M0SENSE_CROSS)objcopy) -O binary -j .text $(M0SENSE_PROBE_OUT)/probe.elf $(M0SENSE_PROBE_OUT)/probe.bin
	$(if $(filter llvm,$(TOOLCHAIN)),$(CLANG) --target=riscv32-unknown-elf $(LLVM_LINK),$(M0SENSE_CROSS)gcc) $(M0SENSE_FLAGS) -nostdlib -nostartfiles \
	    -Wl,--gc-sections,--build-id=none,-Map,$(M0SENSE_PROBE_OUT)/probe-xip.map \
	    -T $(M0SENSE_PROBE_DIR)/probe-xip.ld $(M0SENSE_PROBE_DIR)/xip_entry.S \
	    $(M0SENSE_PROBE_DIR)/entry.S $(M0SENSE_PROBE_DIR)/probe.c \
	    -o $(M0SENSE_PROBE_OUT)/probe-xip.elf
	$(if $(filter llvm,$(TOOLCHAIN)),$(LLVM_PREFIX)llvm-objcopy$(LLVM_SUFFIX),$(M0SENSE_CROSS)objcopy) -O binary $(M0SENSE_PROBE_OUT)/probe-xip.elf $(M0SENSE_PROBE_OUT)/probe-app.bin
	python3 tools/boards/mk_m0sense_uf2.py $(M0SENSE_PROBE_OUT)/probe-xip.elf \
	    $(M0SENSE_PROBE_OUT)/probe-app.bin $(M0SENSE_PROBE_OUT)/probe.uf2
	cp $(M0SENSE_PROBE_DIR)/README.md $(M0SENSE_PROBE_DIR)/openocd.cfg \
	    $(M0SENSE_PROBE_DIR)/load-probe.sh $(M0SENSE_PROBE_OUT)/
	cp tools/boards/m0sense_deploy_uf2.py $(M0SENSE_PROBE_OUT)/deploy-uf2.py
	chmod +x $(M0SENSE_PROBE_OUT)/load-probe.sh
	$(if $(filter llvm,$(TOOLCHAIN)),$(LLVM_PREFIX)llvm-size$(LLVM_SUFFIX),$(M0SENSE_CROSS)size) $(M0SENSE_PROBE_OUT)/probe.elf
	@echo "Probe ready: probe.elf (JTAG SRAM), probe.uf2 (factory M0SENSE USB loader); no full OS"

.PHONY: m0sense-recovery-assets
m0sense-recovery-assets:
	python3 tools/boards/m0sense_recovery_assets.py

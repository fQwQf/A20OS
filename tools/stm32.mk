# STM32 board-only build, image-generation, and hardware launch rules.

$(STM32_BT_CONFIG_HDR): FORCE
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#ifndef GENERATED_STM32_BLUETOOTH_CONFIG_H'; \
		printf '%s\n' '#define GENERATED_STM32_BLUETOOTH_CONFIG_H'; \
		printf '#define STM32_BLUETOOTH_DEVICE_NAME "%s"\n' '$(STM32_BT_NAME)'; \
		printf '#define STM32_BLUETOOTH_PIN "%s"\n' '$(STM32_BT_PIN)'; \
		printf '#define STM32_BLUETOOTH_SERVICE_UUID 0x%sU\n' '$(STM32_BT_UUID)'; \
		printf '#define STM32_BLUETOOTH_SERVICE_UUID_TEXT "%s"\n' '$(STM32_BT_UUID)'; \
		printf '#define STM32_BLUETOOTH_BAUD_RATE %sU\n' '$(STM32_BT_BAUD)'; \
		printf '#define STM32_BLUETOOTH_BAUD_RATE_TEXT "%s"\n' '$(STM32_BT_BAUD)'; \
		printf '%s\n' '#endif'; \
	} > $@.tmp
	@$(PYTHON) tools/stamps.py adopt --tmp "$@.tmp" --target "$@"

$(STM32_WIFI_CONFIG_HDR): FORCE
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#ifndef GENERATED_STM32_WIFI_CONFIG_H'; \
		printf '%s\n' '#define GENERATED_STM32_WIFI_CONFIG_H'; \
		printf '#define STM32_WIFI_SSID "%s"\n' '$(STM32_WIFI_SSID)'; \
		printf '#define STM32_WIFI_PASSWORD "%s"\n' '$(STM32_WIFI_PASSWORD)'; \
		printf '%s\n' '#endif'; \
	} > $@.tmp
	@$(PYTHON) tools/stamps.py adopt --tmp "$@.tmp" --target "$@"

$(BUILD_DIR)/drivers/stm32f1/bluetooth.o: $(STM32_BT_CONFIG_HDR)
$(BUILD_DIR)/drivers/stm32f1/wifi.o: $(STM32_WIFI_CONFIG_HDR)

# Thin wrappers: STM32 configurations live in instances/stm32f103*.toml.
stm32f103-bringup:
	tools/a20 build stm32f103

stm32f103-xuanwu:
	tools/a20 build stm32f103-xuanwu
check-stm32f103:
	@$(PYTHON) tools/stm32.py check-config \
		--kernel-dir "$(KERNEL_DIR)" \
		--arch "$(ARCH)" --board "$(BOARD)" --abi "$(ABI)" \
		--bringup "$(BRINGUP)" --build-target stm32f103-xuanwu

# Thin wrapper: configuration in instances/stm32f103-xuanwu.toml.
flash-stm32f103-xuanwu:
	tools/a20 flash stm32f103-xuanwu

flash-xuanwu-openocd:
	@$(PYTHON) tools/stm32.py openocd-flash \
		--interface "$(STM32_OPENOCD_INTERFACE)" \
		--serial "$(STM32_CMSIS_DAP_SERIAL)" \
		--transport "$(STM32_OPENOCD_TRANSPORT)" \
		--adapter-khz "$(STM32_OPENOCD_ADAPTER_KHZ)" \
		--elf "$(STM32_XUANWU_ELF)"

# Thin wrapper: configuration in instances/stm32f103-qemu.toml.
run-stm32f103-qemu:
	tools/a20 run stm32f103-qemu

run-stm32f103-qemu-impl:
	@$(PYTHON) tools/stm32.py qemu-run --kernel-bin "$(STM32_QEMU_BIN)"

# Full kernel functional instruction test. Python requires unicorn/pyelftools;
# this target does not claim stm32vldiscovery implements the external fabric.
.PHONY: check-mcu-shared-kernel
check-mcu-shared-kernel:
	$(MAKE) ARCH=armv7m BOARD=stm32f103 ABI=native STM32_FLASH_KB=512 \
		STM32_RAM_KB=64 STM32_QEMU=0 STM32_XUANWU=0 STM32_CLUSTER_PORT=0 \
		NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 CONFIG_KERNEL_PREEMPT=1 \
		MCU_SHARED_KERNEL=1 MCU_SHARED_RAM_KB=1024 MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=0 _check-mcu-shared-kernel

.PHONY: _check-mcu-shared-kernel
_check-mcu-shared-kernel: kernel-only
	$(PYTHON) tools/check-mcu-shared-kernel.py \
		$(BUILD_DIR)/kernel.elf \
		--report .kernel-build/cluster/shared-kernel/report.json

.PHONY: check-mcu-shared-fabric
check-mcu-shared-fabric:
	verilator --binary --timing --top-module a20_fabric_tb \
		-Mdir .kernel-build/cluster/shared-fabric-verilator \
		hardware/shared-mcu/a20_fabric.sv hardware/shared-mcu/a20_fabric_tb.sv
	.kernel-build/cluster/shared-fabric-verilator/Va20_fabric_tb

.PHONY: check-mcu-shared-bus
check-mcu-shared-bus:
	verilator --binary --timing --top-module a20_shared_top_tb \
		-Mdir .kernel-build/cluster/shared-bus-verilator \
		hardware/shared-mcu/a20_fabric.sv hardware/shared-mcu/a20_fsmc_bridge.sv \
		hardware/shared-mcu/a20_shared_ram.sv hardware/shared-mcu/a20_shared_top.sv \
		hardware/shared-mcu/a20_shared_top_tb.sv
	.kernel-build/cluster/shared-bus-verilator/Va20_shared_top_tb

.PHONY: build-mcu-shared-rtl check-mcu-shared-rtl _check-mcu-shared-rtl
build-mcu-shared-rtl:
	verilator --cc --exe --build -j 4 --top-module a20_shared_top \
		-GCPUS=2 -GRAM_ADDR_BITS=20 -Mdir .kernel-build/cluster/shared-rtl-verilator \
		hardware/shared-mcu/a20_fabric.sv hardware/shared-mcu/a20_fsmc_bridge.sv \
		hardware/shared-mcu/a20_shared_ram.sv hardware/shared-mcu/a20_shared_top.sv \
		$(CURDIR)/hardware/shared-mcu/a20_bus_cosim.cpp

check-mcu-shared-rtl: build-mcu-shared-rtl
	$(MAKE) ARCH=armv7m BOARD=stm32f103 ABI=native STM32_FLASH_KB=512 \
		STM32_RAM_KB=64 STM32_QEMU=0 STM32_XUANWU=0 STM32_CLUSTER_PORT=0 \
		NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 CONFIG_KERNEL_PREEMPT=1 \
		MCU_SHARED_KERNEL=1 MCU_SHARED_RAM_KB=1024 MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=0 _check-mcu-shared-rtl

_check-mcu-shared-rtl: kernel-only
	$(PYTHON) tools/check-mcu-shared-rtl.py $(BUILD_DIR)/kernel.elf \
		--backend .kernel-build/cluster/shared-rtl-verilator/Va20_shared_top \
		--report .kernel-build/cluster/shared-kernel/report-rtl.json

.PHONY: check-mcu-program check-mcu-program-rtl _check-mcu-program _check-mcu-program-rtl
check-mcu-program:
	$(MAKE) ARCH=armv7m BOARD=stm32f103 ABI=native STM32_FLASH_KB=512 \
		STM32_RAM_KB=64 STM32_QEMU=0 STM32_XUANWU=0 STM32_CLUSTER_PORT=0 \
		NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 CONFIG_KERNEL_PREEMPT=1 \
		MCU_SHARED_KERNEL=1 MCU_SHARED_RAM_KB=1024 MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=1 _check-mcu-program

check-mcu-program-rtl: build-mcu-shared-rtl
	$(MAKE) ARCH=armv7m BOARD=stm32f103 ABI=native STM32_FLASH_KB=512 \
		STM32_RAM_KB=64 STM32_QEMU=0 STM32_XUANWU=0 STM32_CLUSTER_PORT=0 \
		NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 CONFIG_KERNEL_PREEMPT=1 \
		MCU_SHARED_KERNEL=1 MCU_SHARED_RAM_KB=1024 MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=1 _check-mcu-program-rtl

_check-mcu-program: kernel-only
	$(PYTHON) tools/check-mcu-program.py $(BUILD_DIR)/kernel.elf \
		--report .kernel-build/cluster/shared-program/report.json

_check-mcu-program-rtl: kernel-only
	$(PYTHON) tools/check-mcu-program.py $(BUILD_DIR)/kernel.elf \
		--backend .kernel-build/cluster/shared-rtl-verilator/Va20_shared_top \
		--report .kernel-build/cluster/shared-program/report-rtl.json

ifeq ($(MCU_NATIVE_PROGRAM)$(MCU_SHARED_TEST),11)
$(BUILD_DIR)/arch/armv7m/boot/program_test.o: $(BUILD_DIR)/mcu-test-program.elf
$(BUILD_DIR)/mcu-test-program.elf: user/tests/mcu-shared-program/main.c user/liba20rt/a20_compiler_rt.c \
    user/liba20rt/a20_malloc.c user/liba20rt/a20_string.h user/tests/native-memory-contract.h \
    user/tests/mcu-shared-program/link.ld user/liba20rt/a20_task.h \
    user/liba20rt/a20_syscall.h user/liba20rt/a20_types.h user/liba20rt/a20_sync.h user/liba20rt/a20_mem.h \
    kernel/arch/armv7m/include/program_test.h user/liba20rt/a20_execution.h \
    kernel/include/uapi/a20/execution.h
	@mkdir -p $(@D)
	$(CC) -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -Os -ffreestanding \
		-fPIE -fomit-frame-pointer -fno-stack-protector -nostdlib -Wall -Wextra -Werror \
		-Wl,-pie,--no-dynamic-linker,--build-id=none,-z,max-page-size=4096 \
		-Wl,-T,user/tests/mcu-shared-program/link.ld \
		-Iuser/liba20rt -Ikernel/arch/armv7m/include $< user/liba20rt/a20_compiler_rt.c user/liba20rt/a20_malloc.c -lgcc -o $@
endif

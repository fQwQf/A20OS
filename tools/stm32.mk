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
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(STM32_WIFI_CONFIG_HDR): FORCE
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#ifndef GENERATED_STM32_WIFI_CONFIG_H'; \
		printf '%s\n' '#define GENERATED_STM32_WIFI_CONFIG_H'; \
		printf '#define STM32_WIFI_SSID "%s"\n' '$(STM32_WIFI_SSID)'; \
		printf '#define STM32_WIFI_PASSWORD "%s"\n' '$(STM32_WIFI_PASSWORD)'; \
		printf '%s\n' '#endif'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(BUILD_DIR)/drivers/stm32f1/bluetooth.o: $(STM32_BT_CONFIG_HDR)
$(BUILD_DIR)/drivers/stm32f1/wifi.o: $(STM32_WIFI_CONFIG_HDR)

# Thin wrappers: STM32 configurations live in instances/stm32f103*.toml.
stm32f103-bringup:
	tools/a20 build stm32f103

stm32f103-xuanwu:
	tools/a20 build stm32f103-xuanwu
check-stm32f103:
	@! rg -n '0x400[0-9A-Fa-f]{5}|0xE000E[0-9A-Fa-f]{3}' \
		$(KERNEL_DIR)/platform/stm32f103 --glob '*.[ch]' \
		--glob '!board.c' --glob '!board_config.h'
	@! rg -n 'CONFIG_ARMV7M' $(KERNEL_DIR) \
		--glob '!kernel/arch/**' --glob '!kernel/platform/**' \
		--glob '!kernel/external/**' --glob '!kernel/include/core/arch.h'
	$(MAKE) stm32f103-xuanwu
	@echo "check-stm32f103: PASS"

# Thin wrapper: configuration in instances/stm32f103-xuanwu.toml.
flash-stm32f103-xuanwu:
	tools/a20 flash stm32f103-xuanwu

flash-xuanwu-openocd:
	@command -v openocd >/dev/null 2>&1 || { \
		echo "openocd not found; install OpenOCD or use STM32CubeProgrammer"; \
		exit 1; \
	}
	openocd -f $(STM32_OPENOCD_INTERFACE) \
		$(if $(STM32_CMSIS_DAP_SERIAL),-c "adapter serial $(STM32_CMSIS_DAP_SERIAL)") \
		-c "transport select $(STM32_OPENOCD_TRANSPORT)" \
		-c "adapter speed $(STM32_OPENOCD_ADAPTER_KHZ)" \
		-f target/stm32f1x.cfg \
		-c "init" \
		-c "mww 0xE000EDF0 0xA05F0003" \
		-c "sleep 50" \
		-c "flash probe 0" \
		-c "flash write_image erase $(STM32_XUANWU_ELF)" \
		-c "verify_image $(STM32_XUANWU_ELF)" \
		-c "set boot_sp [mrw 0x08000000]" \
		-c "set boot_pc [mrw 0x08000004]" \
		-c "reg msp \$$boot_sp" \
		-c "reg psp 0" \
		-c "reg control 0" \
		-c "reg primask 0" \
		-c "reg basepri 0" \
		-c "reg faultmask 0" \
		-c "reg pc \$$boot_pc" \
		-c "resume" \
		-c "shutdown"

# Thin wrapper: configuration in instances/stm32f103-qemu.toml.
run-stm32f103-qemu:
	tools/a20 run stm32f103-qemu

run-stm32f103-qemu-impl:
	qemu-system-arm -machine stm32vldiscovery -nographic \
		-kernel $(STM32_QEMU_BIN)

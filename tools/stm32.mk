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

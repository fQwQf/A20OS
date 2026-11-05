# Host preparation is separate from offline kernel builds.
ESPTOOL_PORT ?=
ESPTOOL_BAUD ?= 460800
.PHONY: esp32-c3-prepare esp32-c3-package flash-esp32-c3
esp32-c3-prepare:
	$(PYTHON) tools/boards/esp32c3.py prepare
esp32-c3-package: kernel-only
	$(PYTHON) tools/boards/esp32c3.py package --elf "$(KERNEL_ELF)"
flash-esp32-c3: esp32-c3-package
	$(PYTHON) tools/boards/esp32c3.py flash --port "$(ESPTOOL_PORT)" --baud "$(ESPTOOL_BAUD)"

.PHONY: esp32-c3-qemu-prepare check-esp32-c3-rom
esp32-c3-qemu-prepare:
	$(PYTHON) tools/boards/esp32c3.py prepare-qemu
check-esp32-c3-rom: esp32-c3-package
	$(PYTHON) tools/boards/check_esp32c3_rom.py --qemu "$(ESP32C3_QEMU)"

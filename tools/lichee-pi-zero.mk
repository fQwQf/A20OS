LICHEE_PI_ZERO_FIRMWARE_DIR ?= build/lichee-pi-zero/firmware
LICHEE_PI_ZERO_IMAGE ?= build/lichee-pi-zero/a20os-lichee-pi-zero-sdcard.img
LICHEE_PI_ZERO_IMAGE_MIB ?= 128

.PHONY: lichee-pi-zero-prepare lichee-pi-zero-sdcard
lichee-pi-zero-prepare:
	$(PYTHON) tools/lichee_pi_zero.py prepare --out $(LICHEE_PI_ZERO_FIRMWARE_DIR)

lichee-pi-zero-sdcard: kernel-only
	@test "$(BOARD)" = lichee-pi-zero || { echo "requires BOARD=lichee-pi-zero" >&2; exit 1; }
	$(PYTHON) tools/lichee_pi_zero.py sdcard --firmware $(LICHEE_PI_ZERO_FIRMWARE_DIR) \
		--kernel $(KERNEL_BIN) --elf $(KERNEL_ELF) --bundle build/lichee-pi-zero/handoff \
		$(if $(HANDOFF_INSTANCE),--instance $(HANDOFF_INSTANCE),) \
		--output $(LICHEE_PI_ZERO_IMAGE) --size-mib $(LICHEE_PI_ZERO_IMAGE_MIB)

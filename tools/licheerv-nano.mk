LICHEERV_NANO_FIRMWARE_DIR ?= build/licheerv-nano/firmware
LICHEERV_NANO_IMAGE ?= build/licheerv-nano/a20os-licheerv-nano-sdcard.img
LICHEERV_NANO_IMAGE_MIB ?= 128

.PHONY: licheerv-nano-prepare licheerv-nano-sdcard
licheerv-nano-prepare:
	$(PYTHON) tools/licheerv_nano.py prepare --out $(LICHEERV_NANO_FIRMWARE_DIR)

licheerv-nano-sdcard: kernel-bundle
	@test "$(BOARD)" = licheerv-nano || { echo "licheerv-nano-sdcard requires BOARD=licheerv-nano" >&2; exit 1; }
	$(PYTHON) tools/licheerv_nano.py sdcard --firmware $(LICHEERV_NANO_FIRMWARE_DIR) \
		--bundle build/licheerv-nano/handoff --output $(LICHEERV_NANO_IMAGE) \
		--size-mib $(LICHEERV_NANO_IMAGE_MIB)

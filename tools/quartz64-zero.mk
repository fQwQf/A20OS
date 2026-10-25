QUARTZ64_ZERO_FIRMWARE_DIR ?= build/quartz64-zero/firmware
QUARTZ64_ZERO_IMAGE ?= build/quartz64-zero/a20os-quartz64-zero-sdcard.img
QUARTZ64_ZERO_IMAGE_MIB ?= 128
.PHONY: quartz64-zero-prepare quartz64-zero-sdcard
quartz64-zero-prepare:
	$(PYTHON) tools/quartz64_zero.py prepare --out $(QUARTZ64_ZERO_FIRMWARE_DIR)
quartz64-zero-sdcard: kernel-bundle
	@test "$(BOARD)" = quartz64-zero || { echo "quartz64-zero-sdcard requires BOARD=quartz64-zero" >&2; exit 1; }
	$(PYTHON) tools/quartz64_zero.py sdcard --firmware $(QUARTZ64_ZERO_FIRMWARE_DIR) \
		--bundle build/quartz64-zero/handoff --output $(QUARTZ64_ZERO_IMAGE) --size-mib $(QUARTZ64_ZERO_IMAGE_MIB)

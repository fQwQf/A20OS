QUARTZ64_ZERO_VIDEO ?= 0
ifeq ($(QUARTZ64_ZERO_VIDEO),1)
QUARTZ64_ZERO_FIRMWARE_DIR ?= build/quartz64-zero/firmware-video
else
QUARTZ64_ZERO_FIRMWARE_DIR ?= build/quartz64-zero/firmware
endif
QUARTZ64_ZERO_IMAGE ?= build/quartz64-zero/a20os-quartz64-zero-sdcard.img
QUARTZ64_ZERO_IMAGE_MIB ?= 128
.PHONY: quartz64-zero-prepare quartz64-zero-sdcard
quartz64-zero-prepare:
	$(PYTHON) tools/boards/quartz64_zero.py prepare $(if $(filter 1,$(QUARTZ64_ZERO_VIDEO)),--video) --out $(QUARTZ64_ZERO_FIRMWARE_DIR)
quartz64-zero-sdcard: kernel-bundle
	@test "$(BOARD)" = quartz64-zero || { echo "quartz64-zero-sdcard requires BOARD=quartz64-zero" >&2; exit 1; }
	$(PYTHON) tools/boards/quartz64_zero.py sdcard --firmware $(QUARTZ64_ZERO_FIRMWARE_DIR) \
		--bundle build/quartz64-zero/handoff --output $(QUARTZ64_ZERO_IMAGE) --size-mib $(QUARTZ64_ZERO_IMAGE_MIB)

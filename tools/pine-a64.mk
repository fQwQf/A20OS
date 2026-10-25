# Original PINE A64 512 MiB firmware preparation and complete SD artifact.
PINE_A64_FIRMWARE_DIR ?= build/pine-a64/firmware
PINE_A64_IMAGE ?= build/pine-a64/a20os-pine-a64-sdcard.img
PINE_A64_IMAGE_MIB ?= 128

.PHONY: pine-a64-prepare pine-a64-sdcard
pine-a64-prepare:
	$(PYTHON) tools/pine_a64.py prepare --out $(PINE_A64_FIRMWARE_DIR)

# Called with the PINE instance's ARCH/BOARD/ABI/BRINGUP selections. Keep the
# regular Image bundle and whole-card artifact derived from the same ELF.
pine-a64-sdcard: kernel-bundle
	@test "$(BOARD)" = pine-a64 || { echo "pine-a64-sdcard requires BOARD=pine-a64" >&2; exit 1; }
	$(PYTHON) tools/pine_a64.py sdcard --firmware $(PINE_A64_FIRMWARE_DIR) \
		--bundle build/pine-a64/handoff --output $(PINE_A64_IMAGE) \
		--size-mib $(PINE_A64_IMAGE_MIB)

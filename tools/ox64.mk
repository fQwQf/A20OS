# D0 C906 SD payload; SPI lowload/OpenSBI/U-Boot stays on the board.
OX64_FIRMWARE_DIR ?= build/ox64/firmware
OX64_IMAGE ?= build/ox64/a20os-ox64-sdcard.img
OX64_IMAGE_MIB ?= 128
.PHONY: ox64-prepare ox64-sdcard
ox64-prepare:
	$(PYTHON) tools/ox64.py prepare --out $(OX64_FIRMWARE_DIR)
ox64-sdcard: kernel-bundle
	@test "$(BOARD)" = ox64 || { echo "ox64-sdcard requires BOARD=ox64" >&2; exit 1; }
	$(PYTHON) tools/ox64.py sdcard --firmware $(OX64_FIRMWARE_DIR) \
		--bundle build/ox64/handoff --output $(OX64_IMAGE) --size-mib $(OX64_IMAGE_MIB)

# Physical-target actions for `a20 console` / `a20 deploy` (docs/instances.md).
#
# The instance declares which board and which console via the TARGET_* variables
# that tools/a20 derives from its [target] section; the recipes that act on the
# board live here, so adding a board or a programmer never means editing Python.

TARGET_SERIAL ?=
TARGET_MEDIA_DEVICE ?=
TARGET_BOOT_MEDIA ?=

# Refuse to write to a device that does not exist, is not a block device, or is
# mounted.  `dd` to the wrong node is the one mistake in this file that is not
# recoverable, and a plain "device busy" from the kernel is a late place to find
# out.
target-write-media:
	@if [ -z "$(TARGET_BOOT_MEDIA)" ]; then \
		echo "target-write-media: TARGET_BOOT_MEDIA is empty"; exit 1; \
	fi
	@if [ -z "$(TARGET_MEDIA_DEVICE)" ]; then \
		echo "target-write-media: TARGET_MEDIA_DEVICE is empty"; exit 1; \
	fi
	@if [ ! -b "$(TARGET_MEDIA_DEVICE)" ]; then \
		echo "target-write-media: $(TARGET_MEDIA_DEVICE) is not a block device;"; \
		echo "  refusing to dd to it (a mistyped node destroys the host's disk)"; \
		exit 1; \
	fi
	@if mountpoint -q "$(TARGET_MEDIA_DEVICE)" 2>/dev/null; then \
		echo "target-write-media: $(TARGET_MEDIA_DEVICE) is mounted; unmount it first"; \
		exit 1; \
	fi
	@for img in $(TARGET_BOOT_MEDIA); do \
		if [ ! -f "$$img" ]; then \
			echo "target-write-media: missing image $$img"; exit 1; \
		fi; \
		echo "  writing $$img -> $(TARGET_MEDIA_DEVICE)"; \
		dd if="$$img" of="$(TARGET_MEDIA_DEVICE)" bs=4M conv=fsync status=progress; \
		sync; \
	done
	@echo "target-write-media: done"

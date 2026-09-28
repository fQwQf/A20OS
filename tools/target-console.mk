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
# out.  The guards themselves live in tools/media.py so they can be tested
# without a real disk.
target-write-media:
	@$(PYTHON) tools/media.py \
		--boot-media "$(TARGET_BOOT_MEDIA)" \
		--media-device "$(TARGET_MEDIA_DEVICE)"

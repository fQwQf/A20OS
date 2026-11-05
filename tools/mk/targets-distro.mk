
# ================================================================
# Distro rootfs run (boot a stock Linux distribution as userspace)
# ================================================================
# One command that builds the kernel, builds the Alpine distro rootfs
# (ext4), and boots QEMU with it under the GUI display.  The A20OS init
# detects the Alpine distro rootfs mounted at /extra and chroot(2)s into it,
# exec'ing /sbin/init, which brings up dbus / elogind / seatd / udevd and the
# XFCE Wayland session.  The kernel only supplies the Linux ABI + /dev +
# /proc + /sys; the distro owns the userspace.
#
#   make distro-run              # ARCH=riscv64 by default
#   make distro-run ARCH=x86_64
#   make distro-run-riscv64
#
# Requires a host QEMU + an Alpine mirror reachable from the build host.

ALPINE_ROOTFS_IMG ?= build/alpine/rootfs.img

.PHONY: distro-run distro-run-riscv64 distro-run-x86_64
distro-run:
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
	$(MAKE) ARCH=$(ARCH) $(FAT32_IMG)
	@test -s $(KERNEL_ELF) || (echo "ERROR: $(KERNEL_ELF) is missing/empty" >&2; exit 1)
	$(MAKE) ARCH=$(ARCH) ALPINE_ROOTFS_OUTPUT=$(ALPINE_ROOTFS_IMG) rootfs-alpine
	@test -s $(ALPINE_ROOTFS_IMG) || (echo "ERROR: $(ALPINE_ROOTFS_IMG) is missing/empty" >&2; exit 1)
	$(QEMU) $(patsubst -nographic,-display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) $(QEMU_GUI_AUDIO) -serial stdio,$(QEMU_FLAGS_NO_SDCARD)) \
		-drive file=$(ALPINE_ROOTFS_IMG),if=none,format=raw,id=x1 \
		-device $(QEMU_BLK_SECOND),drive=x1 \
		-kernel $(KERNEL_ELF)

distro-run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) distro-run

distro-run-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) distro-run


# ================================================================
# Distro rootfs (package-manager provisioning path)
# ================================================================
# Build an Alpine Linux ext4 rootfs containing XFCE4 and the login/device
# service layer (dbus / elogind / polkit / seatd / eudev) that the
# from-source user/wayland path stubs out.  This is the package-manager
# path: the kernel only has to provide a Linux ABI + ext4 + /dev + /sys.
#
#   make rootfs-alpine ARCH=riscv64
#   make rootfs-alpine ARCH=x86_64  ALPINE_ROOTFS_OUTPUT=build/alpine/rootfs.img
#
# The resulting image is a raw ext4 filesystem (no partition table); mount
# it with `mount -o loop` on the host or attach it to the QEMU root disk.
# `make distro-run` uses this image and boots it under QEMU.

ALPINE_ROOTFS_OUTPUT ?= build/alpine/rootfs.img
ALPINE_ROOTFS_SIZE_MB ?= 8192
ALPINE_MIRROR_ROOT ?= https://mirrors.ustc.edu.cn/alpine

.PHONY: rootfs-alpine
rootfs-alpine:
	ARCH=$(ARCH) OUTPUT=$(ALPINE_ROOTFS_OUTPUT) \
		ROOTFS_SIZE_MB=$(ALPINE_ROOTFS_SIZE_MB) \
		ALPINE_MIRROR_ROOT=$(ALPINE_MIRROR_ROOT) \
		bash user/rootfs/alpine/build.sh

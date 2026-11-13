# Driver source ownership by deployment profile.
#
# generic contains only the irreducible boot substrate and kernel services:
# the unified driver core, buses, and service layers.  Every discoverable
# device driver is packaged by tools/mk/driver-modules.mk as a .a20drv module and
# loaded from the Early or Runtime DriverStore.  There is deliberately no
# built-in device driver list left: adding one must be a conscious bootstrap
# exception, not a new wildcard hidden in the top-level Makefile.

DRIVER_CORE_SRCS := \
    $(wildcard $(KERNEL_DIR)/drivers/core/*.c) \
    $(wildcard $(KERNEL_DIR)/drivers/bus/*.c) \
    $(wildcard $(KERNEL_DIR)/drivers/irqchip/*.c)

# Kernel services, not migratable device drivers. They provide the substrate
# used by optional .a20drv packages and remain in the image.
# simplefb is the bootstrap exception: it preserves the firmware-owned
# scanout before a native GPU driver or module-backed display is available.
# lsdc-boot is the same explicit bootstrap exception for firmware that exposes
# an active scanout through controller registers rather than a DT/GOP handoff.
GENERIC_KERNEL_SERVICE_SRCS := \
    $(KERNEL_DIR)/drivers/i2c/i2c-core.c \
    $(KERNEL_DIR)/drivers/gpio/firmware_input.c \
    $(KERNEL_DIR)/drivers/block/loop.c \
    $(KERNEL_DIR)/drivers/block/udisk.c \
    $(KERNEL_DIR)/drivers/net/tun.c \
    $(KERNEL_DIR)/drivers/char/pty.c \
    $(KERNEL_DIR)/drivers/char/uart.c \
    $(KERNEL_DIR)/drivers/char/fbcon.c \
    $(KERNEL_DIR)/drivers/gpu/framebuffer.c \
    $(KERNEL_DIR)/drivers/gpu/simplefb.c \
    $(KERNEL_DIR)/drivers/gpu/lsdc_boot.c \
    $(KERNEL_DIR)/drivers/gpu/gpu_core.c \
    $(KERNEL_DIR)/drivers/gpu/drm.c \
    $(KERNEL_DIR)/drivers/audio/audio_core.c \
    $(KERNEL_DIR)/drivers/audio/alsa.c \
    $(KERNEL_DIR)/drivers/input/input_mux.c \
    $(KERNEL_DIR)/drivers/usb/core/usb_core.c

ifeq ($(WIFI_RTW89),1)
# The resident broker owns DMA lifetime and transports Ethernet frames for
# the independent userspace radio service.
GENERIC_KERNEL_SERVICE_SRCS += $(KERNEL_DIR)/drivers/net/rtw89_broker.c
endif

# Complete device-driver set, statically linked only for the embedded
# profile.  The generic profile builds each of these as a .a20drv package
# (see tools/mk/driver-modules.mk).
EMBEDDED_DEVICE_DRIVER_SRCS := \
    $(KERNEL_DIR)/drivers/block/ahci.c \
    $(KERNEL_DIR)/drivers/block/dw_sdio.c \
    $(KERNEL_DIR)/drivers/block/sunxi_mmc.c \
    $(KERNEL_DIR)/drivers/block/sunxi_mmc_core.c \
    $(KERNEL_DIR)/drivers/block/sdhci.c \
    $(KERNEL_DIR)/drivers/block/sdhci_core.c \
    $(KERNEL_DIR)/drivers/block/virtio_blk.c \
    $(KERNEL_DIR)/drivers/block/virtio_scsi.c \
    $(KERNEL_DIR)/drivers/i2c/i2c-designware.c \
    $(KERNEL_DIR)/drivers/hid/i2c-hid.c \
    $(KERNEL_DIR)/drivers/net/e1000.c \
    $(KERNEL_DIR)/drivers/net/ls2k_gmac.c \
    $(KERNEL_DIR)/drivers/net/r8169.c \
    $(KERNEL_DIR)/drivers/net/rtl8139.c \
    $(KERNEL_DIR)/drivers/net/starfive_gmac.c \
    $(KERNEL_DIR)/drivers/net/virtio_net.c \
    $(KERNEL_DIR)/drivers/gpu/virtio_gpu.c \
    $(KERNEL_DIR)/drivers/gpu/vmsvga.c \
    $(KERNEL_DIR)/drivers/audio/virtio_snd.c \
    $(KERNEL_DIR)/drivers/audio/sunxi_audio.c \
    $(KERNEL_DIR)/drivers/audio/rk817_audio.c \
    $(KERNEL_DIR)/drivers/audio/sg2002_audio.c \
    $(KERNEL_DIR)/drvmod/examples/hda.c \
    $(KERNEL_DIR)/drvmod/examples/vinput.c \
    $(KERNEL_DIR)/drivers/usb/host/xhci.c \
    $(KERNEL_DIR)/drivers/usb/class/usb_hid.c \
    $(KERNEL_DIR)/drivers/usb/class/usb_hub.c \
    $(KERNEL_DIR)/drivers/usb/class/usb_storage.c

# The embedded profile links the driver set this architecture can actually
# own.  NVMe and PS/2 have thin drvmod packaging and a shared implementation; a machine whose only disk is NVMe
# (or whose only console input is the i8042 keyboard) needs them before any
# userspace DriverStore exists, so they are part of the x86_64 embedded set.
ifeq ($(filter $(ARCH),x86_64 loongarch64),$(ARCH))
EMBEDDED_DEVICE_DRIVER_SRCS += $(KERNEL_DIR)/drivers/block/nvme.c
endif
ifeq ($(ARCH),x86_64)
EMBEDDED_DEVICE_DRIVER_SRCS += $(KERNEL_DIR)/drivers/input/ps2.c
endif

ifeq ($(DRIVER_DEPLOYMENT),generic)
DRIVER_KERNEL_SRCS := $(DRIVER_CORE_SRCS) $(GENERIC_KERNEL_SERVICE_SRCS)
else
# Embedded uses the identical unified driver core and source set, but links it
# all into the kernel and does not build or scan driver packages.
DRIVER_KERNEL_SRCS := $(filter-out $(KERNEL_DIR)/drivers/core/driver_manager.c,$(DRIVER_CORE_SRCS)) \
                      $(GENERIC_KERNEL_SERVICE_SRCS) \
                      $(EMBEDDED_DEVICE_DRIVER_SRCS)
endif

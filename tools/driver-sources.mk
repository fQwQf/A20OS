# Driver source ownership by deployment profile.
#
# generic contains only the irreducible boot substrate and kernel services:
# the unified driver core, buses, and service layers.  Every discoverable
# device driver is packaged by tools/driver-modules.mk as a .a20drv module and
# loaded from the Early or Runtime DriverStore.  There is deliberately no
# built-in device driver list left: adding one must be a conscious bootstrap
# exception, not a new wildcard hidden in the top-level Makefile.

DRIVER_CORE_SRCS := \
    $(wildcard $(KERNEL_DIR)/drivers/core/*.c) \
    $(wildcard $(KERNEL_DIR)/drivers/bus/*.c)

# Kernel services, not migratable device drivers. They provide the substrate
# used by optional .a20drv packages and remain in the image.
GENERIC_KERNEL_SERVICE_SRCS := \
    $(KERNEL_DIR)/drivers/block/loop.c \
    $(KERNEL_DIR)/drivers/block/udisk.c \
    $(KERNEL_DIR)/drivers/char/pty.c \
    $(KERNEL_DIR)/drivers/char/uart.c \
    $(KERNEL_DIR)/drivers/gpu/framebuffer.c \
    $(KERNEL_DIR)/drivers/gpu/gpu_core.c \
    $(KERNEL_DIR)/drivers/audio/audio_core.c \
    $(KERNEL_DIR)/drivers/input/input_mux.c \
    $(KERNEL_DIR)/drivers/usb/core/usb_core.c

# Complete device-driver set, statically linked only for the embedded
# profile.  The generic profile builds each of these as a .a20drv package
# (see tools/driver-modules.mk).
EMBEDDED_DEVICE_DRIVER_SRCS := \
    $(KERNEL_DIR)/drivers/block/ahci.c \
    $(KERNEL_DIR)/drivers/block/dw_sdio.c \
    $(KERNEL_DIR)/drivers/block/virtio_blk.c \
    $(KERNEL_DIR)/drivers/block/virtio_scsi.c \
    $(KERNEL_DIR)/drivers/net/e1000.c \
    $(KERNEL_DIR)/drivers/net/ls2k_gmac.c \
    $(KERNEL_DIR)/drivers/net/starfive_gmac.c \
    $(KERNEL_DIR)/drivers/net/virtio_net.c \
    $(KERNEL_DIR)/drivers/gpu/virtio_gpu.c \
    $(KERNEL_DIR)/drivers/gpu/vmsvga.c \
    $(KERNEL_DIR)/drivers/audio/virtio_snd.c \
    $(KERNEL_DIR)/drivers/usb/host/xhci.c \
    $(KERNEL_DIR)/drivers/usb/class/usb_hid.c \
    $(KERNEL_DIR)/drivers/usb/class/usb_storage.c

ifeq ($(DRIVER_DEPLOYMENT),generic)
DRIVER_KERNEL_SRCS := $(DRIVER_CORE_SRCS) $(GENERIC_KERNEL_SERVICE_SRCS)
else
# Embedded uses the identical unified driver core and source set, but links it
# all into the kernel and does not build or scan driver packages.
DRIVER_KERNEL_SRCS := $(filter-out $(KERNEL_DIR)/drivers/core/driver_manager.c,$(DRIVER_CORE_SRCS)) \
                      $(GENERIC_KERNEL_SERVICE_SRCS) \
                      $(EMBEDDED_DEVICE_DRIVER_SRCS)
endif

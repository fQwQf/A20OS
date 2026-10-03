# GENERATED from components/drivers.toml by `make regen-driver-fragment`.
# Do not edit by hand: edit the TOML and regenerate.  `make
# check-component-registry` fails if this file is stale.

DRVMOD_MODULES_riscv64 := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv dw-sdio.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv vinput.a20drv vinput-probe.a20drv abi-probe.a20drv hda.a20drv
EARLY_DRVMOD_MODULES_riscv64 := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv dw-sdio.a20drv
DRVMOD_MODULES_x86_64 := virtio-blk.a20drv virtio-scsi.a20drv ahci.a20drv pc-spkr.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv vinput.a20drv hda.a20drv nvme.a20drv ps2.a20drv tpm.a20drv e1000.a20drv vmsvga.a20drv xhci.a20drv usb-hid.a20drv usb-storage.a20drv usb-hub.a20drv
EARLY_DRVMOD_MODULES_x86_64 := virtio-blk.a20drv virtio-scsi.a20drv ahci.a20drv pc-spkr.a20drv
DRVMOD_MODULES_aarch64 := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv vinput.a20drv vinput-probe.a20drv hda.a20drv xhci.a20drv usb-hid.a20drv usb-storage.a20drv usb-hub.a20drv
EARLY_DRVMOD_MODULES_aarch64 := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv
DRVMOD_MODULES_loongarch64 := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv virtio-net.a20drv virtio-gpu.a20drv virtio-snd.a20drv vinput.a20drv hda.a20drv nvme.a20drv xhci.a20drv usb-hid.a20drv usb-storage.a20drv usb-hub.a20drv
EARLY_DRVMOD_MODULES_loongarch64 := rtc.a20drv virtio-blk.a20drv virtio-scsi.a20drv

# Unset for any other ARCH, matching the previous explicit empty
# assignment: undefined and empty are equivalent for every consumer.
DRVMOD_MODULES := $(DRVMOD_MODULES_$(ARCH))
EARLY_DRVMOD_MODULES := $(EARLY_DRVMOD_MODULES_$(ARCH))

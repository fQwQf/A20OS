# Native QEMU board gate; the same common test also runs on server profiles.
.PHONY: check-raspberrypi4 _check-raspberrypi4 check-fdt-view
check-raspberrypi4:
	$(MAKE) ARCH=aarch64 BOARD=raspberrypi4 ABI=both BRINGUP=1 \
		DRIVER_DEPLOYMENT=embedded NR_CPUS=4 ALLOW_UNVERIFIED_SMP=1 _check-raspberrypi4

_check-raspberrypi4: kernel-only
	$(PYTHON) tools/check-raspberrypi4.py --kernel $(KERNEL_BIN)

check-fdt-view:
	$(PYTHON) tools/check-fdt-view.py

.PHONY: check-smp-native
check-smp-native:
	$(PYTHON) tools/check-smp-native.py

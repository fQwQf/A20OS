# Development launch, VirtualBox image, and GDB entry points.

run:
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) _run_impl
run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _run_impl
run-gui-riscv64 run-gui-rv:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _run_gui_impl
run-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _run_impl
run-gui-loongarch64 run-gui-la:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _run_gui_impl
run-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _run_impl
run-gui-arm64 run-gui-aarch64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _run_gui_impl
run-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _run_impl
run-gui-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _run_gui_impl
vbox-iso-x86_64:
	$(MAKE) ARCH=x86_64 ABI=both BRINGUP=0 _vbox_iso_x86_64_impl
_vbox_iso_x86_64_impl: dev-build
	tools/mk_grub_iso.sh $(KERNEL_ELF) $(BUILD_DIR)/a20os-x86_64.iso
vbox-image-aarch64:
	$(MAKE) ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both BRINGUP=0 _vbox_image_aarch64_impl
vbox-text-image-aarch64:
	$(MAKE) ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both BRINGUP=0 _vbox_text_image_aarch64_impl
vbox-gui-image-aarch64: vbox-image-aarch64
	$(MAKE) ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both BRINGUP=0 _vbox_gui_image_aarch64_impl
_vbox_image_aarch64_impl: $(VBOX_AARCH64_IMG)
	@echo "VirtualBox ARM64 image ready: $(VBOX_AARCH64_IMG)"
_vbox_text_image_aarch64_impl: $(VBOX_AARCH64_TEXT_IMG)
	@echo "VirtualBox ARM64 text image ready: $(VBOX_AARCH64_TEXT_IMG)"
_vbox_gui_image_aarch64_impl: $(VBOX_AARCH64_EFI) $(GUI_FAT32_IMG) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $(BUILD_DIR)/a20os-vbox-aarch64-gui.img $(GUI_FAT32_IMG)
	@echo "VirtualBox ARM64 GUI image ready: $(BUILD_DIR)/a20os-vbox-aarch64-gui.img"
run-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) _run_impl
run-gui-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) _run_gui_impl
run-riscv32:
	$(MAKE) ARCH=riscv32 BRINGUP=$(BRINGUP) _run_impl
run-ppc64le:
	$(MAKE) ARCH=ppc64le BRINGUP=$(BRINGUP) _run_impl
run-nommu-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl
run-nommu-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl
run-nommu-aarch64 run-nommu-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl
run-nommu-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl
run-nommu-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl
run-gui-nommu-arm32 run-nommu-gui-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) NOMMU=1 _run_gui_impl
run-nommu-riscv32:
	$(MAKE) ARCH=riscv32 BRINGUP=$(BRINGUP) NOMMU=1 _run_impl

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	@test -s $(KERNEL_ELF) || (echo "ERROR: kernel ELF missing or empty: $(KERNEL_ELF)" ; exit 1)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

_run_gui_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) $(GUI_FAT32_IMG)
	@test -s $(KERNEL_ELF) || (echo "ERROR: kernel ELF missing or empty: $(KERNEL_ELF)" ; exit 1)
	$(QEMU) $(subst $(FAT32_IMG),$(GUI_FAT32_IMG),$(patsubst -nographic,-display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) $(QEMU_GUI_AUDIO) -serial stdio,$(QEMU_FLAGS))) -kernel $(KERNEL_ELF)

debug-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _debug_impl
debug-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _debug_impl
debug-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _debug_impl
debug-x86_64:
	$(MAKE) ARCH=x86_64 BRINGUP=$(BRINGUP) _debug_impl
debug-arm32:
	$(MAKE) ARCH=arm32 BRINGUP=$(BRINGUP) _debug_impl
debug-riscv32:
	$(MAKE) ARCH=riscv32 BRINGUP=$(BRINGUP) _debug_impl
debug-ppc64le:
	$(MAKE) ARCH=ppc64le BRINGUP=$(BRINGUP) _debug_impl
_debug_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 OPT="-O0 -g -DDEBUG" kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) OPT="-O0 -g -DDEBUG" dev-build
endif
	@echo "Waiting for GDB connection on port 1234..."
	@echo "=========================================================="
	@echo "Please run in another terminal:"
	@echo "  gdb-multiarch $(KERNEL_ELF)"
	@echo "  (gdb) target remote :1234"
	@echo "=========================================================="
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s

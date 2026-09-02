# Development launch, VirtualBox image, and GDB entry points.
#
# The per-arch run/debug targets are thin compatibility wrappers around the
# declarative instances in instances/*.toml (see docs/instances.md).  New
# configurations should be added as instance files and launched with
# `tools/a20 run <instance>`; the generic variable-driven `run` target and
# the *_impl targets below remain the execution engine shared by
# `make run ARCH=...` and tools/a20.

.PHONY: run _run_impl _run_gui_impl _debug_impl \
	vbox-gui-image-aarch64 _vbox_gui_image_aarch64_impl \
	run-nommu-riscv64 run-nommu-loongarch64 \
	run-nommu-aarch64 run-nommu-arm64 run-nommu-x86_64 \
	run-nommu-arm32 run-nommu-riscv32

# run-<arch> boots the text instance; BRINGUP=1 selects the -bringup variant.
define A20_RUN
	$(if $(filter 1,$(BRINGUP)),tools/a20 run qemu-$(1)-bringup,tools/a20 run qemu-$(1))
endef
define A20_RUN_GUI
	tools/a20 run qemu-$(1)-gui
endef
define A20_DEBUG
	$(if $(filter 1,$(BRINGUP)),tools/a20 debug qemu-$(1)-bringup,tools/a20 debug qemu-$(1))
endef

run:
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) _run_impl

run-riscv64:            ; $(call A20_RUN,riscv64)
run-gui-riscv64 run-gui-rv: ; $(call A20_RUN_GUI,riscv64)
run-loongarch64:        ; $(call A20_RUN,loongarch64)
run-gui-loongarch64 run-gui-la: ; $(call A20_RUN_GUI,loongarch64)
run-arm64:              ; $(call A20_RUN,aarch64)
run-gui-arm64 run-gui-aarch64: ; $(call A20_RUN_GUI,aarch64)
run-x86_64:             ; $(call A20_RUN,x86_64)
run-gui-x86_64:         ; $(call A20_RUN_GUI,x86_64)
run-arm32:              ; $(call A20_RUN,arm32)
run-gui-arm32:          ; $(call A20_RUN_GUI,arm32)
run-riscv32:            ; $(call A20_RUN,riscv32)
run-ppc64le:            ; $(call A20_RUN,ppc64le)

run-nommu-riscv64:      ; tools/a20 run qemu-riscv64-nommu
run-nommu-aarch64 run-nommu-arm64: ; tools/a20 run qemu-aarch64-nommu
run-nommu-arm32:        ; tools/a20 run qemu-arm32-nommu
run-gui-nommu-arm32 run-nommu-gui-arm32: ; tools/a20 run qemu-arm32-nommu-gui
run-nommu-riscv32:      ; tools/a20 run qemu-riscv32-nommu
run-nommu-loongarch64:
	@echo "run-nommu-loongarch64: skipped (LoongArch64 NOMMU is not supported)"
run-nommu-x86_64:
	@echo "run-nommu-x86_64: skipped (x86_64 NOMMU is not supported)"

debug-riscv64:    ; $(call A20_DEBUG,riscv64)
debug-loongarch64: ; $(call A20_DEBUG,loongarch64)
debug-arm64:      ; $(call A20_DEBUG,aarch64)
debug-x86_64:     ; $(call A20_DEBUG,x86_64)
debug-arm32:      ; $(call A20_DEBUG,arm32)
debug-riscv32:    ; $(call A20_DEBUG,riscv32)
debug-ppc64le:    ; $(call A20_DEBUG,ppc64le)

# Thin wrappers: VirtualBox image configurations live in instances/vbox-*.toml.
vbox-iso-x86_64:        ; tools/a20 package vbox-iso-x86_64
vbox-image-aarch64:     ; tools/a20 package vbox-aarch64
vbox-text-image-aarch64: ; tools/a20 package vbox-aarch64-text
vbox-gui-image-aarch64: ; tools/a20 package vbox-aarch64-gui

_vbox_iso_x86_64_impl: dev-build
	tools/mk_grub_iso.sh $(KERNEL_ELF) $(BUILD_DIR)/a20os-x86_64.iso
_vbox_image_aarch64_impl: $(VBOX_AARCH64_IMG)
	@echo "VirtualBox ARM64 image ready: $(VBOX_AARCH64_IMG)"
_vbox_text_image_aarch64_impl: $(VBOX_AARCH64_TEXT_IMG)
	@echo "VirtualBox ARM64 text image ready: $(VBOX_AARCH64_TEXT_IMG)"
_vbox_gui_image_aarch64_impl: $(VBOX_AARCH64_EFI) $(GUI_FAT32_IMG) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $(BUILD_DIR)/a20os-vbox-aarch64-gui.img $(GUI_FAT32_IMG)
	@echo "VirtualBox ARM64 GUI image ready: $(BUILD_DIR)/a20os-vbox-aarch64-gui.img"

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	# Text-mode boots never launch the LVGL desktop; skip building it so the
	# dev-build does not compile lvgl for a console run.
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) USER_BUILD_DESKTOP=0 dev-build
endif
	@test -s $(KERNEL_ELF) || (echo "ERROR: kernel ELF missing or empty: $(KERNEL_ELF)" ; exit 1)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

_run_gui_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) GUI_FAT32_IMAGE_MB=$(GUI_FAT32_IMAGE_MB) $(GUI_FAT32_IMG)
	@test -s $(KERNEL_ELF) || (echo "ERROR: kernel ELF missing or empty: $(KERNEL_ELF)" ; exit 1)
	$(QEMU) $(subst $(FAT32_IMG),$(GUI_FAT32_IMG),$(patsubst -nographic,-display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) $(QEMU_GUI_AUDIO) -serial stdio,$(QEMU_FLAGS))) -kernel $(KERNEL_ELF)

_debug_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 OPT="-O0 -g -DDEBUG" kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) OPT="-O0 -g -DDEBUG" USER_BUILD_DESKTOP=0 dev-build
endif
	@echo "Waiting for GDB connection on port 1234..."
	@echo "=========================================================="
	@echo "Please run in another terminal:"
	@echo "  gdb-multiarch $(KERNEL_ELF)"
	@echo "  (gdb) target remote :1234"
	@echo "=========================================================="
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s

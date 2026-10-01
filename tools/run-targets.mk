# Development launch, VirtualBox image, and GDB entry points.
#
# The per-arch run/debug targets are thin compatibility wrappers around the
# declarative instances in instances/*.toml (see docs/instances.md).  New
# configurations should be added as instance files and launched with
# `tools/a20 run <instance>`; the generic variable-driven `run` target and
# the *_impl targets below remain the execution engine shared by
# `make run ARCH=...` and tools/a20.

.PHONY: run _run_impl _debug_impl _qemu_argv _qemu_argv_debug \
	run-nommu-riscv64 run-nommu-loongarch64 \
	run-nommu-aarch64 run-nommu-arm64 run-nommu-x86_64 \
	run-nommu-arm32 run-nommu-riscv32

# run-<arch> boots the text instance; BRINGUP=1 selects the -bringup variant.
define A20_RUN
	$(if $(filter 1,$(BRINGUP)),tools/a20 run qemu-$(1)-bringup,tools/a20 run qemu-$(1))
endef
define A20_DEBUG
	$(if $(filter 1,$(BRINGUP)),tools/a20 debug qemu-$(1)-bringup,tools/a20 debug qemu-$(1))
endef

run:
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) _run_impl

run-riscv64:            ; $(call A20_RUN,riscv64)
run-loongarch64:        ; $(call A20_RUN,loongarch64)
run-arm64:              ; $(call A20_RUN,aarch64)
run-x86_64:             ; $(call A20_RUN,x86_64)
run-arm32:              ; $(call A20_RUN,arm32)
run-riscv32:            ; $(call A20_RUN,riscv32)
run-ppc64le:            ; $(call A20_RUN,ppc64le)

run-nommu-riscv64:      ; tools/a20 run qemu-riscv64-nommu
run-nommu-aarch64 run-nommu-arm64: ; tools/a20 run qemu-aarch64-nommu
run-nommu-arm32:        ; tools/a20 run qemu-arm32-nommu
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

_vbox_iso_x86_64_impl: dev-build
	tools/mk_grub_iso.sh $(KERNEL_ELF) $(BUILD_DIR)/a20os-x86_64.iso

# The same ISO at a path an instance can name in [target].boot_media.  The
# VirtualBox one lives under the per-build directory, whose name carries the arch,
# board and variant, so it cannot be written by a manifest that has to commit to
# one path.  This is the whole deployment route for a thin client: dd this to a
# USB stick and boot it.
pc-rescue-iso: dev-build
	@mkdir -p build/x86_64-pc
	tools/mk_grub_iso.sh $(KERNEL_ELF) build/x86_64-pc/a20os-rescue.iso
	@echo "rescue ISO ready: build/x86_64-pc/a20os-rescue.iso"

# A board with no block driver has no medium to write, so its artifact is the
# kernel plus the commands that hand it to the boot chain already on the board.
# The load address is read back out of the ELF by the script, so a board that
# relocates its image needs no entry here.
kernel-bundle: dev-build
	@mkdir -p build/$(BOARD)
	READELF="$(READELF)" tools/mk_kernel_bundle.sh \
		$(KERNEL_ELF) $(KERNEL_BIN) build/$(BOARD)/handoff $(BOARD)
_vbox_image_aarch64_impl: $(VBOX_AARCH64_IMG)
	@echo "VirtualBox ARM64 image ready: $(VBOX_AARCH64_IMG)"
_vbox_text_image_aarch64_impl: $(VBOX_AARCH64_TEXT_IMG)
	@echo "VirtualBox ARM64 text image ready: $(VBOX_AARCH64_TEXT_IMG)"

# The QEMU command line: make owns every flag, and these targets only expand
# them.  tools/qemu.py reads the result and does the running, so the launch
# sequence (build, verify, exec) is no longer shell inside a recipe.  They must
# stay free of prerequisites, or asking for the argv would rebuild the world.
_qemu_argv:
	@printf '%s\n' '$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)'

_qemu_argv_debug:
	@printf '%s\n' '$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s'

_run_impl:
	$(PYTHON) tools/qemu.py run --arch $(ARCH) --bringup $(BRINGUP)

_debug_impl:
	$(PYTHON) tools/qemu.py debug --arch $(ARCH) --bringup $(BRINGUP)

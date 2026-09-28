# ----------------------------------------------------------------
# Extra packages (vim / git / gcc) on a separate ext4 disk
# ----------------------------------------------------------------

.PHONY: _run_extra_impl extra-fetch-sources

RISCV_GCC_MUSL_LIBC ?= user/external/toolchain/musl-cross-make/output/riscv64-linux-musl/lib/libc.so

# A fresh clone does not materialize optional application gitlinks.  Fetch
# them before evaluating user/extra.mk so requested packages cannot be
# silently omitted.  GitHub uses SSH by default; set VF2_GIT_TRANSPORT=https
# on hosts without a configured SSH key.
extra-fetch-sources:
	tools/vf2/fetch-extra-sources.sh

extra-user-apps: extra-fetch-sources
	$(MAKE) ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" \
		PROFILE=$(PROFILE)  \
		$(USER_BUILD_STAMP)
	$(MAKE) -f user/extra.mk ARCH=$(ARCH) OPT="$(OPT)" \
		PACKAGES="$(EXTRA_PACKAGES)" \
		CA_CERT_BUNDLE="$(CA_CERT_BUNDLE)"

# Debian/Ubuntu cross toolchains already provide a complete target sysroot and
# return immediately here.  Fedora's cross GCC omits it, so bootstrap the
# target runtime from Fedora's official RISC-V repository into the project.
prepare-riscv64-glibc-sysroot:
	@$(PYTHON) tools/pkg.py prepare-riscv64-sysroot \
		--arch "$(ARCH)" \
		--extra-packages "$(EXTRA_PACKAGES)" \
		--lib-dir "$(RISCV_GLIBC_LIB_DIR)" \
		--local-root "$(RISCV_GLIBC_LOCAL_ROOT)" \
		--fedora-release "$(FEDORA_RISCV_RELEASE)"

force_extra_image_stamp:
	@:

$(EXTRA_IMAGE_STAMP): force_extra_image_stamp
	@$(PYTHON) tools/stamps.py extra-inputs \
		--stamp "$@" --arch "$(ARCH)" --nommu "$(NOMMU)" \
		--opt="$(OPT)" --profile "$(PROFILE)" \
		--user-variant "$(USER_VARIANT)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--extra-packages "$(EXTRA_PACKAGES)" \
		--extra-mb "$(EXTRA_IMAGE_MB)" --extra-img "$(EXTRA_IMG)" \
		--extra-dir "user/build/extra/$(ARCH)" \
		--riscv-gcc-musl-libc "$(RISCV_GCC_MUSL_LIBC)" \
		--riscv-glibc-lib-dir "$(RISCV_GLIBC_LIB_DIR)" \
		--riscv-glibc-local-lib-dir "$(RISCV_GLIBC_LOCAL_LIB_DIR)" \
		--ca-cert-bundle "$(CA_CERT_BUNDLE)" \
		--vim-runtime "user/external/apps/vim/runtime" \
		--git-templates "user/external/apps/git/templates/blt"
# Refresh the input manifest after package preparation, then let a second make
# decide from real timestamps whether the expensive image recipe is necessary.
extra-img: extra-user-apps prepare-riscv64-glibc-sysroot
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG="$(EXTRA_IMG)" "$(EXTRA_IMAGE_STAMP)"
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG="$(EXTRA_IMG)" _extra-img

_extra-img: $(EXTRA_IMG)

$(EXTRA_IMG): $(EXTRA_IMAGE_STAMP)
	@echo "Building extra packages image..."
	@$(PYTHON) tools/img.py extra \
		--extra-img "$(EXTRA_IMG)" --extra-mb "$(EXTRA_IMAGE_MB)" \
		--extra-staging-dir "$(EXTRA_STAGING_DIR)" \
		--extra-dir "user/build/extra/$(ARCH)" \
		--extra-packages "$(EXTRA_PACKAGES)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--mkfs-ext4 "$(MKFS_EXT4)" --arch "$(ARCH)" \
		--riscv-gcc-musl-libc "$(RISCV_GCC_MUSL_LIBC)" \
		--riscv-glibc-lib-dir "$(RISCV_GLIBC_LIB_DIR)" \
		--riscv-glibc-local-lib-dir "$(RISCV_GLIBC_LOCAL_LIB_DIR)" \
		--ca-cert-bundle "$(CA_CERT_BUNDLE)" \
		--extra-dns "$(EXTRA_DNS)"
# Helper: QEMU flags for the extra disk (appended conditionally)
ifeq ($(ARCH), riscv64)
# Slot 5 is reserved for the user-space virtio-input placement and is skipped
# by the kernel's VirtIO-MMIO enumerator.  Keep the extra disk on an otherwise
# unused slot so it is discovered and mounted at /extra during early boot.
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.1
else ifeq ($(ARCH), loongarch64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
else ifeq ($(ARCH), aarch64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), x86_64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
else ifeq ($(ARCH), arm32)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), riscv32)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), ppc64le)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
endif

run-riscv64-extra:
	$(MAKE) ARCH=riscv64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-loongarch64-extra:
	$(MAKE) ARCH=loongarch64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-arm64-extra:
	$(MAKE) ARCH=aarch64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-x86_64-extra:
	@echo "run-x86_64-extra: skipped (extra packages are not supported on x86_64)"

run-arm32-extra:
	@echo "run-arm32-extra: skipped (extra packages are not supported on ARM32)"

run-riscv32-extra:
	@echo "run-riscv32-extra: skipped (extra packages are not supported on RISC-V32)"

run-ppc64le-extra:
	@echo "run-ppc64le-extra: skipped (extra packages are not supported on PPC64LE)"

_run_extra_impl: extra-fetch-sources
	$(MAKE) ARCH=$(ARCH) BRINGUP=0 dev-build
	@if [ -f user/external/apps/fastfetch/src/fastfetch.c ]; then \
		$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" PROFILE=$(PROFILE) \
			BUILD_DIR=build/$(USER_VARIANT) fastfetch; \
	else \
		echo "[EXTRA] fastfetch source unavailable; skipping"; \
	fi
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG=$(EXTRA_IMG) extra-img
	$(QEMU) $(QEMU_FLAGS_NO_SDCARD) $(EXTRA_QEMU_BLK) -kernel $(KERNEL_ELF) \
		$(EXTRA_QEMU_APPEND)

EXTRA_DNS_riscv64 = 10.0.2.3
EXTRA_DNS = $(EXTRA_DNS_$(ARCH))
EXTRA_QEMU_APPEND_riscv64 = -append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=$(EXTRA_DNS_riscv64) a20.hostname=a20os'
EXTRA_QEMU_APPEND = $(EXTRA_QEMU_APPEND_$(ARCH))

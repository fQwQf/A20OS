# ----------------------------------------------------------------
# Extra packages (vim / git / gcc) on a separate ext4 disk
# ----------------------------------------------------------------

extra-user-apps:
	$(MAKE) -f user/extra.mk ARCH=$(ARCH) OPT="$(OPT)" PACKAGES="$(EXTRA_PACKAGES)"

# Debian/Ubuntu cross toolchains already provide a complete target sysroot and
# return immediately here.  Fedora's cross GCC omits it, so bootstrap the
# target runtime from Fedora's official RISC-V repository into the project.
prepare-riscv64-glibc-sysroot:
	@if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter rust rustc cargo rustfmt,$(EXTRA_PACKAGES))" ]; then \
		user/extra/prepare-riscv64-glibc-sysroot.sh \
			"$(RISCV_GLIBC_LIB_DIR)" "$(RISCV_GLIBC_LOCAL_ROOT)" "$(FEDORA_RISCV_RELEASE)"; \
	fi

force_extra_image_stamp:
	@:

$(EXTRA_IMAGE_STAMP): force_extra_image_stamp
	@set -e; \
	mkdir -p "$(dir $@)"; \
	tmp="$@.tmp"; \
	{ \
		printf '%s\n' \
			"arch=$(ARCH)" \
			"nommu=$(NOMMU)" \
			"opt=$(OPT)" \
			"profile=$(PROFILE)" \
			"user_variant=$(USER_VARIANT)" \
			"packages=$(sort $(EXTRA_PACKAGES))" \
			"image_mb=$(EXTRA_IMAGE_MB)" \
			"image=$(EXTRA_IMG)" \
			"glibc_dir=$(RISCV_GLIBC_LIB_DIR)" \
			"glibc_local_dir=$(RISCV_GLIBC_LOCAL_LIB_DIR)"; \
		for f in "$(USER_BUILD_DIR)"/*; do \
			[ -f "$$f" ] || continue; \
			name=$$(basename "$$f"); \
			case "$$name" in *.o|*.a|*.so|*.d) continue ;; esac; \
			find -H "$$f" -maxdepth 0 -printf 'user %f %s %T@\n'; \
		done; \
		for f in user/build/extra/$(ARCH)/*; do \
			[ -f "$$f" ] || continue; \
			name=$$(basename "$$f"); \
			case " $(EXTRA_PACKAGES) " in *" $$name "*) \
				find -H "$$f" -maxdepth 0 -printf 'extra %f %s %T@\n' ;; \
			esac; \
		done; \
		for package in $(sort $(EXTRA_PACKAGES)); do \
			case "$$package" in \
				vim) stamp=.vim-built ;; \
				git) stamp=.git-built ;; \
				gcc|cc) stamp=.gcc-built ;; \
				rust|rustc|cargo|rustfmt) stamp=.rust-built ;; \
				*) continue ;; \
			esac; \
			f="user/build/extra/$(ARCH)/stamp/$$stamp"; \
			[ ! -f "$$f" ] || find "$$f" -maxdepth 0 -printf 'stamp %f %s %T@\n'; \
		done; \
		if [ -n "$(filter vim,$(EXTRA_PACKAGES))" ]; then \
			find user/external/vim/runtime -type f -printf 'vim-runtime %P %s %T@\n' 2>/dev/null || true; \
		fi; \
		if [ -n "$(filter git,$(EXTRA_PACKAGES))" ]; then \
			find user/external/git/templates/blt -type f -printf 'git-template %P %s %T@\n' 2>/dev/null || true; \
		fi; \
		if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter rust rustc cargo rustfmt,$(EXTRA_PACKAGES))" ]; then \
			for dir in "$(RISCV_GLIBC_LIB_DIR)" "$(RISCV_GLIBC_LOCAL_LIB_DIR)"; do \
				[ -n "$$dir" ] || continue; \
				for name in ld-linux-riscv64-lp64d.so.1 libc.so.6 libdl.so.2 libm.so.6 \
					libpthread.so.0 librt.so.1 libatomic.so.1 libgcc_s.so.1; do \
					f="$$dir/$$name"; \
					[ ! -f "$$f" ] || find -H "$$f" -maxdepth 0 -printf 'glibc %p %s %T@\n'; \
				done; \
			done; \
		fi; \
		find Makefile user/extra.mk -maxdepth 0 -type f -printf 'recipe %p %s %T@\n'; \
	} | LC_ALL=C sort > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$@" "$$tmp"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
		echo "[EXTRA] image inputs changed"; \
	fi

# Refresh the input manifest after package preparation, then let a second make
# decide from real timestamps whether the expensive image recipe is necessary.
extra-img: extra-user-apps prepare-riscv64-glibc-sysroot
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG="$(EXTRA_IMG)" "$(EXTRA_IMAGE_STAMP)"
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG="$(EXTRA_IMG)" _extra-img

_extra-img: $(EXTRA_IMG)

$(EXTRA_IMG): $(EXTRA_IMAGE_STAMP)
	@echo "Building extra packages image..."
	@rm -rf $(EXTRA_STAGING_DIR) && mkdir -p $(EXTRA_STAGING_DIR)/bin
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		case "$$name" in \
			.build-id) continue ;; \
			*.o|*.a|*.so|*.d) continue ;; \
		esac; \
		cp "$$f" "$(EXTRA_STAGING_DIR)/bin/$$name"; \
	done; \
	for f in user/build/extra/$(ARCH)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		case " $(EXTRA_PACKAGES) " in *" $$name "*) cp "$$f" "$(EXTRA_STAGING_DIR)/bin/$$name" ;; esac; \
	done
	@set -e; \
	if [ -n "$(filter gcc cc,$(EXTRA_PACKAGES))" ] && [ -d user/build/extra/$(ARCH)/obj/gcc-install ]; then \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/libexec "$(EXTRA_STAGING_DIR)/libexec"; \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/lib "$(EXTRA_STAGING_DIR)/lib"; \
		for t in user/build/extra/$(ARCH)/obj/gcc-install/bin/*; do \
			[ -f "$$t" ] && cp "$$t" "$(EXTRA_STAGING_DIR)/bin/$$(basename $$t)"; \
		done; \
		mv "$(EXTRA_STAGING_DIR)/bin/gcc" "$(EXTRA_STAGING_DIR)/bin/gcc-real"; \
		printf '#!/bin/sh\nexec /test/bin/gcc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/gcc"; \
		mv "$(EXTRA_STAGING_DIR)/bin/cc" "$(EXTRA_STAGING_DIR)/bin/cc-real"; \
		printf '#!/bin/sh\nexec /test/bin/cc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cc"; \
	fi
	@if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter rust rustc cargo rustfmt,$(EXTRA_PACKAGES))" ]; then \
		RUST=user/build/extra/$(ARCH)/obj/rust; \
		[ -x "$$RUST/bin/rustc" ] && [ -x "$$RUST/bin/cargo" ] && \
			[ -x "$$RUST/bin/rustfmt" ] && [ -x "$$RUST/bin/cargo-fmt" ] || \
			{ echo "Rust installation incomplete in $$RUST"; exit 1; }; \
		GLIBC="$(RISCV_GLIBC_LIB_DIR)"; \
		REQUIRED_GLIBC="ld-linux-riscv64-lp64d.so.1 libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0 librt.so.1 libatomic.so.1 libgcc_s.so.1"; \
		MISSING_GLIBC=""; \
		for f in $$REQUIRED_GLIBC; do [ -n "$$GLIBC" ] && [ -f "$$GLIBC/$$f" ] || MISSING_GLIBC="$$MISSING_GLIBC $$f"; done; \
		if [ -n "$$MISSING_GLIBC" ]; then \
			GLIBC="$(RISCV_GLIBC_LOCAL_LIB_DIR)"; \
			MISSING_GLIBC=""; \
			for f in $$REQUIRED_GLIBC; do [ -f "$$GLIBC/$$f" ] || MISSING_GLIBC="$$MISSING_GLIBC $$f"; done; \
		fi; \
		[ -z "$$MISSING_GLIBC" ] || { \
			echo "RISC-V glibc runtime incomplete in '$$GLIBC'; missing:$$MISSING_GLIBC"; \
			echo "Install/provide the cross glibc runtime or set RISCV_GLIBC_LIB_DIR to a directory containing all required libraries"; \
			exit 1; \
		}; \
		cp -a "$$RUST" "$(EXTRA_STAGING_DIR)/rust"; \
		printf '#!/bin/sh\nexec /test/rust/bin/rustc --target riscv64gc-unknown-linux-musl -C linker=/test/rust/lib/rustlib/riscv64gc-unknown-linux-gnu/bin/rust-lld -C relocation-model=static -C link-arg=-L/test/rust/a20-sysroot/lib -C link-arg=-static -C link-arg=/test/rust/a20-sysroot/lib/crt1.o -C link-arg=/test/rust/a20-sysroot/lib/crti.o -C link-arg=/test/rust/a20-sysroot/lib/crtn.o "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/rustc"; \
		printf '#!/bin/sh\nexport RUSTC=/test/rust/bin/rustc\nexport CARGO_BUILD_TARGET=riscv64gc-unknown-linux-musl\nexec /test/rust/bin/cargo --config /test/rust/config.toml "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cargo"; \
		printf '#!/bin/sh\nexec /test/rust/bin/rustfmt "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/rustfmt"; \
		printf '#!/bin/sh\nexec /test/rust/bin/cargo-fmt "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cargo-fmt"; \
		printf '[target.riscv64gc-unknown-linux-musl]\nlinker = "/test/rust/lib/rustlib/riscv64gc-unknown-linux-gnu/bin/rust-lld"\nrustflags = ["-C", "relocation-model=static", "-C", "link-arg=-L/test/rust/a20-sysroot/lib", "-C", "link-arg=-static", "-C", "link-arg=/test/rust/a20-sysroot/lib/crt1.o", "-C", "link-arg=/test/rust/a20-sysroot/lib/crti.o", "-C", "link-arg=/test/rust/a20-sysroot/lib/crtn.o"]\n' > "$(EXTRA_STAGING_DIR)/rust/config.toml"; \
		chmod 0755 "$(EXTRA_STAGING_DIR)/bin/rustc" "$(EXTRA_STAGING_DIR)/bin/cargo" \
			"$(EXTRA_STAGING_DIR)/bin/rustfmt" "$(EXTRA_STAGING_DIR)/bin/cargo-fmt"; \
		mkdir -p "$(EXTRA_STAGING_DIR)/glibc/lib"; \
		for f in $$REQUIRED_GLIBC; do \
			cp -aL "$$GLIBC/$$f" "$(EXTRA_STAGING_DIR)/glibc/lib/$$f"; \
		done; \
	fi
	@VIM_RT="$(EXTRA_STAGING_DIR)/share/vim/vim92"; \
	VIM_SRC=user/external/vim/runtime; \
	if [ -z "$(filter vim,$(EXTRA_PACKAGES))" ] || [ ! -d "$$VIM_SRC" ]; then exit 0; fi; \
	mkdir -p "$$VIM_RT"; \
	for f in defaults.vim filetype.vim ftoff.vim ftplugin.vim ftplugof.vim indent.vim indoff.vim; do \
		[ -f "$$VIM_SRC/$$f" ] && cp "$$VIM_SRC/$$f" "$$VIM_RT/$$f"; \
	done; \
	for d in syntax indent ftplugin autoload; do \
		mkdir -p "$$VIM_RT/$$d"; \
		cp -a "$$VIM_SRC/$$d/." "$$VIM_RT/$$d/"; \
	done
	@GIT_TEMPLATE_SRC=user/external/git/templates/blt; \
	GIT_TEMPLATE_DST="$(EXTRA_STAGING_DIR)/share/git-core/templates"; \
	if [ -n "$(filter git,$(EXTRA_PACKAGES))" ] && [ -d "$$GIT_TEMPLATE_SRC" ]; then \
		mkdir -p "$$GIT_TEMPLATE_DST"; \
		cp -a "$$GIT_TEMPLATE_SRC"/. "$$GIT_TEMPLATE_DST"/; \
	fi
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXTRA_IMG) bs=1048576 count=$(EXTRA_IMAGE_MB) 2>/dev/null
	$(MKFS_EXT4) -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index \
		-d $(EXTRA_STAGING_DIR) $(EXTRA_IMG)
	@rm -rf $(EXTRA_STAGING_DIR)
	@echo "Extra image: $(EXTRA_IMG) ($(EXTRA_IMAGE_MB)MB)"

# Helper: QEMU flags for the extra disk (appended conditionally)
ifeq ($(ARCH), riscv64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
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
	$(MAKE) ARCH=x86_64 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-arm32-extra:
	$(MAKE) ARCH=arm32 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-riscv32-extra:
	$(MAKE) ARCH=riscv32 BRINGUP=0 PROFILE=benchmark _run_extra_impl

run-ppc64le-extra:
	$(MAKE) ARCH=ppc64le BRINGUP=0 PROFILE=benchmark _run_extra_impl

_run_extra_impl:
	$(MAKE) ARCH=$(ARCH) BRINGUP=0 dev-build
	@if [ -f user/external/fastfetch/src/fastfetch.c ]; then \
		$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" PROFILE=$(PROFILE) \
			BUILD_DIR=build/$(USER_VARIANT) fastfetch; \
	else \
		echo "[EXTRA] fastfetch source unavailable; skipping"; \
	fi
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG=$(EXTRA_IMG) extra-img
	$(QEMU) $(QEMU_FLAGS_NO_SDCARD) $(EXTRA_QEMU_BLK) -kernel $(KERNEL_ELF)


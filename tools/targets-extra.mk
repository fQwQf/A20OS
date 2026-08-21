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
		PROFILE=$(PROFILE) BUILD_DESKTOP=$(USER_BUILD_DESKTOP) \
		$(USER_BUILD_STAMP)
	$(MAKE) -f user/extra.mk ARCH=$(ARCH) OPT="$(OPT)" \
		PACKAGES="$(EXTRA_PACKAGES)" \
		CA_CERT_BUNDLE="$(CA_CERT_BUNDLE)"

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
	if [ -n "$(filter lamina,$(EXTRA_PACKAGES))" ]; then \
		for pat in 'liblaminaCore.so*' 'liblmcas.so*' 'liblmmc.so*' 'libLammpCore.so*' 'libstdc++.so*'; do \
			for f in user/build/extra/$(ARCH)/$$pat; do \
				[ -f "$$f" ] || continue; \
				find -H "$$f" -maxdepth 0 -printf 'extra %f %s %T@\n'; \
			done; \
		done; \
	fi; \
		for package in $(sort $(EXTRA_PACKAGES)); do \
			case "$$package" in \
				vim) stamp=.vim-built ;; \
				git) stamp=.git-built ;; \
				gcc|cc) stamp=.gcc-built ;; \
				rust|rustc|cargo|rustfmt) stamp=.rust-built ;; \
				lamina) stamp=.lamina-built ;; \
				*) continue ;; \
			esac; \
			f="user/build/extra/$(ARCH)/stamp/$$stamp"; \
			[ ! -f "$$f" ] || find "$$f" -maxdepth 0 -printf 'stamp %f %s %T@\n'; \
		done; \
		if [ -n "$(filter vim,$(EXTRA_PACKAGES))" ]; then \
			find user/external/apps/vim/runtime -type f -printf 'vim-runtime %P %s %T@\n' 2>/dev/null || true; \
		fi; \
		if [ -n "$(filter git,$(EXTRA_PACKAGES))" ]; then \
			find user/external/apps/git/templates/blt -type f -printf 'git-template %P %s %T@\n' 2>/dev/null || true; \
			for f in user/build/extra/$(ARCH)/git-remote-http user/build/extra/$(ARCH)/git-remote-https; do \
				[ ! -f "$$f" ] || find -H "$$f" -maxdepth 0 -printf 'git-helper %f %s %T@\n'; \
			done; \
			[ -z "$(CA_CERT_BUNDLE)" ] || find -L "$(CA_CERT_BUNDLE)" -maxdepth 0 -type f \
				-printf 'ca-bundle %p %s %T@\n'; \
		fi; \
		if [ "$(ARCH)" = riscv64 ] && [ -n "$(filter gcc cc,$(EXTRA_PACKAGES))" ]; then \
			find -H "$(RISCV_GCC_MUSL_LIBC)" -maxdepth 0 -type f \
				-printf 'gcc-musl-libc %p %s %T@\n'; \
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
	@rm -f "$(EXTRA_IMG)"
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
	done; \
	if [ -n "$(filter lamina,$(EXTRA_PACKAGES))" ]; then \
		for pat in 'liblaminaCore.so*' 'liblmcas.so*' 'liblmmc.so*' 'libLammpCore.so*' 'libstdc++.so*'; do \
			for f in user/build/extra/$(ARCH)/$$pat; do \
				[ -f "$$f" ] || continue; \
				cp -P "$$f" "$(EXTRA_STAGING_DIR)/bin/$$(basename "$$f")"; \
			done; \
		done; \
	fi
	@set -e; \
	if [ -n "$(filter gcc cc,$(EXTRA_PACKAGES))" ] && [ -d user/build/extra/$(ARCH)/obj/gcc-install ]; then \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/libexec "$(EXTRA_STAGING_DIR)/libexec"; \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/lib "$(EXTRA_STAGING_DIR)/lib"; \
		cp -a user/build/extra/$(ARCH)/obj/gcc-install/include "$(EXTRA_STAGING_DIR)/include"; \
		for t in user/build/extra/$(ARCH)/obj/gcc-install/bin/*; do \
			[ -f "$$t" ] && cp "$$t" "$(EXTRA_STAGING_DIR)/bin/$$(basename $$t)"; \
		done; \
		mv "$(EXTRA_STAGING_DIR)/bin/gcc" "$(EXTRA_STAGING_DIR)/bin/gcc-real"; \
		printf '#!/bin/sh\nexec /extra/bin/gcc-real --sysroot=/extra -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/gcc"; \
		mv "$(EXTRA_STAGING_DIR)/bin/cc" "$(EXTRA_STAGING_DIR)/bin/cc-real"; \
		printf '#!/bin/sh\nexec /extra/bin/cc-real --sysroot=/extra -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cc"; \
		chmod 0755 "$(EXTRA_STAGING_DIR)/bin/gcc" "$(EXTRA_STAGING_DIR)/bin/cc"; \
		if [ "$(ARCH)" = riscv64 ]; then \
			MUSL_LIBC="$(RISCV_GCC_MUSL_LIBC)"; \
			[ -f "$$MUSL_LIBC" ] || { \
				echo "[EXTRA] missing GCC musl runtime $$MUSL_LIBC"; exit 1; \
			}; \
			mkdir -p "$(EXTRA_STAGING_DIR)/musl/lib"; \
			cp "$$MUSL_LIBC" "$(EXTRA_STAGING_DIR)/musl/lib/libc.so"; \
		fi; \
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
		printf '#!/bin/sh\nexec /extra/rust/bin/rustc --target riscv64gc-unknown-linux-musl -C linker=/extra/rust/lib/rustlib/riscv64gc-unknown-linux-gnu/bin/rust-lld -C relocation-model=static -C link-arg=-L/extra/rust/a20-sysroot/lib -C link-arg=-static -C link-arg=/extra/rust/a20-sysroot/lib/crt1.o -C link-arg=/extra/rust/a20-sysroot/lib/crti.o -C link-arg=/extra/rust/a20-sysroot/lib/crtn.o "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/rustc"; \
		printf '#!/bin/sh\nexport RUSTC=/extra/rust/bin/rustc\nexport CARGO_BUILD_TARGET=riscv64gc-unknown-linux-musl\nexec /extra/rust/bin/cargo --config /extra/rust/config.toml "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cargo"; \
		printf '#!/bin/sh\nexec /extra/rust/bin/rustfmt "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/rustfmt"; \
		printf '#!/bin/sh\nexec /extra/rust/bin/cargo-fmt "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cargo-fmt"; \
		printf '[target.riscv64gc-unknown-linux-musl]\nlinker = "/extra/rust/lib/rustlib/riscv64gc-unknown-linux-gnu/bin/rust-lld"\nrustflags = ["-C", "relocation-model=static", "-C", "link-arg=-L/extra/rust/a20-sysroot/lib", "-C", "link-arg=-static", "-C", "link-arg=/extra/rust/a20-sysroot/lib/crt1.o", "-C", "link-arg=/extra/rust/a20-sysroot/lib/crti.o", "-C", "link-arg=/extra/rust/a20-sysroot/lib/crtn.o"]\n' > "$(EXTRA_STAGING_DIR)/rust/config.toml"; \
		chmod 0755 "$(EXTRA_STAGING_DIR)/bin/rustc" "$(EXTRA_STAGING_DIR)/bin/cargo" \
			"$(EXTRA_STAGING_DIR)/bin/rustfmt" "$(EXTRA_STAGING_DIR)/bin/cargo-fmt"; \
		mkdir -p "$(EXTRA_STAGING_DIR)/glibc/lib"; \
		for f in $$REQUIRED_GLIBC; do \
			cp -aL "$$GLIBC/$$f" "$(EXTRA_STAGING_DIR)/glibc/lib/$$f"; \
		done; \
	fi
	@VIM_RT="$(EXTRA_STAGING_DIR)/share/vim/vim92"; \
	VIM_SRC=user/external/apps/vim/runtime; \
	if [ -z "$(filter vim,$(EXTRA_PACKAGES))" ] || [ ! -d "$$VIM_SRC" ]; then exit 0; fi; \
	mkdir -p "$$VIM_RT"; \
	for f in defaults.vim filetype.vim ftoff.vim ftplugin.vim ftplugof.vim indent.vim indoff.vim; do \
		[ -f "$$VIM_SRC/$$f" ] && cp "$$VIM_SRC/$$f" "$$VIM_RT/$$f"; \
	done; \
	for d in syntax indent ftplugin autoload; do \
		mkdir -p "$$VIM_RT/$$d"; \
		cp -a "$$VIM_SRC/$$d/." "$$VIM_RT/$$d/"; \
	done
	@GIT_TEMPLATE_SRC=user/external/apps/git/templates/blt; \
	GIT_TEMPLATE_DST="$(EXTRA_STAGING_DIR)/share/git-core/templates"; \
	if [ -n "$(filter git,$(EXTRA_PACKAGES))" ]; then \
		if [ -d "$$GIT_TEMPLATE_SRC" ]; then \
			mkdir -p "$$GIT_TEMPLATE_DST"; \
			cp -a "$$GIT_TEMPLATE_SRC"/. "$$GIT_TEMPLATE_DST"/; \
		fi; \
		for helper in git-remote-http git-remote-https; do \
			src="user/build/extra/$(ARCH)/$$helper"; \
			[ -x "$$src" ] || { echo "[EXTRA] missing Git HTTPS helper $$src"; exit 1; }; \
			cp "$$src" "$(EXTRA_STAGING_DIR)/bin/$$helper"; \
		done; \
		[ -n "$(CA_CERT_BUNDLE)" ] && [ -f "$(CA_CERT_BUNDLE)" ] || { \
			echo "[EXTRA] no host CA certificate bundle found; set CA_CERT_BUNDLE"; exit 1; \
		}; \
		mkdir -p "$(EXTRA_STAGING_DIR)/etc/ssl/certs"; \
		cp -L "$(CA_CERT_BUNDLE)" \
			"$(EXTRA_STAGING_DIR)/etc/ssl/certs/ca-certificates.crt"; \
		if [ -n "$(EXTRA_DNS)" ]; then \
			printf 'nameserver %s\noptions timeout:2 attempts:3\n' "$(EXTRA_DNS)" \
				> "$(EXTRA_STAGING_DIR)/etc/resolv.conf"; \
		fi; \
	fi
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXTRA_IMG) bs=1048576 count=$(EXTRA_IMAGE_MB) 2>/dev/null
	$(MKFS_EXT4) -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index \
		-d $(EXTRA_STAGING_DIR) $(EXTRA_IMG)
	@rm -rf $(EXTRA_STAGING_DIR)
	@echo "Extra image: $(EXTRA_IMG) ($(EXTRA_IMAGE_MB)MB)"

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

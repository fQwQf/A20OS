# ----------------------------------------------------------------
# Retired: the source-built "extra" disk
# ----------------------------------------------------------------
# The extra disk used to be a second ext4 image holding user software
# compiled from the vendored trees in user/external/ -- vim, git, zlib,
# a curl+mbedtls pair, a binutils+GCC Canadian cross, rust and
# Lamina1.  All of those now come from Alpine packages assembled by a
# world manifest, so that whole path is gone: the vendored trees are
# deleted, `user/extra.mk` is deleted, and `tools/img.py extra` is gone.
#
# What survives is only the *partition*: a real board still needs a
# writable ext4 at /extra, and an apk world image is a drop-in for it
# because mkrootfs.py already emits a partitionless raw ext4.
#
# These stubs stay deliberately.  Without them `make run-riscv64-extra`
# fails with "no rule to make target", which teaches nothing; with them
# the old incantation fails with the exact replacement, so the habit
# breaks with a pointer instead of a puzzle.

.PHONY: extra-img _extra-img extra-user-apps extra-fetch-sources \
	prepare-riscv64-glibc-sysroot force_extra_image_stamp \
	vf2-extra

# Building the old source-built disk.
extra-img _extra-img extra-user-apps force_extra_image_stamp:
	@echo "[extra] '$@' was removed: there is no source-built extra disk any more." >&2
	@echo "[extra] An Alpine world image replaces it:" >&2
	@echo "[extra]   make ARCH=$(ARCH) image-world PKG_WORLD=devel" >&2
	@echo "[extra] 'devel' carries musl busybox vim git curl ca-certificates less." >&2
	@echo "[extra] Add PKG_WORLD=devtools for gcc and musl-dev." >&2
	@echo "[extra] See docs/packaging/images.md for the world list." >&2
	@exit 2

# The glibc sysroot only ever existed so the vendored glibc-linked
# packages could run; apk packages ship their own loader.
prepare-riscv64-glibc-sysroot:
	@echo "[extra] '$@' was removed with the source-built extra disk." >&2
	@echo "[extra] Alpine packages bring their own dynamic loader." >&2
	@exit 2

# Only fastfetch is still a gitlink, because it is the one program built
# into the FAT32 root, which no Alpine world replaces.
extra-fetch-sources:
	@echo "[extra] '$@' was removed: vim, git, zlib, gcc, binutils and" >&2
	@echo "[extra] musl-cross-make are gone from the tree." >&2
	@echo "[extra] Use: git submodule update --init user/external/apps/fastfetch" >&2
	@exit 2

# Running QEMU with the old source-built disk.  One explicit multi-target rule
# rather than run-%-extra: these names are declared .PHONY in
# tools/targets-base.mk, and .PHONY suppresses pattern-rule lookup, so a stem
# pattern here would silently do nothing.  The arch comes out of $@ instead.
run-riscv64-extra run-loongarch64-extra run-aarch64-extra run-arm64-extra \
run-x86_64-extra run-arm32-extra run-riscv32-extra run-ppc64le-extra:
	@arch="$@"; arch="$${arch#run-}"; arch="$${arch%-extra}"; \
	echo "[extra] '$@' was removed: it booted a disk compiled from vendored source." >&2; \
	echo "[extra] Use the apk world for the same software:" >&2; \
	echo "[extra]   make ARCH=$$arch run-world PKG_WORLD=devel" >&2; \
	echo "[extra] 'devel' = musl busybox vim git curl ca-certificates less" >&2; \
	echo "[extra] 'devtools' adds gcc musl-dev git fastfetch" >&2; \
	echo "[extra] 'xfce' boots the desktop (tools/a20 run xfce-$$arch)" >&2; \
	exit 2

# The full VF2 card whose only distinguishing feature was the source-built
# disk.  vf2-sdcard now stages an apk world image in that partition.
vf2-extra:
	@echo "[extra] 'vf2-extra' was removed: the full card was defined by its" >&2
	@echo "[extra] source-built extra.img, and that build path is gone." >&2
	@echo "[extra] Use: make vf2-sdcard VF2_WORLD=devel" >&2
	@echo "[extra] VF2_WORLD selects the apk world for the /extra partition;" >&2
	@echo "[extra] it defaults to 'devel' (vim/git/curl/less)." >&2
	@exit 2
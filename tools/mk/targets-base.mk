# ================================================================
# Targets
# ================================================================

.PHONY: print-driver-deployment
.PHONY: all clean run-riscv64 run-loongarch64 run-arm64 run-x86_64 vbox-iso-x86_64 _vbox_iso_x86_64_impl vbox-image-aarch64 _vbox_image_aarch64_impl vbox-text-image-aarch64 _vbox_text_image_aarch64_impl run-arm32 run-riscv32 debug-riscv64 debug-loongarch64 debug-arm64 debug-x86_64 debug-arm32 debug-riscv32 stm32f103-bringup stm32f103-xuanwu flash-stm32f103-xuanwu run-stm32f103-qemu check-stm32f103 check-kernel-build check-kernel-build-all check-user-build check-user-build-all check-dev-build check-release-build check-build-matrix check-build-matrix-all check-upgrade-userland-smokes check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup check-arm32-bringup check-riscv32-bringup check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user check-arm32-user check-riscv32-user FORCE regen-rootfs-overlay check-rootfs-overlay-generator kernel-only dev-build extra-img _extra-img extra-user-apps prepare-riscv64-glibc-sysroot force_extra_image_stamp run-riscv64-extra run-loongarch64-extra run-arm64-extra run-x86_64-extra run-arm32-extra run-riscv32-extra native-handle-test-arch native-libc-arch native-programs native-futex-arch native-debug-test-arch native-ext-test-arch mlibc-sysroot mlibc-hello-rv mlibc-sbase mlibc-mksh native-ipc-arch native-ipc-la native-contract-arch native-contract-la native-cluster-arch native-cluster-la native-uinputd-arch native-uinputd-rv native-uedud-arch native-uedud-rv native-personality-arch native-personality-rv native-abi-behavior-arch native-abi-behavior-rv native-linux-arch native-linux-rv native-svc-arch native-svc-rv native-rust-idl-peer-arch native-rust-idl-peer-rv native-rust-idl-peer native-shmring-arch native-shmring-rv native-rtcd-arch native-rtcd-rv native-registry-arch native-registry-rv native-isolation-arch native-isolation-rv native-ubd-arch native-ubd-rv native-ufsd-arch native-ufsd-rv native-handle-test-rv native-handle-test-la native-handle-test-aarch64 native-handle-test-x86_64 native-handle-test-arm32 native-handle-test-rv32 native-handle-test native-handle-test-all native-libc-rv native-libc-la native-libc-aarch64 native-libc-x86_64 native-libc-arm32 native-libc-rv32 native-libc native-libc-all native-futex-rv native-futex-la native-futex-aarch64 native-futex-x86_64 native-futex-arm32 native-futex-rv32 native-futex-all native-debug-test-rv native-debug-test-la native-debug-test-aarch64 native-debug-test-x86_64 native-debug-test-arm32 native-debug-test-rv32 native-debug-test-all native-ext-test-rv native-ext-test-la native-ext-test-aarch64 native-ext-test-x86_64 native-ext-test-arm32 native-ext-test-rv32 native-ext-test-all native-mm-rv native-mm-la native-mm-aarch64 native-mm-x86_64 native-mm-arm32 native-mm-rv32 native-mm-all native-signal-rv native-signal-la native-signal-aarch64 native-signal-x86_64 native-signal-arm32 native-signal-rv32 native-signal-all native-ipc-rv native-ipc-la native-ipc-aarch64 native-ipc-x86_64 native-ipc-arm32 native-ipc-rv32 native-ipc-all native-contract-rv native-contract-la native-contract-aarch64 native-contract-x86_64 native-contract-arm32 native-contract-rv32 native-contract-all native-cluster-rv native-cluster-la native-cluster-aarch64 native-cluster-x86_64 native-cluster-arm32 native-cluster-rv32 native-cluster-all

FORCE:

print-driver-deployment:
	@printf 'ARCH=%s BOARD=%s DRIVER_DEPLOYMENT=%s build_dir=%s modules=%s builtin_device_drivers=%s\n' \
		'$(ARCH)' '$(BOARD)' '$(DRIVER_DEPLOYMENT)' '$(BUILD_DIR)' '$(words $(DRVMOD_MODULES))' \
		'$(words $(EMBEDDED_DEVICE_DRIVER_SRCS))'

$(BUILD_TIME_HDR):
	@mkdir -p $(dir $@)
	@printf '#ifndef A20_BUILD_UNIX_TIME\n#define A20_BUILD_UNIX_TIME %sULL\n#endif\n' "$$(date -u +%s)" > $@

regen-rootfs-overlay: tools/packaging/gen_rootfs_overlay.py $(ROOTFS_OVERLAY_FILES)
	@mkdir -p $(dir $(ROOTFS_OVERLAY_SRC)) $(dir $(ROOTFS_OVERLAY_HDR))
	$(PYTHON) $< --out-c $(ROOTFS_OVERLAY_SRC) --out-h $(ROOTFS_OVERLAY_HDR) --root $(ROOTFS_OVERLAY_DIR)

# These generated files are checked in so archive builds do not depend on Python.
# Regenerate them explicitly: checkout mtimes are not a reliable dependency order.
check-rootfs-overlay-generator:
	@$(PYTHON) tools/gensync.py rootfs-overlay \
		--committed "$(ROOTFS_OVERLAY_HDR)" \
		--out-name rootfs_overlay.h \
		--also $(ROOTFS_OVERLAY_SRC) \
		-- $(PYTHON) tools/packaging/gen_rootfs_overlay.py \
			--out-c @TMP@/rootfs_overlay.c \
			--out-h @TMP@/rootfs_overlay.h \
			--root $(ROOTFS_OVERLAY_DIR)

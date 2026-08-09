# ================================================================
# Targets
# ================================================================

.PHONY: all final-all final-submit-rv final-submit-la preliminary-all all-architectures clean run-riscv64 run-gui-riscv64 run-gui-rv run-loongarch64 run-gui-loongarch64 run-gui-la run-arm64 run-gui-arm64 run-gui-aarch64 run-x86_64 run-gui-x86_64 vbox-iso-x86_64 _vbox_iso_x86_64_impl vbox-image-aarch64 _vbox_image_aarch64_impl vbox-text-image-aarch64 _vbox_text_image_aarch64_impl run-arm32 run-gui-arm32 run-riscv32 run-ppc64le debug-riscv64 debug-loongarch64 debug-arm64 debug-x86_64 debug-arm32 debug-riscv32 debug-ppc64le \
		run-gui-nommu-arm32 run-nommu-gui-arm32 \
		stm32f103-bringup stm32f103-xuanwu flash-stm32f103-xuanwu run-stm32f103-qemu \
		check-stm32f103 \
		check-kernel-build check-kernel-build-all check-user-build check-user-build-all check-dev-build check-contest-build check-contest-build-all check-build-matrix check-build-matrix-all check-abi-smoke-gate check-doc-drift check-doc-test-gates check-final-definition check-concurrency-foundation check-mm-lock-model check-abi-boundary check-driver-core-model check-external-dependency-boundary \
		check-arch-boundary check-task-state-boundary \
		check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup check-arm32-bringup check-riscv32-bringup check-ppc64le-bringup \
		check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user check-arm32-user check-riscv32-user check-ppc64le-user \
		smoke-riscv64 smoke-loongarch64 smoke-aarch64 smoke-x86_64 smoke-qemu-gui-x86_64 smoke-qemu-gui-riscv64 smoke-qemu-gui-aarch64 smoke-qemu-gui-arm32 smoke-qemu-gui-loongarch64 smoke-arm32 smoke-riscv32 smoke-ppc64le smoke-abi-linux smoke-a20-channel smoke-ptrace smoke-network-suite smoke-proc-a20 smoke-proc-stress smoke-procfs-stress smoke-mm-stress smoke-mm-fork-exec-race smoke-vfs-stress smoke-vfs-edge smoke-sched-stress smoke-futex-stress smoke-socket-stress smoke-driver-lifecycle smoke-drvmod smoke-drvmod-riscv64 smoke-drvmod-x86_64 smoke-drvmod-aarch64 smoke-drvmod-loongarch64 smoke-hda smoke-audio-userspace smoke-virtio-sound smoke-pci-portability smoke-native-handle smoke-native-libc smoke-native-futex smoke-io-event smoke-signalfd-stress smoke-evdev-stress smoke-scm-stress \
		smoke-arch-mmu-matrix \
		FORCE regen-rootfs-overlay \
		user_apps fs_img kernel-only dev-build contest-rv contest-la \
		eval-dev-build-rv eval-dev-build-la \
		qemu-disk-rv qemu-disk-la \
		extra-img _extra-img extra-user-apps prepare-riscv64-glibc-sysroot force_extra_image_stamp run-riscv64-extra run-loongarch64-extra run-arm64-extra run-x86_64-extra run-arm32-extra run-riscv32-extra run-ppc64le-extra \
		native-test-arch native-handle-test-arch native-libc-arch native-programs \
	native-futex-arch native-futex-rv smoke-native-futex native-debug-test-arch native-debug-test-rv smoke-native-debug native-ext-test-arch native-ext-test-rv smoke-native-ext mlibc-sysroot mlibc-hello-rv smoke-mlibc \
		native-ipc-arch native-ipc-rv native-ipc-la smoke-native-ipc \
		native-contract-arch native-contract-rv native-contract-la smoke-native-contract \
		native-uinputd-arch native-uinputd-rv smoke-dual-input \
		native-personality-arch native-personality-rv smoke-native-personality \
		native-linux-arch native-linux-rv smoke-native-linux \
		check-a20-idl \
		smoke-iommu-discovery \
		native-svc-arch native-svc-rv smoke-native-svc \
		native-shmring-arch native-shmring-rv smoke-native-shmring \
		native-rtcd-arch native-rtcd-rv smoke-native-rtcd \
		native-registry-arch native-registry-rv smoke-native-registry \
		native-isolation-arch native-isolation-rv smoke-native-isolation \
		native-ubd-arch native-ubd-rv smoke-native-ubd \
		smoke-clock-vdso \
		native-test-rv native-test-la native-test-aarch64 native-test-x86_64 native-test-arm32 native-test-rv32 native-test-ppc64le native-test native-test-all \
		native-minimal-rv native-minimal-la native-minimal \
		native-handle-test-rv native-handle-test-la native-handle-test-aarch64 native-handle-test-x86_64 native-handle-test-arm32 native-handle-test-rv32 native-handle-test-ppc64le native-handle-test native-handle-test-all \
		native-libc-rv native-libc-la native-libc-aarch64 native-libc-x86_64 native-libc-arm32 native-libc-rv32 native-libc-ppc64le native-libc native-libc-all \
		eval eval-all eval-rv eval-la \
		final-eval-rv-cagent final-eval-la-cagent \
		final-eval-rv-buildstorm final-eval-la-buildstorm \
		final-probe-rv-buildstorm-1c final-probe-rv-buildstorm-8c \
		final-probe-la-buildstorm-1c final-probe-la-buildstorm-8c \
		final-stage6-rv-ext4-dir-tail final-stage6-la-ext4-dir-tail \
		final-stage6-rv-helper final-stage6-la-helper \
		final-stage7-rv-shebang final-stage7-la-shebang \
		final-stage7-rv-rustc-j8 final-stage7-la-rustc-j8 \
		final-stage7-rv-rustc-llvm-j8 final-stage7-la-rustc-llvm-j8 \
		final-stage7-rv-1c-j1 final-stage7-la-1c-j1 \
		final-stage7-rv-8c-j1 final-stage7-la-8c-j1 \
		final-stage7-rv-8c-j2 final-stage7-la-8c-j2 \
		final-stage7-rv-8c-j4 final-stage7-la-8c-j4 \
		final-stage7-rv-8c-j8 final-stage7-la-8c-j8 \
		final-stage7-rv-8c-default final-stage7-la-8c-default \
		final-stage9-rv-1c-perf final-stage9-rv-8c-perf \
		final-stage9-la-1c-perf final-stage9-la-8c-perf

FORCE:

print-driver-deployment:
	@printf 'ARCH=%s BOARD=%s DRIVER_DEPLOYMENT=%s build_dir=%s modules=%s builtin_device_drivers=%s\n' \
		'$(ARCH)' '$(BOARD)' '$(DRIVER_DEPLOYMENT)' '$(BUILD_DIR)' '$(words $(DRVMOD_MODULES))' \
		'$(words $(EMBEDDED_DEVICE_DRIVER_SRCS))'

$(BUILD_TIME_HDR):
	@mkdir -p $(dir $@)
	@printf '#ifndef A20_BUILD_UNIX_TIME\n#define A20_BUILD_UNIX_TIME %sULL\n#endif\n' "$$(date -u +%s)" > $@

regen-rootfs-overlay: tools/gen_rootfs_overlay.py $(ROOTFS_OVERLAY_FILES)
	@mkdir -p $(dir $(ROOTFS_OVERLAY_SRC)) $(dir $(ROOTFS_OVERLAY_HDR))
	$(PYTHON) $< --out-c $(ROOTFS_OVERLAY_SRC) --out-h $(ROOTFS_OVERLAY_HDR) --root $(ROOTFS_OVERLAY_DIR)

$(ROOTFS_OVERLAY_SRC) $(ROOTFS_OVERLAY_HDR): tools/gen_rootfs_overlay.py $(ROOTFS_OVERLAY_FILES)
	$(PYTHON) $< --out-c $(ROOTFS_OVERLAY_SRC) --out-h $(ROOTFS_OVERLAY_HDR) --root $(ROOTFS_OVERLAY_DIR)

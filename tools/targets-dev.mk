# ----------------------------------------------------------------
# Development build (for `make run-riscv64` / `make run-loongarch64`)
# ----------------------------------------------------------------

dev-build: $(KERNEL_BIN) $(USER_BUILD_STAMP) $(FS_TEST_IMG) $(EXT4_IMG) $(ISOFS_IMG)
	@echo "Dev build complete: $(KERNEL_BIN), $(FAT32_IMG), $(EXT4_IMG), $(ISOFS_IMG)"

user_apps: $(USER_BUILD_STAMP)

.PHONY: user_apps

.PHONY: force_user_build force_vbox_rootfs_verify
force_user_build:
	@:

force_vbox_rootfs_verify:
	@:

.PHONY: force_native_build
force_native_build:
	@:

$(USER_BUILD_STAMP): user/Makefile force_user_build | $(USER_BUILD_CHECK_DIRS)
	@$(PYTHON) tools/stamps.py user \
		--stamp "$(USER_BUILD_STAMP)" --build-id "$(USER_BUILD_ID)" \
		--arch "$(ARCH)" --nommu "$(NOMMU)" \
		--user-build-dir "$(USER_BUILD_DIR)" --user-variant "$(USER_VARIANT)" \
		--user-opt="$(USER_OPT)" --opt="$(OPT)" --profile "$(PROFILE)" \
		--roots "user/Makefile $(USER_BUILD_CHECK_DIRS)" \
		--skip "*/.git user/build user/external/musl/build-*"
$(NATIVE_BUILD_STAMP): $(USER_BUILD_STAMP) force_native_build
	@$(PYTHON) tools/stamps.py native \
		--stamp "$(NATIVE_BUILD_STAMP)" --build-id "$(USER_BUILD_ID)" \
		--arch "$(ARCH)" --nommu "$(NOMMU)" --opt="$(OPT)" \
		--binaries "$(NATIVE_BINS)" \
		--roots "user/liba20rt user/liba20c user/tests user/svc user/svc/fscompat kernel/fs/diskfs kernel/include/drivers/dual"
fs_img: $(FS_TEST_IMG)

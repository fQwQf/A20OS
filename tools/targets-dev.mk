# ----------------------------------------------------------------
# Development build (for `make run-riscv64` / `make run-loongarch64`)
# ----------------------------------------------------------------

dev-build: $(KERNEL_BIN) $(USER_BUILD_STAMP) $(FS_TEST_IMG) $(EXT4_IMG)
	@echo "Dev build complete: $(KERNEL_BIN), $(FAT32_IMG), $(EXT4_IMG)"

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
	@set -e; \
	mkdir -p $(dir $@); \
	current=""; \
	if [ -f "$@" ]; then current=$$(cat "$@"); fi; \
	need_build=0; \
	need_clean=0; \
	if [ "$$current" != "$(USER_BUILD_ID)" ]; then \
		need_build=1; \
		need_clean=1; \
	elif [ ! -x "$(USER_BUILD_DIR)/init" ] || [ ! -x "$(USER_BUILD_DIR)/mksh" ]; then \
		need_build=1; \
	elif find user/Makefile $(USER_BUILD_CHECK_DIRS) \
		\( -path '*/.git' -o -path 'user/build' -o -path 'user/external/musl/build-*' \) -prune -o \
		-type f -newer "$@" -print -quit | grep -q .; then \
		need_build=1; \
	fi; \
	if [ "$$need_build" -eq 1 ]; then \
		if [ "$$need_clean" -eq 1 ]; then \
			$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" \
				PROFILE=$(PROFILE) BUILD_DIR=build/$(USER_VARIANT) clean; \
		fi; \
		$(MAKE) -C user ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(USER_OPT)" PROFILE=$(PROFILE) BUILD_DESKTOP=$(USER_BUILD_DESKTOP) \
			BUILD_DIR=build/$(USER_VARIANT); \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[USER] $(USER_BUILD_ID) up to date"; \
	fi

$(NATIVE_BUILD_STAMP): $(USER_BUILD_STAMP) force_native_build
	@set -e; \
	need_build=0; \
	current=""; \
	if [ -f "$@" ]; then current=$$(cat "$@"); fi; \
	if [ "$$current" != "$(USER_BUILD_ID)" ]; then \
		need_build=1; \
	elif [ ! -x "$(NATIVE_HELLO_BIN)" ] || [ ! -x "$(NATIVE_HANDLE_BIN)" ] || \
	     [ ! -x "$(NATIVE_LIBC_BIN)" ] || [ ! -x "$(NATIVE_FUTEX_BIN)" ] || \
	     [ ! -x "$(NATIVE_MM_BIN)" ] || [ ! -x "$(NATIVE_SIGNAL_BIN)" ] || \
	     [ ! -x "$(NATIVE_IPC_BIN)" ] || [ ! -x "$(NATIVE_CONTRACT_BIN)" ] || \
	     [ ! -x "$(NATIVE_SVCMAN_BIN)" ] || \
	     [ ! -x "$(NATIVE_SHMRING_BIN)" ] || [ ! -x "$(NATIVE_SHMRINGD_BIN)" ] || \
	     [ ! -x "$(NATIVE_CHAND_BIN)" ] || [ ! -x "$(NATIVE_ECHOD_BIN)" ] || \
	     [ ! -x "$(NATIVE_REGISTRY_BIN)" ] || [ ! -x "$(NATIVE_SVCMGR_BIN)" ] || \
	     [ ! -x "$(NATIVE_ISOLATION_BIN)" ] || \
	     [ ! -x "$(NATIVE_UBDD_BIN)" ] || [ ! -x "$(NATIVE_UINPUTD_BIN)" ] || \
	     [ ! -x "$(NATIVE_PERSONALITY_BIN)" ] || \
	     [ ! -x "$(NATIVE_LINUX_BIN)" ] || \
	     [ ! -x "$(NATIVE_RTCD_BIN)" ] || [ ! -x "$(NATIVE_RTCDD_BIN)" ]; then \
		need_build=1; \
	elif find user/liba20rt user/liba20c user/tests user/svc kernel/include/drivers/dual -type f -newer "$@" \
		-print -quit | grep -q .; then \
		need_build=1; \
	fi; \
	if [ "$$need_build" -eq 1 ]; then \
		$(MAKE) ARCH=$(ARCH) NOMMU=$(NOMMU) OPT="$(OPT)" native-programs; \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[NATIVE] $(USER_BUILD_ID) up to date"; \
	fi

fs_img: $(FS_TEST_IMG)


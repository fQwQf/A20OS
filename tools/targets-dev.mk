# ----------------------------------------------------------------
# Development build (for `make run-riscv64` / `make run-loongarch64`)
# ----------------------------------------------------------------

dev-build: $(KERNEL_BIN) $(USER_BUILD_STAMP) $(FS_TEST_IMG) $(EXT4_IMG) $(ISOFS_IMG)
	@echo "Dev build complete: $(KERNEL_BIN), $(FAT32_IMG), $(EXT4_IMG), $(ISOFS_IMG)"

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
		--envelope-acquire-race-test "$(ENVELOPE_ACQUIRE_RACE_TEST)" \
		--syscall-smoke-task-construction-test "$(SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST)" \
		--trap-storm-test "$(TRAP_STORM_TEST)" \
		--roots "user/Makefile $(USER_BUILD_CHECK_DIRS)" \
		--skip "*/.git user/build user/external/musl/build-*"
$(NATIVE_BUILD_STAMP): $(USER_BUILD_STAMP) force_native_build
	@$(PYTHON) tools/stamps.py native \
		--stamp "$(NATIVE_BUILD_STAMP)" --build-id "$(USER_BUILD_ID)" \
		--arch "$(ARCH)" --nommu "$(NOMMU)" --opt="$(OPT)" \
		--native-rust-peer "$(NATIVE_RUST_PEER)" \
		--native-rust-idl-service "$(NATIVE_RUST_IDL_SERVICE)" \
		--native-rust-echod "$(NATIVE_RUST_ECHOD)" \
		--native-rust-svcmgr "$(NATIVE_RUST_SVCMGR)" \
		--native-rust-rtcd "$(NATIVE_RUST_RTCD)" \
		--native-rust-ufsd "$(NATIVE_RUST_UFSD)" \
		--native-rust-ufsd-ext4-orphan "$(NATIVE_RUST_UFSD_EXT4_ORPHAN)" \
		--native-rust-ufsd-ext4-extent-txn "$(NATIVE_RUST_UFSD_EXT4_EXTENT_TXN)" \
		--native-rust-ufsd-test-ext4-trunc-free-fail-once "$(NATIVE_RUST_UFSD_TEST_EXT4_TRUNC_FREE_FAIL_ONCE)" \
		--native-rust-ufsd-test-close-orphan-write-fail-once "$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE)" \
		--native-rust-ufsd-test-close-orphan-write-fail-after "$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER)" \
		--native-rust-ufsd-test-close-orphan-restore-fail "$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL)" \
		--native-rust-ufsd-test-close-token-drop-ack-once "$(NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--rust-peer-binary "$(RUST_USER_PEER_BIN)" \
		--rust-service-binary "$(RUST_USER_SERVICE_BIN)" \
		--rust-service-test-binary "$(RUST_USER_SERVICE_TEST_BIN)" \
		--rust-echod-binary "$(RUST_USER_SERVICE_BIN)" \
		--rust-svcmgr-binary "$(RUST_USER_SVCMGR_BIN)" \
		--rust-rtcd-binary "$(RUST_USER_RTCD_BIN)" \
		--rust-ufsd-binary "$(RUST_USER_UFSD_BIN)" \
		--rust-ufsd-test-close-token-binary "$(RUST_USER_UFSD_TEST_CLOSE_TOKEN_BIN)" \
		--rust-ufsd-test-ext4-trunc-free-bin "$(RUST_USER_UFSD_TEST_EXT4_TRUNC_FREE_BIN)" \
		--binaries "$(NATIVE_BINS)" \
		--roots "user/liba20rt user/liba20c user/tests user/svc user/svc/fscompat kernel/fs/diskfs kernel/include/drivers/dual kernel/include/uapi/a20 tools/targets-native.mk tools/targets-dev.mk tools/rust-user.mk $(if $(filter 1,$(NATIVE_RUST_PEER)),user/svc/rust_idl_peer.rs user/svc/idl_negotiation.rs user/svc/rust_peer_crt0.c user/svc/a20_services.idl tools/a20idl.py) $(if $(filter 1,$(NATIVE_RUST_IDL_SERVICE)),user/svc/rust_idl_echod.rs user/tests/test_native_rust_idl_service.c) $(if $(filter 1,$(NATIVE_RUST_ECHOD)),user/svc/rust_idl_echod.rs user/svc/a20_services.idl tools/a20idl.py) $(if $(filter 1,$(NATIVE_RUST_SVCMGR)),user/svc/svcmgr.rs user/svc/svcmgr_lifecycle.rs user/svc/svcmgr_rust_abi.c user/svc/svcmgr_rust_abi.h user/svc/a20_services.idl tools/a20idl.py) $(if $(filter 1,$(NATIVE_RUST_RTCD)),user/svc/rtcd.rs user/svc/rtcd_rust_abi.c user/svc/rtcd_rust_abi.h user/svc/a20_services.idl tools/a20idl.py) $(if $(filter 1,$(NATIVE_RUST_UFSD)),user/svc/ufsd.rs user/svc/ufsd_protocol.rs user/svc/ufsd_rust_abi.c user/svc/ufsd_rust_abi.h user/svc/ufs_backends.h user/svc/ufs_fat_backend.c user/svc/ufs_vnfs_backend.c user/svc/fscompat kernel/fs/diskfs tools/a20idl.py user/svc/a20_services.idl)"

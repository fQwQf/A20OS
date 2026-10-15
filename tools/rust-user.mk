# User-space Rust object integration for the Native ABI.
#
# tools/rust-kernel.mk builds freestanding Rust objects for the kernel:
# `--target <triple> --crate-type lib --emit=obj`, then the C link picks the
# object up.  A user-space peer needs exactly the same treatment and nothing
# more: rustc compiles ONE static object, and the existing Native svc recipe
# links it against crt0, liba20rt and the syscall ABI exactly like
# user/svc/echod.c.  There is no hosted target, no libc, no allocator and no
# Cargo manifest anywhere in this file, so nothing here depends on rustc's own
# linker or on a crate registry.
#
# The Rust IDL peer, adversary, and service support the installed RV64 and
# AArch64 freestanding targets.  Their protocol logic is shared; the narrow C
# syscall shim delegates register placement and trap behavior to the Native
# SDK for each target.  Other Rust user services keep their own architecture
# guards until an ABI-matched target and runtime evidence are available.
#
# The object lives under $(USER_BUILD_DIR)/rust/ rather than beside the binary:
# tools/img.py:build_fat32() copies every *file* of $(USER_BUILD_DIR) into the
# FAT32 image, so the .o must stay in a subdirectory to keep the image
# byte-comparable.
#
# Include this file AFTER tools/targets-native.mk: it consumes NATIVE_CC,
# NATIVE_CFLAGS, NATIVE_LIBS, NATIVE_LD, NATIVE_SDK_SRC, NATIVE_COMPILER_RT_SRC,
# NATIVE_ARCH_SRC, NATIVE_SDK_HDRS, NATIVE_TAG and USER_BUILD_DIR.

RUSTC ?= rustc
NATIVE_RUST_PEER ?= 0
NATIVE_RUST_IDL_SERVICE ?= 0
NATIVE_RUST_ECHOD ?= 0
NATIVE_RUST_SVCMGR ?= 0
NATIVE_RUST_RTCD ?= 0
NATIVE_RUST_UFSD ?= 0
# Keep the recovery record parser independently switchable for paired C/Rust
# format comparisons while the surrounding UFSD remains Rust-owned.
NATIVE_RUST_UFSD_JBD2_RECORD ?= 1
# Keep the userspace service fixed while selecting the orphan policy owner,
# so C/Rust comparisons differ only at the ext4 orphan-controller boundary.
NATIVE_RUST_UFSD_EXT4_ORPHAN ?= 1
# Independently select the bounded depth-0 extent-root trim controller.
# This keeps the Rust UFSD protocol/lifetime constant in C/Rust comparisons.
NATIVE_RUST_UFSD_EXT4_EXTENT_TXN ?= 1
# Test-only: drop exactly one ACK after accepting a tokenized CLOSE, so a
# guest can retry the same token and verify the server's replay response.
NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE ?= 0
# Test-only: fail the first real block write made while reaping an orphan
# during final close, then allow the Rust OP_SYNC retry to complete.
NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE ?= 0
# When the close-orphan fail-once profile is enabled, inject on this numbered
# backend block write during one reap. 1 retains the original single-write
# fixture; larger values exercise rollback after completed frees.
NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER ?= 1
# Test-only: fail the selected orphan-reap write and the immediately following
# compensation write, to verify fatal status/quarantine handling.
NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL ?= 0

# The isolated IDL service fixture also builds the Rust peer used by its
# generated-code C/Rust contract checks. Production Rust echod can be selected
# independently as the existing runtime service.
ifeq ($(NATIVE_RUST_IDL_SERVICE),1)
override NATIVE_RUST_PEER := 1
endif

RUST_USER_PEER_SRC := user/svc/rust_idl_peer.rs
RUST_USER_ACK_ADVERSARY_SRC := user/svc/rust_idl_adversary.rs
RUST_USER_SERVICE_SRC := user/svc/rust_idl_echod.rs
RUST_USER_NEGOTIATION_SRC := user/svc/idl_negotiation.rs
RUST_USER_CRT0_SRC := user/svc/rust_peer_crt0.c
RUST_USER_OBJ_DIR  := $(USER_BUILD_DIR)/rust
RUST_USER_PEER_OBJ := $(RUST_USER_OBJ_DIR)/rust_idl_peer.o
RUST_USER_ACK_TEST_OBJ := $(RUST_USER_OBJ_DIR)/rust_idl_peer_ack_test.o
RUST_USER_SERVICE_OBJ := $(RUST_USER_OBJ_DIR)/rust_idl_echod.o
RUST_USER_PEER_BIN := $(USER_BUILD_DIR)/rust-idl-peer-$(NATIVE_TAG)
RUST_USER_ACK_TEST_BIN := $(USER_BUILD_DIR)/rust-idl-peer-ack-$(NATIVE_TAG)
RUST_USER_ACK_ADVERSARY_OBJ := $(RUST_USER_OBJ_DIR)/rust_idl_adversary.o
RUST_USER_ACK_ADVERSARY_BIN := $(USER_BUILD_DIR)/rust-idl-adversary-$(NATIVE_TAG)
RUST_USER_SERVICE_BIN := $(USER_BUILD_DIR)/rust-idl-echod-$(NATIVE_TAG)
RUST_USER_SERVICE_TEST_BIN := $(USER_BUILD_DIR)/native-rust-idl-service-$(NATIVE_TAG)
RUST_USER_SVCMGR_SRC := user/svc/svcmgr.rs
RUST_USER_SVCMGR_LIFECYCLE_SRC := user/svc/svcmgr_lifecycle.rs
RUST_USER_SVCMGR_ABI_SRC := user/svc/svcmgr_rust_abi.c
RUST_USER_SVCMGR_ABI_HDR := user/svc/svcmgr_rust_abi.h
RUST_USER_SVCMGR_OBJ := $(RUST_USER_OBJ_DIR)/svcmgr.o
# Keep the Rust build as a separately inspectable artifact.  The dev-image
# selector in stamps.py installs this ELF at the established svcmgr-rv name
# only after C and Rust native-programs have both linked successfully.
RUST_USER_SVCMGR_BIN := $(USER_BUILD_DIR)/svcmgr-rust-$(NATIVE_TAG)
RUST_USER_RTCD_SRC := user/svc/rtcd.rs
RUST_USER_RTCD_ABI_SRC := user/svc/rtcd_rust_abi.c
RUST_USER_RTCD_ABI_HDR := user/svc/rtcd_rust_abi.h
RUST_USER_RTCD_OBJ := $(RUST_USER_OBJ_DIR)/rtcd.o
RUST_USER_RTCD_BIN := $(USER_BUILD_DIR)/rtcd-rust-$(NATIVE_TAG).a20drv
RUST_USER_UFSD_SRC := user/svc/ufsd.rs
RUST_USER_UFSD_SUPERVISION_SRC := user/svc/ufsd_supervision.rs
RUST_USER_UFSD_ABI_SRC := user/svc/ufsd_rust_abi.c
RUST_USER_UFSD_ABI_HDR := user/svc/ufsd_rust_abi.h
RUST_USER_UFSD_PROTOCOL_SRC := user/svc/ufsd_protocol.rs
RUST_USER_UFSD_CACHE_SRC := user/svc/fscompat/bcache_user.rs
RUST_USER_UFSD_CACHE_ABI_SRC := user/svc/fscompat/bcache_user_rust_abi.c
RUST_USER_UFSD_JBD2_POLICY_SRC := user/svc/jbd2_legacy_policy.rs
RUST_USER_UFSD_JBD2_TXN_SRC := user/svc/jbd2_txn.rs
RUST_USER_UFSD_JBD2_RECORD_SRC := user/svc/jbd2_record.rs
RUST_USER_UFSD_EXT4_ORPHAN_SRC := user/svc/ext4_orphan.rs
RUST_USER_UFSD_EXTENT_TXN_SRC := kernel/fs/diskfs/ext4_extent_txn.rs
RUST_USER_UFSD_MOUNT_STATE_SRC := kernel/fs/diskfs/ext4_mount_state.rs
RUST_USER_UFSD_TEST_PROFILE := $(if $(filter 1,$(NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE)),test-drop-close-token-ack,$(if $(filter 1,$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE)),$(if $(filter 1,$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL)),test-close-orphan-restore-w$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER),test-close-orphan-w$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER)),prod))
RUST_USER_UFSD_ORPHAN_PROFILE := $(if $(filter 1,$(NATIVE_RUST_UFSD_EXT4_ORPHAN)),orphan-rust,orphan-c)
RUST_USER_UFSD_EXTENT_PROFILE := $(if $(filter 1,$(NATIVE_RUST_UFSD_EXT4_EXTENT_TXN)),extent-rust,extent-c)
# Keep diagnostic and production objects/binaries separate. Switching the
# fail-once profile must never reuse an object compiled with a different cfg.
RUST_USER_UFSD_OBJ := $(RUST_USER_OBJ_DIR)/ufsd-$(RUST_USER_UFSD_ORPHAN_PROFILE)-$(RUST_USER_UFSD_EXTENT_PROFILE)-$(RUST_USER_UFSD_TEST_PROFILE).o
RUST_USER_UFSD_BIN := $(USER_BUILD_DIR)/ufsd-rust-$(NATIVE_TAG)-$(RUST_USER_UFSD_ORPHAN_PROFILE)-$(RUST_USER_UFSD_EXTENT_PROFILE)$(if $(filter prod,$(RUST_USER_UFSD_TEST_PROFILE)),,-$(RUST_USER_UFSD_TEST_PROFILE))
RUST_USER_UFSD_TEST_CLOSE_TOKEN_BIN := $(USER_BUILD_DIR)/ufsd-rust-$(NATIVE_TAG)-$(RUST_USER_UFSD_ORPHAN_PROFILE)-$(RUST_USER_UFSD_EXTENT_PROFILE)-test-drop-close-token-ack
RUST_USER_IDL_RUST := $(RUST_USER_OBJ_DIR)/a20_services_idl.rs

# Shared build helpers used by both the opt-in peer and the supervisor.
$(RUST_USER_IDL_RUST): user/svc/a20_services.idl user/liba20rt/a20_types.h tools/a20idl.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/a20idl.py user/svc/a20_services.idl /dev/null --rust-output $@

define RUST_USER_PEER_RECIPE
@mkdir -p $(dir $(1)) $(dir $(2))
$(3) -ffreestanding -nostdlib -static \
	$(4) \
	-Iuser -Iuser/liba20rt \
	-T$(NATIVE_LD) \
	$(5) \
	$(NATIVE_SDK_SRC) \
	$(NATIVE_COMPILER_RT_SRC) \
	$(NATIVE_ARCH_SRC) \
	$(6) \
	$(2) \
	$(NATIVE_LIBS) \
	-o $(1)
endef

RUST_USER_TARGET_riscv64 := riscv64gc-unknown-none-elf
RUST_USER_TARGET_aarch64 := aarch64-unknown-none
RUST_USER_TARGET_riscv32 := $(abspath tools/rust-targets/riscv32imafdc-a20.json)
RUST_USER_TARGET_x86_64 := x86_64-unknown-none
RUST_USER_TARGET := $(RUST_USER_TARGET_$(ARCH))
RUST_USER_TARGET_LIBDIR := $(shell $(RUSTC) --print target-libdir --target $(RUST_USER_TARGET) 2>/dev/null)
RUST_USER_LIBCORE := $(firstword $(wildcard $(RUST_USER_TARGET_LIBDIR)/libcore-*.rlib))
RUST_USER_LIBCOMPILER_BUILTINS := $(firstword $(wildcard $(RUST_USER_TARGET_LIBDIR)/libcompiler_builtins-*.rlib))

# Match the tree's medany user code model for riscv64.
RUST_USER_CODE_MODEL_riscv64 := medium
RUST_USER_CODE_MODEL_aarch64 := small
RUST_USER_CODE_MODEL_riscv32 := medium
RUST_USER_CODE_MODEL_x86_64 := small
RUST_USER_CODE_MODEL := $(RUST_USER_CODE_MODEL_$(ARCH))

# This experimental binary is opt-in. Including this fragment must not alter
# C-only builds, `clean`, or native outputs for other architectures.
ifneq ($(filter native-rust-idl-peer-arch,$(MAKECMDGOALS)),)
ifneq ($(NATIVE_RUST_PEER),1)
$(error native-rust-idl-peer-arch requires NATIVE_RUST_PEER=1; use native-rust-idl-peer)
endif

ifeq ($(filter $(ARCH),riscv64 riscv32 aarch64 x86_64),)
$(error Rust Native IDL peer requires ARCH=riscv64, riscv32, aarch64, or x86_64 (got $(ARCH)))
endif
endif

ifneq ($(filter 1,$(NATIVE_RUST_PEER) $(NATIVE_RUST_ECHOD) $(NATIVE_RUST_IDL_SERVICE) $(NATIVE_RUST_SVCMGR)),)
ifneq ($(filter 1,$(NATIVE_RUST_PEER)),)
ifeq ($(filter $(ARCH),riscv64 riscv32 aarch64 x86_64),)
$(error NATIVE_RUST_PEER=1 requires ARCH=riscv64, riscv32, aarch64, or x86_64 (got $(ARCH)))
endif
endif
ifneq ($(filter 1,$(NATIVE_RUST_ECHOD)),)
ifeq ($(filter $(ARCH),riscv64 riscv32 aarch64 x86_64),)
$(error NATIVE_RUST_ECHOD requires ARCH=riscv64, riscv32, aarch64, or x86_64 (got $(ARCH)))
endif
endif
ifneq ($(filter 1,$(NATIVE_RUST_IDL_SERVICE)),)
ifeq ($(filter $(ARCH),riscv64 riscv32 aarch64 x86_64),)
$(error NATIVE_RUST_IDL_SERVICE requires ARCH=riscv64, riscv32, aarch64, or x86_64 (got $(ARCH)))
endif
endif

# Do not silently turn an unsupported/debug optimization option into a
# different build. The no_std user peer currently supports only these levels.
RUST_USER_SUPPORTED_OPTS := -O1 -O2 -O3 -Os
ifeq ($(strip $(USER_OPT)),)
$(error Rust user peer requires an explicit supported USER_OPT value)
endif
ifneq ($(filter $(USER_OPT),$(RUST_USER_SUPPORTED_OPTS)),$(USER_OPT))
$(error Rust user peer does not support USER_OPT='$(USER_OPT)' (supported: $(RUST_USER_SUPPORTED_OPTS)))
endif
RUST_USER_OPT_LEVEL := $(patsubst -O%,%,$(USER_OPT))

ifeq ($(NATIVE_RUST_PEER),1)
NATIVE_OUTPUTS += $(RUST_USER_PEER_BIN)
native-programs: $(RUST_USER_PEER_BIN)
endif

# RV32 Native uses ILP32D, while rustc's built-in riscv32 target uses
# ILP32F. Build a matching core in this private USER_BUILD_DIR before compiling
# any of the IDL peer/service objects.
ifeq ($(ARCH),riscv32)
RUST_USER_RV32_TARGET := $(abspath tools/rust-targets/riscv32imafdc-a20.json)
RUST_USER_RV32_TARGET_NAME := riscv32imafdc-a20
RUST_USER_RV32_SYSROOT := $(RUST_USER_OBJ_DIR)/rv32-sysroot
RUST_USER_RV32_CARGO_TARGET_DIR := $(RUST_USER_OBJ_DIR)/rv32-core-cargo
RUST_USER_RV32_CORE_LIBDIR := $(RUST_USER_RV32_SYSROOT)/lib/rustlib/$(RUST_USER_RV32_TARGET_NAME)/lib
RUST_USER_RV32_CORE_FLAGS := $(RUST_USER_OBJ_DIR)/.rv32-core-flags
RUST_USER_RV32_CORE_READY := $(RUST_USER_OBJ_DIR)/.rv32-core-ready
RUST_USER_TARGET_BOOTSTRAP := RUSTC_BOOTSTRAP=1
RUST_USER_TARGET_OBJECT_FLAGS := -Z unstable-options --sysroot $(RUST_USER_RV32_SYSROOT)
.PHONY: FORCE_RUST_USER_RV32_CORE_FLAGS
FORCE_RUST_USER_RV32_CORE_FLAGS:
$(RUST_USER_RV32_CORE_FLAGS): FORCE_RUST_USER_RV32_CORE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' 'rustc=$(shell $(RUSTC) --version --verbose 2>/dev/null)' 'cargo=$(shell $(CARGO) --version 2>/dev/null)' 'target=$(shell sha256sum $(RUST_USER_RV32_TARGET) 2>/dev/null)' 'rust-src=$(shell sysroot=`$(RUSTC) --print sysroot 2>/dev/null`; find "$$sysroot/lib/rustlib/src/rust/library" -type f -print0 2>/dev/null | sort -z | xargs -0 sha256sum 2>/dev/null | sha256sum | cut -d' ' -f1)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; rm -f $(RUST_USER_RV32_CORE_READY); else rm -f $@.tmp; fi
$(RUST_USER_RV32_CORE_READY): $(RUST_USER_RV32_TARGET) tools/rust-target-core/Cargo.toml tools/rust-target-core/Cargo.lock tools/rust-target-core/src/lib.rs $(RUST_USER_RV32_CORE_FLAGS)
	@mkdir -p $(RUST_USER_RV32_CORE_LIBDIR) $(RUST_USER_RV32_CARGO_TARGET_DIR)
	RUSTC=$(RUSTC) RUSTC_BOOTSTRAP=1 CARGO_TARGET_DIR=$(RUST_USER_RV32_CARGO_TARGET_DIR) $(CARGO) clean --manifest-path tools/rust-target-core/Cargo.toml --target $(RUST_USER_RV32_TARGET)
	RUSTC=$(RUSTC) RUSTC_BOOTSTRAP=1 CARGO_TARGET_DIR=$(RUST_USER_RV32_CARGO_TARGET_DIR) $(CARGO) build --manifest-path tools/rust-target-core/Cargo.toml --target $(RUST_USER_RV32_TARGET) -Z json-target-spec -Z build-std=core --release
	cp $(RUST_USER_RV32_CARGO_TARGET_DIR)/$(RUST_USER_RV32_TARGET_NAME)/release/deps/libcore-*.rlib $(RUST_USER_RV32_CORE_LIBDIR)/libcore.rlib
	cp $(RUST_USER_RV32_CARGO_TARGET_DIR)/$(RUST_USER_RV32_TARGET_NAME)/release/deps/libcompiler_builtins-*.rlib $(RUST_USER_RV32_CORE_LIBDIR)/libcompiler_builtins.rlib
	cp $(RUST_USER_RV32_CORE_FLAGS) $(RUST_USER_RV32_CORE_READY)
else
RUST_USER_TARGET_BOOTSTRAP :=
RUST_USER_TARGET_OBJECT_FLAGS :=
endif

RUST_USER_PEER_FLAGS := $(RUST_USER_TARGET_BOOTSTRAP) $(RUSTC) --edition=2021 --target $(RUST_USER_TARGET) $(RUST_USER_TARGET_OBJECT_FLAGS) \
	-C panic=abort -C opt-level=$(RUST_USER_OPT_LEVEL) \
	-C codegen-units=1 -C relocation-model=static \
	-C code-model=$(RUST_USER_CODE_MODEL)

RUST_USER_PEER_FLAGS_STAMP := $(RUST_USER_OBJ_DIR)/.rust-idl-peer-flags
.PHONY: FORCE_RUST_USER_PEER_FLAGS
FORCE_RUST_USER_PEER_FLAGS:

$(RUST_USER_PEER_FLAGS_STAMP): FORCE_RUST_USER_PEER_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s %s\n' '$(RUST_USER_PEER_FLAGS)' \
		'$(shell $(RUSTC) --version 2>/dev/null)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_USER_PEER_OBJ): $(RUST_USER_PEER_SRC) $(RUST_USER_NEGOTIATION_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk $(RUST_USER_PEER_FLAGS_STAMP) $(RUST_USER_RV32_CORE_READY)
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUST_USER_TARGET_BOOTSTRAP) $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) $(RUST_USER_TARGET_OBJECT_FLAGS) \
		-C panic=abort -C opt-level=$(RUST_USER_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static \
		-C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@

$(RUST_USER_PEER_BIN): $(RUST_USER_PEER_OBJ) $(RUST_USER_CRT0_SRC) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		$(NATIVE_LD) user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		$(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) tools/rust-user.mk
	$(call RUST_USER_PEER_RECIPE,$@,$(RUST_USER_PEER_OBJ),$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$(RUST_USER_CRT0_SRC))

# Separate test-profile client: malformed-ACK adversarial cases are never
# part of the ordinary Native peer smoke binary.
$(RUST_USER_ACK_TEST_OBJ): $(RUST_USER_PEER_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk $(RUST_USER_PEER_FLAGS_STAMP) $(RUST_USER_RV32_CORE_READY)
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUST_USER_TARGET_BOOTSTRAP) $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--cfg idl_ack_test --target $(RUST_USER_TARGET) $(RUST_USER_TARGET_OBJECT_FLAGS) -C panic=abort -C opt-level=$(RUST_USER_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@

$(RUST_USER_ACK_TEST_BIN): $(RUST_USER_ACK_TEST_OBJ) $(RUST_USER_CRT0_SRC) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		$(NATIVE_LD) user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		$(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) tools/rust-user.mk
	$(call RUST_USER_PEER_RECIPE,$@,$(RUST_USER_ACK_TEST_OBJ),$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$(RUST_USER_CRT0_SRC))

# Test-only malicious wire peer for the IDL acknowledgment guest matrix.
# It is built only by the explicit adversarial target and is never added to
# normal Native outputs or production images.
$(RUST_USER_ACK_ADVERSARY_OBJ): $(RUST_USER_ACK_ADVERSARY_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk $(RUST_USER_PEER_FLAGS_STAMP) $(RUST_USER_RV32_CORE_READY)
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUST_USER_TARGET_BOOTSTRAP) $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) $(RUST_USER_TARGET_OBJECT_FLAGS) -C panic=abort -C opt-level=$(RUST_USER_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@

$(RUST_USER_ACK_ADVERSARY_BIN): $(RUST_USER_ACK_ADVERSARY_OBJ) $(RUST_USER_CRT0_SRC) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		$(NATIVE_LD) user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		$(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) tools/rust-user.mk
	$(call RUST_USER_PEER_RECIPE,$@,$(RUST_USER_ACK_ADVERSARY_OBJ),$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$(RUST_USER_CRT0_SRC))

$(RUST_USER_SERVICE_OBJ): $(RUST_USER_SERVICE_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk $(RUST_USER_PEER_FLAGS_STAMP) $(RUST_USER_RV32_CORE_READY)
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUST_USER_TARGET_BOOTSTRAP) $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) $(RUST_USER_TARGET_OBJECT_FLAGS) \
		-C panic=abort -C opt-level=$(RUST_USER_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static \
		-C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@

$(RUST_USER_SERVICE_BIN): $(RUST_USER_SERVICE_OBJ) $(RUST_USER_CRT0_SRC) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		$(NATIVE_LD) user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		$(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) tools/rust-user.mk
	$(call RUST_USER_PEER_RECIPE,$@,$(RUST_USER_SERVICE_OBJ),$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$(RUST_USER_CRT0_SRC))

$(RUST_USER_SERVICE_TEST_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_rust_idl_service.c $(A20_SERVICES_IDL_HDR) \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h $(NATIVE_SDK_HDRS)
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_rust_idl_service.c,$@)

ifneq ($(filter 1,$(NATIVE_RUST_ECHOD) $(NATIVE_RUST_IDL_SERVICE)),)
NATIVE_OUTPUTS += $(RUST_USER_SERVICE_BIN)
native-programs: $(RUST_USER_SERVICE_BIN)
endif

ifeq ($(NATIVE_RUST_IDL_SERVICE),1)
NATIVE_OUTPUTS += $(RUST_USER_SERVICE_TEST_BIN)
native-programs: $(RUST_USER_SERVICE_TEST_BIN)
.PHONY: native-rust-idl-service
native-rust-idl-service: $(RUST_USER_SERVICE_BIN) $(RUST_USER_SERVICE_TEST_BIN)
else
.PHONY: native-rust-idl-service
native-rust-idl-service:
	@echo "native-rust-idl-service requires NATIVE_RUST_IDL_SERVICE=1 and ARCH=riscv64 or aarch64" >&2
	@exit 2
endif

endif # NATIVE_RUST_PEER || NATIVE_RUST_ECHOD || NATIVE_RUST_IDL_SERVICE

ifeq ($(NATIVE_RUST_ECHOD),1)
.PHONY: native-rust-echod
native-rust-echod: $(RUST_USER_SERVICE_BIN)
else
.PHONY: native-rust-echod
native-rust-echod:
	@echo "native-rust-echod requires NATIVE_RUST_ECHOD=1 and ARCH=riscv64, riscv32, or aarch64" >&2
	@exit 2
endif

ifeq ($(NATIVE_RUST_SVCMGR),1)
ifeq ($(filter $(ARCH),riscv64 riscv32 aarch64 x86_64),)
$(error NATIVE_RUST_SVCMGR=1 requires ARCH=riscv64, riscv32, aarch64, or x86_64 (got $(ARCH)))
endif
ifneq ($(strip $(filter $(USER_OPT),-O1 -O2 -O3 -Os)),$(USER_OPT))
$(error Rust svcmgr requires USER_OPT in -O1 -O2 -O3 -Os (got '$(USER_OPT)'))
endif

RUST_USER_SVCMGR_OPT_LEVEL := $(patsubst -O%,%,$(USER_OPT))
RUST_USER_SVCMGR_FLAGS_STAMP := $(RUST_USER_OBJ_DIR)/.rust-svcmgr-flags
.PHONY: FORCE_RUST_USER_SVCMGR_FLAGS
FORCE_RUST_USER_SVCMGR_FLAGS:

$(RUST_USER_SVCMGR_FLAGS_STAMP): FORCE_RUST_USER_SVCMGR_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s %s\n' '$(RUSTC) --target $(RUST_USER_TARGET) -C panic=abort -C opt-level=$(RUST_USER_SVCMGR_OPT_LEVEL) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USER_CODE_MODEL)' '$(shell $(RUSTC) --version 2>/dev/null)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_USER_SVCMGR_OBJ): $(RUST_USER_SVCMGR_SRC) $(RUST_USER_SVCMGR_LIFECYCLE_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk $(RUST_USER_SVCMGR_FLAGS_STAMP) $(if $(filter riscv32,$(ARCH)),$(RUST_USER_RV32_CORE_READY))
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUST_USER_TARGET_BOOTSTRAP) $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) $(RUST_USER_TARGET_OBJECT_FLAGS) -C panic=abort -C opt-level=$(RUST_USER_SVCMGR_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@

$(RUST_USER_SVCMGR_BIN): $(RUST_USER_SVCMGR_OBJ) $(RUST_USER_CRT0_SRC) $(RUST_USER_SVCMGR_ABI_SRC) $(RUST_USER_SVCMGR_ABI_HDR) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		$(NATIVE_LD) $(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) tools/rust-user.mk
	$(call RUST_USER_PEER_RECIPE,$@,$(RUST_USER_SVCMGR_OBJ),$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$(RUST_USER_CRT0_SRC) $(RUST_USER_SVCMGR_ABI_SRC))

NATIVE_OUTPUTS += $(RUST_USER_SVCMGR_BIN)
native-programs: $(RUST_USER_SVCMGR_BIN)
.PHONY: native-rust-svcmgr
native-rust-svcmgr: $(RUST_USER_SVCMGR_BIN)
else
.PHONY: native-rust-svcmgr
native-rust-svcmgr:
	@echo "native-rust-svcmgr requires NATIVE_RUST_SVCMGR=1 and a supported Native architecture" >&2
	@exit 2
endif
.PHONY: native-rust-svcmgr-rv32 native-rust-svcmgr-aarch64 native-rust-svcmgr-x86_64
native-rust-svcmgr-rv32:
	$(MAKE) -j2 ARCH=riscv32 NOMMU=$(NOMMU) NATIVE_RUST_SVCMGR=1 native-rust-svcmgr
native-rust-svcmgr-aarch64:
	$(MAKE) -j2 ARCH=aarch64 NOMMU=$(NOMMU) NATIVE_RUST_SVCMGR=1 native-rust-svcmgr
native-rust-svcmgr-x86_64:
	$(MAKE) -j2 ARCH=x86_64 NOMMU=$(NOMMU) NATIVE_RUST_SVCMGR=1 native-rust-svcmgr

ifeq ($(NATIVE_RUST_PEER),1)
# Build and, for the generated-header dependency, make the peer as strict as
# the C services: a change to user/svc/a20_services.idl rebuilds both.
native-rust-idl-peer-arch: $(RUST_USER_PEER_BIN)
else
native-rust-idl-peer-arch:
	@echo "native-rust-idl-peer-arch requires NATIVE_RUST_PEER=1 and an enabled Native architecture" >&2
	@exit 2
endif

.PHONY: native-rust-idl-peer native-rust-idl-peer-rv native-rust-idl-peer-rv32 native-rust-idl-peer-aarch64 native-rust-idl-peer-x86_64 native-rust-idl-peer-arch
native-rust-idl-peer:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-peer-arch
native-rust-idl-peer-rv: native-rust-idl-peer
native-rust-idl-peer-rv32:
	$(MAKE) -j2 ARCH=riscv32 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-peer-arch
native-rust-idl-peer-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-peer-arch
native-rust-idl-peer-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-peer-arch

.PHONY: native-rust-idl-adversary native-rust-idl-adversary-rv32 native-rust-idl-adversary-x86_64 native-rust-idl-adversary-arch
ifeq ($(NATIVE_RUST_PEER),1)
native-rust-idl-adversary-arch: $(RUST_USER_ACK_ADVERSARY_BIN) $(RUST_USER_ACK_TEST_BIN)
else
native-rust-idl-adversary-arch:
	@echo "native-rust-idl-adversary-arch requires NATIVE_RUST_PEER=1" >&2
	@exit 2
endif
native-rust-idl-adversary:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-adversary-arch
native-rust-idl-adversary-rv32:
	$(MAKE) -j2 ARCH=riscv32 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-adversary-arch
native-rust-idl-adversary-x86_64:
	$(MAKE) -j2 ARCH=x86_64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-adversary-arch

# Build the peer and the test-only malformed-ACK client/server in one
# recursive make, so the shared IDL output and flags stamp have one owner.
.PHONY: native-rust-idl-ack-test native-rust-idl-ack-test-rv32 native-rust-idl-ack-test-x86_64
native-rust-idl-ack-test:
	$(MAKE) -j2 ARCH=riscv64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 \
		native-rust-idl-peer-arch native-rust-idl-adversary-arch
native-rust-idl-ack-test-rv32:
	$(MAKE) -j2 ARCH=riscv32 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 \
		native-rust-idl-peer-arch native-rust-idl-adversary-arch
native-rust-idl-ack-test-x86_64:
	$(MAKE) -j2 ARCH=x86_64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 \
		native-rust-idl-peer-arch native-rust-idl-adversary-arch
.PHONY: native-rust-idl-ack-test-aarch64
native-rust-idl-ack-test-aarch64:
	$(MAKE) -j2 ARCH=aarch64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 \
		native-rust-idl-peer-arch native-rust-idl-adversary-arch

.PHONY: native-rust-idl-service-aarch64 native-rust-idl-service-rv32 native-rust-idl-service-x86_64
native-rust-idl-service-rv32:
	$(MAKE) -j2 ARCH=riscv32 NOMMU=$(NOMMU) NATIVE_RUST_IDL_SERVICE=1 native-rust-idl-service
native-rust-idl-service-aarch64:
	$(MAKE) -j2 ARCH=aarch64 NOMMU=$(NOMMU) NATIVE_RUST_IDL_SERVICE=1 native-rust-idl-service
native-rust-idl-service-x86_64:
	$(MAKE) -j2 ARCH=x86_64 NOMMU=$(NOMMU) NATIVE_RUST_IDL_SERVICE=1 native-rust-idl-service

# The Rust RTC owns the service protocol and request/alarm state machine;
# the C translation unit exposes only SDK and Goldfish register primitives.
ifeq ($(NATIVE_RUST_RTCD),1)
ifeq ($(filter $(ARCH),riscv64 aarch64),)
$(error NATIVE_RUST_RTCD=1 requires ARCH=riscv64 or aarch64 Goldfish RTC (got $(ARCH)))
endif
ifneq ($(strip $(filter $(USER_OPT),-O1 -O2 -O3 -Os)),$(USER_OPT))
$(error Rust rtcd requires USER_OPT in -O1 -O2 -O3 -Os)
endif
RUST_USER_RTCD_OPT_LEVEL := $(patsubst -O%,%,$(USER_OPT))
NATIVE_OUTPUTS += $(RUST_USER_RTCD_BIN)
native-programs: $(RUST_USER_RTCD_BIN)
$(RUST_USER_RTCD_OBJ): $(RUST_USER_RTCD_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) -C panic=abort -C opt-level=$(RUST_USER_RTCD_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@
$(RUST_USER_RTCD_BIN): $(RUST_USER_RTCD_OBJ) $(RUST_USER_CRT0_SRC) $(RUST_USER_RTCD_ABI_SRC) $(RUST_USER_RTCD_ABI_HDR) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) $(NATIVE_LD) \
		kernel/include/drivers/dual/goldfish_rtc.h kernel/include/drivers/dual/drv_env.h \
		kernel/include/drivers/driver_descriptor.h user/liba20rt/a20_sdk.h user/svc/a20_services.idl
	@mkdir -p $(dir $@)
	$(NATIVE_CC) -ffreestanding -nostdlib -static $(NATIVE_CFLAGS) -Iuser -Iuser/liba20rt -Ikernel/include \
		-T$(NATIVE_LD) $(NATIVE_CRT0) $(RUST_USER_CRT0_SRC) $(NATIVE_SDK_SRC) \
		$(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) $(RUST_USER_RTCD_ABI_SRC) \
		$(RUST_USER_RTCD_OBJ) $(NATIVE_LIBS) -o $@
.PHONY: native-rust-rtcd native-rust-rtcd-arch
native-rust-rtcd: native-rust-rtcd-arch
native-rust-rtcd-arch: $(RUST_USER_RTCD_BIN)
else
.PHONY: native-rust-rtcd native-rust-rtcd-arch
native-rust-rtcd native-rust-rtcd-arch:
	@echo "native-rust-rtcd requires NATIVE_RUST_RTCD=1 and ARCH=riscv64 or aarch64" >&2
	@exit 2
endif
.PHONY: native-rust-rtcd-aarch64
native-rust-rtcd-aarch64:
	$(MAKE) -j2 ARCH=aarch64 NOMMU=$(NOMMU) NATIVE_RUST_RTCD=1 native-rust-rtcd

# Rust owns ufsd startup, mount registration, supervision/EventQ lifecycle,
# wire validation and opcode dispatch. The existing C backend implementations
# remain the narrow filesystem algorithm boundary.
ifeq ($(NATIVE_RUST_UFSD),1)
ifneq ($(ARCH),riscv64)
$(error NATIVE_RUST_UFSD=1 is currently supported only for ARCH=riscv64)
endif
ifeq ($(filter $(NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE),0 1),)
$(error NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE must be 0 or 1)
endif
ifeq ($(NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE),1)
ifneq ($(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE),0)
$(error close-token lost-ACK and close-orphan failure profiles are mutually exclusive)
endif
endif
ifneq ($(strip $(filter $(USER_OPT),-O1 -O2 -O3 -Os)),$(USER_OPT))
$(error Rust ufsd requires USER_OPT in -O1 -O2 -O3 -Os)
endif
ifeq ($(filter $(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE),0 1),)
$(error NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE must be 0 or 1)
endif
ifeq ($(filter $(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL),0 1),)
$(error NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL must be 0 or 1)
endif
ifeq ($(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL),1)
ifneq ($(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE),1)
$(error restore-failure injection requires the close-orphan fail-once hook)
endif
ifeq ($(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER),16)
$(error restore-failure injection needs a write index below 16)
endif
endif
ifeq ($(filter $(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
$(error NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER must be in 1..16)
endif
ifeq ($(filter $(NATIVE_RUST_UFSD_EXT4_ORPHAN),0 1),)
$(error NATIVE_RUST_UFSD_EXT4_ORPHAN must be 0 or 1)
endif
ifeq ($(filter $(NATIVE_RUST_UFSD_EXT4_EXTENT_TXN),0 1),)
$(error NATIVE_RUST_UFSD_EXT4_EXTENT_TXN must be 0 or 1)
endif
RUST_USER_UFSD_OPT_LEVEL := $(patsubst -O%,%,$(USER_OPT))
RUST_USER_UFSD_BACKEND_SRCS := $(filter-out user/svc/ufsd.c user/svc/fscompat/bcache_user.c,$(UFSD_CORE_SRCS)) $(UFSD_VNFS_SRCS) $(RUST_USER_UFSD_CACHE_ABI_SRC)
NATIVE_OUTPUTS += $(RUST_USER_UFSD_BIN)
native-programs: $(RUST_USER_UFSD_BIN)
$(RUST_USER_UFSD_OBJ): $(RUST_USER_UFSD_SRC) user/svc/ufs_lifetime.rs $(RUST_USER_UFSD_SUPERVISION_SRC) $(RUST_USER_UFSD_PROTOCOL_SRC) $(RUST_USER_UFSD_CACHE_SRC) $(RUST_USER_UFSD_JBD2_POLICY_SRC) $(RUST_USER_UFSD_JBD2_TXN_SRC) $(RUST_USER_UFSD_JBD2_RECORD_SRC) $(RUST_USER_UFSD_EXT4_ORPHAN_SRC) $(RUST_USER_UFSD_EXTENT_TXN_SRC) $(RUST_USER_UFSD_MOUNT_STATE_SRC) $(RUST_USER_UFSD_SYNC_PLAN_SRC) $(RUST_USER_IDL_RUST) kernel/fs/diskfs/ext4_inode_free_txn.h tools/rust-user.mk
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) -C panic=abort -C opt-level=$(RUST_USER_UFSD_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USER_CODE_MODEL) \
		$(if $(filter test-close-orphan-%,$(RUST_USER_UFSD_TEST_PROFILE)),--cfg ufsd_test_close_orphan,) \
		$(if $(filter test-drop-close-token-ack,$(RUST_USER_UFSD_TEST_PROFILE)),--cfg ufsd_test_close_token_drop_ack_once,) \
		$< -o $@

$(RUST_USER_UFSD_BIN): $(RUST_USER_UFSD_OBJ) $(RUST_USER_UFSD_SRC) $(RUST_USER_UFSD_SUPERVISION_SRC) $(RUST_USER_UFSD_PROTOCOL_SRC) $(RUST_USER_UFSD_CACHE_SRC) $(RUST_USER_UFSD_JBD2_POLICY_SRC) $(RUST_USER_UFSD_JBD2_TXN_SRC) $(RUST_USER_UFSD_EXT4_ORPHAN_SRC) $(RUST_USER_UFSD_EXTENT_TXN_SRC) $(RUST_USER_UFSD_MOUNT_STATE_SRC) $(RUST_USER_UFSD_SYNC_PLAN_SRC) kernel/fs/diskfs/ext4_inode_free_txn.h user/svc/ufs_close_token.h tools/rust-user.mk \
		$(RUST_USER_CRT0_SRC) $(RUST_USER_UFSD_ABI_SRC) $(RUST_USER_UFSD_ABI_HDR) \
		$(RUST_USER_UFSD_BACKEND_SRCS) $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) \
		$(NATIVE_ARCH_SRC) $(NATIVE_LD) $(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) user/svc/ufs_backends.h \
		kernel/include/fs/ufs_proto.h kernel/include/fs/fat32lite.h
	@mkdir -p $(dir $@)
	$(NATIVE_CC) -ffreestanding -nostdlib -static $(NATIVE_CFLAGS) -DCONFIG_UFSD_RUST_JBD2_POLICY -DCONFIG_UFSD_RUST_JBD2_TXN $(if $(filter 1,$(NATIVE_RUST_UFSD_EXT4_ORPHAN)),-DCONFIG_UFSD_RUST_EXT4_ORPHAN,) $(if $(filter 1,$(NATIVE_RUST_UFSD_EXT4_EXTENT_TXN)),-DCONFIG_EXT4_RUST_EXTENT_TXN,) $(if $(filter 1,$(NATIVE_RUST_UFSD_JBD2_RECORD)),-DCONFIG_UFSD_RUST_JBD2_RECORD,) $(if $(filter 1,$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE)),-DCONFIG_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE -DCONFIG_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER=$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER),) $(if $(filter 1,$(NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL)),-DCONFIG_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL,) -Iuser -Iuser/liba20rt -Iuser/svc/fscompat -Ikernel/include \
		-T$(NATIVE_LD) $(NATIVE_CRT0) $(RUST_USER_CRT0_SRC) $(NATIVE_SDK_SRC) \
		$(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) $(RUST_USER_UFSD_ABI_SRC) \
		$(RUST_USER_UFSD_BACKEND_SRCS) $(RUST_USER_UFSD_OBJ) \
		$(RUST_USER_LIBCORE) $(RUST_USER_LIBCOMPILER_BUILTINS) $(NATIVE_LIBS) -o $@
.PHONY: native-rust-ufsd native-rust-ufsd-arch
native-rust-ufsd native-rust-ufsd-arch: $(RUST_USER_UFSD_BIN)
else
.PHONY: native-rust-ufsd native-rust-ufsd-arch
native-rust-ufsd native-rust-ufsd-arch:
	@echo "native-rust-ufsd requires NATIVE_RUST_UFSD=1 and ARCH=riscv64" >&2
	@exit 2
endif

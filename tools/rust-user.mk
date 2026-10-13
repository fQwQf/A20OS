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
# Scope: ARCH=riscv64 only, and only because riscv64gc-unknown-none-elf is the
# one installed target whose ABI matches the Native user ABI that
# tools/targets-native.mk builds with (`-march=rv64gc -mabi=lp64d
# -mcmodel=medany`, so code-model=medium).  Every other architecture keeps the
# C reference until a matching freestanding target is installed — the same
# policy tools/rust-kernel.mk applies to RV32.  Extending this file is a matter
# of adding one RUST_USER_TARGET_<arch> line plus its code-model line; nothing
# in the Rust source changes, because the source speaks the ABI, not a host.
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

RUST_USER_PEER_SRC := user/svc/rust_idl_peer.rs
RUST_USER_CRT0_SRC := user/svc/rust_peer_crt0.c
RUST_USER_OBJ_DIR  := $(USER_BUILD_DIR)/rust
RUST_USER_PEER_OBJ := $(RUST_USER_OBJ_DIR)/rust_idl_peer.o
RUST_USER_PEER_BIN := $(USER_BUILD_DIR)/rust-idl-peer-$(NATIVE_TAG)
RUST_USER_IDL_RUST := $(RUST_USER_OBJ_DIR)/a20_services_idl.rs

RUST_USER_TARGET_riscv64 := riscv64gc-unknown-none-elf
RUST_USER_TARGET := $(RUST_USER_TARGET_$(ARCH))

# Match the tree's medany user code model for riscv64.
RUST_USER_CODE_MODEL_riscv64 := medium
RUST_USER_CODE_MODEL := $(RUST_USER_CODE_MODEL_$(ARCH))

# This experimental binary is opt-in. Including this fragment must not alter
# C-only builds, `clean`, or native outputs for other architectures.
ifneq ($(filter native-rust-idl-peer-arch,$(MAKECMDGOALS)),)
ifneq ($(NATIVE_RUST_PEER),1)
$(error native-rust-idl-peer-arch requires NATIVE_RUST_PEER=1; use native-rust-idl-peer)
endif
ifneq ($(ARCH),riscv64)
$(error Rust Native IDL peer is currently supported only for ARCH=riscv64 (got $(ARCH)))
endif
endif

ifeq ($(NATIVE_RUST_PEER),1)
ifneq ($(ARCH),riscv64)
$(error NATIVE_RUST_PEER=1 is currently supported only for ARCH=riscv64 (got $(ARCH)))
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

NATIVE_OUTPUTS += $(RUST_USER_PEER_BIN)
native-programs: $(RUST_USER_PEER_BIN)

RUST_USER_PEER_FLAGS := $(RUSTC) --edition=2021 --target $(RUST_USER_TARGET) \
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

$(RUST_USER_IDL_RUST): user/svc/a20_services.idl user/liba20rt/a20_types.h tools/a20idl.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/a20idl.py user/svc/a20_services.idl /dev/null --rust-output $@

$(RUST_USER_PEER_OBJ): $(RUST_USER_PEER_SRC) $(RUST_USER_IDL_RUST) tools/rust-user.mk $(RUST_USER_PEER_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	A20_IDL_RUST_BINDINGS='$(abspath $(RUST_USER_IDL_RUST))' $(RUSTC) --edition=2021 --crate-type bin --emit=obj \
		--target $(RUST_USER_TARGET) \
		-C panic=abort -C opt-level=$(RUST_USER_OPT_LEVEL) \
		-C codegen-units=1 -C relocation-model=static \
		-C code-model=$(RUST_USER_CODE_MODEL) \
		$< -o $@

# Same single-invocation whole-program link the Native svc programs use
# (tools/targets-native.mk:NATIVE_SVC_RECIPE), with the Rust object added and
# the C programs replaced by the crt0 shim.
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
	$(RUST_USER_PEER_OBJ) \
	$(NATIVE_LIBS) \
	-o $(1)
endef

$(RUST_USER_PEER_BIN): $(RUST_USER_PEER_OBJ) $(RUST_USER_CRT0_SRC) \
		$(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		$(NATIVE_LD) user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		$(A20_SERVICES_IDL_HDR) $(NATIVE_SDK_HDRS) tools/rust-user.mk
	$(call RUST_USER_PEER_RECIPE,$@,$(RUST_USER_PEER_OBJ),$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$(RUST_USER_CRT0_SRC))

# Build and, for the generated-header dependency, make the peer as strict as
# the C services: a change to user/svc/a20_services.idl rebuilds both.
native-rust-idl-peer-arch: $(RUST_USER_PEER_BIN)
else
native-rust-idl-peer-arch:
	@echo "native-rust-idl-peer-arch requires NATIVE_RUST_PEER=1 and ARCH=riscv64" >&2
	@exit 2
endif

.PHONY: native-rust-idl-peer native-rust-idl-peer-rv native-rust-idl-peer-arch
native-rust-idl-peer:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) NATIVE_RUST_PEER=1 native-rust-idl-peer-arch
native-rust-idl-peer-rv: native-rust-idl-peer

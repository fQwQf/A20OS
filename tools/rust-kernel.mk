# Small, dependency-free Rust object integration for the kernel. Keep target
# triples explicit here: they are part of the C/Rust ABI contract and must be
# reviewed alongside each architecture's C flags.
RUSTC ?= rustc
RUST_KERNEL_SRC := kernel/net/netfilter_rewrite.rs
RUST_NETFILTER_TARGET_x86_64 := x86_64-unknown-none
RUST_NETFILTER_TARGET_aarch64 := aarch64-unknown-none
RUST_NETFILTER_TARGET_loongarch64 := loongarch64-unknown-none
RUST_NETFILTER_TARGET_riscv64 := riscv64imac-unknown-none-elf
# rustc's installed RV32 target only has the ILP32F ABI; A20OS C uses ILP32D.
# Keep RV32 on the C reference until a matching target/core is available.
RUST_NETFILTER_TARGET_arm32 := armv7a-none-eabihf
RUST_NETFILTER_TARGET_armv7m := thumbv7m-none-eabi

RUST_NETFILTER_TARGET := $(RUST_NETFILTER_TARGET_$(ARCH))
RUST_NETFILTER_OBJECT := $(BUILD_DIR)/rust/netfilter_rewrite.o
RUST_TASK_OBJECT_TARGET_x86_64 := x86_64-unknown-none
RUST_TASK_OBJECT_TARGET_aarch64 := aarch64-unknown-none
RUST_TASK_OBJECT_TARGET_loongarch64 := loongarch64-unknown-none
RUST_TASK_OBJECT_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_TASK_OBJECT_TARGET_arm32 := armv7a-none-eabihf
RUST_TASK_OBJECT_TARGET := $(RUST_TASK_OBJECT_TARGET_$(ARCH))
RUST_TASK_OBJECT := $(BUILD_DIR)/rust/task_object.o
RUST_CHANNEL_RECV_TXN_SRC := kernel/abi/native/channel_recv_txn.rs
RUST_CHANNEL_RECV_TARGET_x86_64 := x86_64-unknown-none
RUST_CHANNEL_RECV_TARGET_aarch64 := aarch64-unknown-none
RUST_CHANNEL_RECV_TARGET_loongarch64 := loongarch64-unknown-none
RUST_CHANNEL_RECV_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_CHANNEL_RECV_TARGET_arm32 := armv7a-none-eabihf
RUST_CHANNEL_RECV_TARGET := $(RUST_CHANNEL_RECV_TARGET_$(ARCH))
RUST_CHANNEL_RECV_TXN_OBJECT := $(BUILD_DIR)/rust/channel_recv_txn.o
RUST_DONTUNMAP_TXN_SRC := kernel/mm/mremap_dontunmap_txn.rs
RUST_DONTUNMAP_TARGET_x86_64 := x86_64-unknown-none
RUST_DONTUNMAP_TARGET_aarch64 := aarch64-unknown-none
RUST_DONTUNMAP_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_DONTUNMAP_TARGET := $(RUST_DONTUNMAP_TARGET_$(ARCH))
RUST_DONTUNMAP_TXN_OBJECT := $(BUILD_DIR)/rust/mremap_dontunmap_txn.o
RUST_HANDLE_SLOT_SRC := kernel/abi/native/handle_slot.rs
RUST_HANDLE_SLOT_TARGET_x86_64 := x86_64-unknown-none
RUST_HANDLE_SLOT_TARGET_aarch64 := aarch64-unknown-none
RUST_HANDLE_SLOT_TARGET_loongarch64 := loongarch64-unknown-none
RUST_HANDLE_SLOT_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_HANDLE_SLOT_TARGET_arm32 := armv7a-none-eabihf
RUST_HANDLE_SLOT_TARGET := $(RUST_HANDLE_SLOT_TARGET_$(ARCH))
RUST_HANDLE_SLOT_OBJECT := $(BUILD_DIR)/rust/handle_slot.o

RUST_OPT_LEVEL := $(if $(filter -Os,$(OPT)),s,$(patsubst -O%,%,$(OPT)))
ifeq ($(filter $(OPT),-O1 -O2 -O3 -Os),)
ifneq ($(filter rust,$(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(DONTUNMAP_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL)),)
$(error Rust kernel slices support OPT=-O1/-O2/-O3/-Os; -O0 can leave core panic/overflow symbols unresolved)
endif
endif
RUST_DEBUG_INFO := $(if $(findstring -g,$(OPT)),2,0)

ifeq ($(HANDLE_SLOT_TRANSITIONS_IMPL),rust)
ifeq ($(strip $(RUST_HANDLE_SLOT_TARGET)),)
$(error HANDLE_SLOT_TRANSITIONS_IMPL=rust has no Rust target for ARCH=$(ARCH); keep the C reference selected)
endif
RUST_HANDLE_SLOT_CODE_MODEL_x86_64 := large
RUST_HANDLE_SLOT_CODE_MODEL_aarch64 := large
RUST_HANDLE_SLOT_CODE_MODEL_loongarch64 := medium
RUST_HANDLE_SLOT_CODE_MODEL_riscv64 := medium
RUST_HANDLE_SLOT_CODE_MODEL_arm32 := small
RUST_HANDLE_SLOT_FEATURES_aarch64 := -neon,-fp-armv8
RUST_HANDLE_SLOT_FEATURES_arm32 := -neon
RUST_HANDLE_SLOT_EXTRA_x86_64 := -C no-redzone=yes
RUST_HANDLE_SLOT_CODE_MODEL := $(RUST_HANDLE_SLOT_CODE_MODEL_$(ARCH))
RUST_HANDLE_SLOT_FEATURES := $(RUST_HANDLE_SLOT_FEATURES_$(ARCH))
RUST_HANDLE_SLOT_EXTRA := $(RUST_HANDLE_SLOT_EXTRA_$(ARCH))
RUST_HANDLE_SLOT_FLAGS_STAMP := $(BUILD_DIR)/rust/.handle-slot-flags
.PHONY: FORCE_RUST_HANDLE_SLOT_FLAGS
FORCE_RUST_HANDLE_SLOT_FLAGS:

$(RUST_HANDLE_SLOT_FLAGS_STAMP): FORCE_RUST_HANDLE_SLOT_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_HANDLE_SLOT_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_HANDLE_SLOT_CODE_MODEL) $(RUST_HANDLE_SLOT_EXTRA) -C target-feature=$(RUST_HANDLE_SLOT_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_HANDLE_SLOT_OBJECT): $(RUST_HANDLE_SLOT_SRC) tools/rust-kernel.mk $(RUST_HANDLE_SLOT_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_HANDLE_SLOT_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_HANDLE_SLOT_CODE_MODEL),-C code-model=$(RUST_HANDLE_SLOT_CODE_MODEL)) \
		$(RUST_HANDLE_SLOT_EXTRA) \
		$(if $(RUST_HANDLE_SLOT_FEATURES),-C target-feature=$(RUST_HANDLE_SLOT_FEATURES)) \
		$< -o $@
endif

ifeq ($(DONTUNMAP_TXN_IMPL),rust)
ifeq ($(strip $(RUST_DONTUNMAP_TARGET)),)
$(error DONTUNMAP_TXN_IMPL=rust has no matching Rust target for ARCH=$(ARCH); keep the C reference selected)
endif
RUST_DONTUNMAP_CODE_MODEL_x86_64 := large
RUST_DONTUNMAP_CODE_MODEL_aarch64 := large
RUST_DONTUNMAP_CODE_MODEL_riscv64 := medium
RUST_DONTUNMAP_FEATURES_aarch64 := -neon,-fp-armv8
RUST_DONTUNMAP_EXTRA_x86_64 := -C no-redzone=yes
RUST_DONTUNMAP_CODE_MODEL := $(RUST_DONTUNMAP_CODE_MODEL_$(ARCH))
RUST_DONTUNMAP_FEATURES := $(RUST_DONTUNMAP_FEATURES_$(ARCH))
RUST_DONTUNMAP_EXTRA := $(RUST_DONTUNMAP_EXTRA_$(ARCH))
RUST_DONTUNMAP_FLAGS_STAMP := $(BUILD_DIR)/rust/.mremap-dontunmap-flags
.PHONY: FORCE_RUST_DONTUNMAP_FLAGS
FORCE_RUST_DONTUNMAP_FLAGS:

$(RUST_DONTUNMAP_FLAGS_STAMP): FORCE_RUST_DONTUNMAP_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_DONTUNMAP_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_DONTUNMAP_CODE_MODEL) $(RUST_DONTUNMAP_EXTRA) -C target-feature=$(RUST_DONTUNMAP_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_DONTUNMAP_TXN_OBJECT): $(RUST_DONTUNMAP_TXN_SRC) tools/rust-kernel.mk $(RUST_DONTUNMAP_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_DONTUNMAP_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_DONTUNMAP_CODE_MODEL),-C code-model=$(RUST_DONTUNMAP_CODE_MODEL)) \
		$(RUST_DONTUNMAP_EXTRA) \
		$(if $(RUST_DONTUNMAP_FEATURES),-C target-feature=$(RUST_DONTUNMAP_FEATURES)) \
		$< -o $@
endif

ifeq ($(NETFILTER_REWRITE_IMPL),rust)
ifeq ($(strip $(RUST_NETFILTER_TARGET)),)
$(error NETFILTER_REWRITE_IMPL=rust has no target configured for ARCH=$(ARCH); use the C reference)
endif

RUST_NETFILTER_TARGET_FEATURES_x86_64 :=
RUST_NETFILTER_EXTRA_FLAGS_x86_64 := -C no-redzone=yes
RUST_NETFILTER_CODE_MODEL_x86_64 := large
# The kernel links in the high half; match the existing C code-model choices.
RUST_NETFILTER_CODE_MODEL_aarch64 := large
RUST_NETFILTER_CODE_MODEL_riscv64 := medium
RUST_NETFILTER_CODE_MODEL_loongarch64 := medium
RUST_NETFILTER_CODE_MODEL_arm32 := small
RUST_NETFILTER_CODE_MODEL_armv7m := small
# Match the kernel's general-register-only rule; this object carries no floating
# point values across its C ABI and must not start using SIMD in the packet path.
RUST_NETFILTER_TARGET_FEATURES_aarch64 := -neon,-fp-armv8
RUST_NETFILTER_TARGET_FEATURES_arm32 := -neon
RUST_NETFILTER_TARGET_FEATURES := $(RUST_NETFILTER_TARGET_FEATURES_$(ARCH))
RUST_NETFILTER_EXTRA_FLAGS := $(RUST_NETFILTER_EXTRA_FLAGS_$(ARCH))
RUST_NETFILTER_CODE_MODEL := $(RUST_NETFILTER_CODE_MODEL_$(ARCH))

RUST_NETFILTER_FLAGS_STAMP := $(BUILD_DIR)/rust/.netfilter-rewrite-flags
.PHONY: FORCE_RUST_NETFILTER_FLAGS
FORCE_RUST_NETFILTER_FLAGS:

$(RUST_NETFILTER_FLAGS_STAMP): FORCE_RUST_NETFILTER_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version 2>/dev/null) --target $(RUST_NETFILTER_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_NETFILTER_CODE_MODEL) $(RUST_NETFILTER_EXTRA_FLAGS) -C target-feature=$(RUST_NETFILTER_TARGET_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_NETFILTER_OBJECT): $(RUST_KERNEL_SRC) tools/rust-kernel.mk $(RUST_NETFILTER_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_NETFILTER_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_NETFILTER_CODE_MODEL),-C code-model=$(RUST_NETFILTER_CODE_MODEL)) \
		$(RUST_NETFILTER_EXTRA_FLAGS) \
		$(if $(RUST_NETFILTER_TARGET_FEATURES),-C target-feature=$(RUST_NETFILTER_TARGET_FEATURES)) \
		$< -o $@
endif

ifeq ($(CHANNEL_RECV_TXN_IMPL),rust)
ifeq ($(strip $(RUST_CHANNEL_RECV_TARGET)),)
$(error CHANNEL_RECV_TXN_IMPL=rust has no target configured for ARCH=$(ARCH); keep the C reference selected)
endif
RUST_CHANNEL_RECV_CODE_MODEL_x86_64 := large
RUST_CHANNEL_RECV_CODE_MODEL_aarch64 := large
RUST_CHANNEL_RECV_CODE_MODEL_loongarch64 := medium
RUST_CHANNEL_RECV_CODE_MODEL_riscv64 := medium
RUST_CHANNEL_RECV_CODE_MODEL_arm32 := small
RUST_CHANNEL_RECV_FEATURES_aarch64 := -neon,-fp-armv8
RUST_CHANNEL_RECV_FEATURES_arm32 := -neon
RUST_CHANNEL_RECV_EXTRA_x86_64 := -C no-redzone=yes
RUST_CHANNEL_RECV_CODE_MODEL := $(RUST_CHANNEL_RECV_CODE_MODEL_$(ARCH))
RUST_CHANNEL_RECV_FEATURES := $(RUST_CHANNEL_RECV_FEATURES_$(ARCH))
RUST_CHANNEL_RECV_EXTRA := $(RUST_CHANNEL_RECV_EXTRA_$(ARCH))
RUST_CHANNEL_RECV_FLAGS_STAMP := $(BUILD_DIR)/rust/.channel-recv-txn-flags
.PHONY: FORCE_RUST_CHANNEL_RECV_FLAGS
FORCE_RUST_CHANNEL_RECV_FLAGS:

$(RUST_CHANNEL_RECV_FLAGS_STAMP): FORCE_RUST_CHANNEL_RECV_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_CHANNEL_RECV_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_CHANNEL_RECV_CODE_MODEL) $(RUST_CHANNEL_RECV_EXTRA) -C target-feature=$(RUST_CHANNEL_RECV_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_CHANNEL_RECV_TXN_OBJECT): $(RUST_CHANNEL_RECV_TXN_SRC) tools/rust-kernel.mk $(RUST_CHANNEL_RECV_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_CHANNEL_RECV_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_CHANNEL_RECV_CODE_MODEL),-C code-model=$(RUST_CHANNEL_RECV_CODE_MODEL)) \
		$(RUST_CHANNEL_RECV_EXTRA) \
		$(if $(RUST_CHANNEL_RECV_FEATURES),-C target-feature=$(RUST_CHANNEL_RECV_FEATURES)) \
		$< -o $@
endif

ifeq ($(TASK_OBJECTS_IMPL),rust)
ifeq ($(strip $(RUST_TASK_OBJECT_TARGET)),)
$(error TASK_OBJECTS_IMPL=rust has no matching Rust target for ARCH=$(ARCH); keep C selected on this architecture)
endif
RUST_TASK_OBJECT_CODE_MODEL_x86_64 := large
RUST_TASK_OBJECT_CODE_MODEL_aarch64 := large
RUST_TASK_OBJECT_CODE_MODEL_loongarch64 := medium
RUST_TASK_OBJECT_CODE_MODEL_riscv64 := medium
RUST_TASK_OBJECT_CODE_MODEL_arm32 := small
RUST_TASK_OBJECT_FEATURES_aarch64 := -neon,-fp-armv8
RUST_TASK_OBJECT_FEATURES_arm32 := -neon
RUST_TASK_OBJECT_EXTRA_x86_64 := -C no-redzone=yes
RUST_TASK_OBJECT_CODE_MODEL := $(RUST_TASK_OBJECT_CODE_MODEL_$(ARCH))
RUST_TASK_OBJECT_FEATURES := $(RUST_TASK_OBJECT_FEATURES_$(ARCH))
RUST_TASK_OBJECT_EXTRA := $(RUST_TASK_OBJECT_EXTRA_$(ARCH))
RUST_TASK_OBJECT_FLAGS_STAMP := $(BUILD_DIR)/rust/.task-object-flags
.PHONY: FORCE_RUST_TASK_OBJECT_FLAGS
FORCE_RUST_TASK_OBJECT_FLAGS:

$(RUST_TASK_OBJECT_FLAGS_STAMP): FORCE_RUST_TASK_OBJECT_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version 2>/dev/null) --target $(RUST_TASK_OBJECT_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_TASK_OBJECT_CODE_MODEL) $(RUST_TASK_OBJECT_EXTRA) -C target-feature=$(RUST_TASK_OBJECT_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_TASK_OBJECT): kernel/proc/task_object.rs tools/rust-kernel.mk $(RUST_TASK_OBJECT_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_TASK_OBJECT_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_TASK_OBJECT_CODE_MODEL),-C code-model=$(RUST_TASK_OBJECT_CODE_MODEL)) \
		$(RUST_TASK_OBJECT_EXTRA) \
		$(if $(RUST_TASK_OBJECT_FEATURES),-C target-feature=$(RUST_TASK_OBJECT_FEATURES)) \
		$< -o $@
endif

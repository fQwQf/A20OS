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
RUST_HANDLE_ENTRY_SRC := kernel/abi/native/handle_entry.rs
RUST_HANDLE_TABLE_STATE_SRC := kernel/abi/native/handle_table_state.rs
RUST_HANDLE_TABLE_STATE_HDR := kernel/abi/native/handle_table_state.h
RUST_HANDLE_SLOT_TARGET_x86_64 := x86_64-unknown-none
RUST_HANDLE_SLOT_TARGET_aarch64 := aarch64-unknown-none
RUST_HANDLE_SLOT_TARGET_loongarch64 := loongarch64-unknown-none
RUST_HANDLE_SLOT_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_HANDLE_SLOT_TARGET_arm32 := armv7a-none-eabihf
RUST_HANDLE_SLOT_TARGET := $(RUST_HANDLE_SLOT_TARGET_$(ARCH))
RUST_HANDLE_SLOT_OBJECT := $(BUILD_DIR)/rust/handle_slot.o
RUST_HANDLE_TABLE_STATE_OBJECT := $(BUILD_DIR)/rust/handle_table_state.o

RUST_OPT_LEVEL := $(if $(filter -Os,$(OPT)),s,$(patsubst -O%,%,$(OPT)))
ifeq ($(filter $(OPT),-O1 -O2 -O3 -Os),)
ifneq ($(filter rust,$(NETFILTER_REWRITE_IMPL) $(TASK_OBJECTS_IMPL) $(CHANNEL_RECV_TXN_IMPL) $(DONTUNMAP_TXN_IMPL) $(HANDLE_SLOT_TRANSITIONS_IMPL) $(USER_COPY_IMPL) $(PROCESS_VM_IMPL) $(PARK_WAIT_IMPL) $(WAIT_TIMER_IMPL) $(WAIT_QUEUE_IMPL) $(RW_MUTEX_IMPL) $(MUTEX_STATE_IMPL) $(COMPLETION_IMPL) $(VMA_IMPL) $(VMO_PAGE_IMPL) $(VMO_LIFECYCLE_IMPL) $(VMO_PAGER_IMPL)),)
$(error Rust kernel slices support OPT=-O1/-O2/-O3/-Os; -O0 can leave core panic/overflow symbols unresolved)
endif

endif
RUST_DEBUG_INFO := $(if $(findstring -g,$(OPT)),2,0)

# MUTEX_STATE owns locked/depth/owner transitions; C retains the IRQ-save lock,
# task parking and wake queue operations around them.
RUST_MUTEX_STATE_SRC := kernel/core/mutex_state.rs
RUST_MUTEX_STATE_TARGET_x86_64 := x86_64-unknown-none
RUST_MUTEX_STATE_TARGET_aarch64 := aarch64-unknown-none
RUST_MUTEX_STATE_TARGET_loongarch64 := loongarch64-unknown-none
RUST_MUTEX_STATE_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_MUTEX_STATE_TARGET_arm32 := armv7a-none-eabihf
RUST_MUTEX_STATE_TARGET := $(RUST_MUTEX_STATE_TARGET_$(ARCH))
RUST_MUTEX_STATE_OBJECT := $(BUILD_DIR)/rust/mutex_state.o

ifeq ($(MUTEX_STATE_IMPL),rust)
ifeq ($(strip $(RUST_MUTEX_STATE_TARGET)),)
$(error MUTEX_STATE_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_MUTEX_STATE_CODE_MODEL_x86_64 := large
RUST_MUTEX_STATE_CODE_MODEL_aarch64 := large
RUST_MUTEX_STATE_CODE_MODEL_loongarch64 := medium
RUST_MUTEX_STATE_CODE_MODEL_riscv64 := medium
RUST_MUTEX_STATE_CODE_MODEL_arm32 := small
RUST_MUTEX_STATE_FEATURES_aarch64 := -neon,-fp-armv8
RUST_MUTEX_STATE_FEATURES_arm32 := -neon
RUST_MUTEX_STATE_EXTRA_x86_64 := -C no-redzone=yes
RUST_MUTEX_STATE_CODE_MODEL := $(RUST_MUTEX_STATE_CODE_MODEL_$(ARCH))
RUST_MUTEX_STATE_FEATURES := $(RUST_MUTEX_STATE_FEATURES_$(ARCH))
RUST_MUTEX_STATE_EXTRA := $(RUST_MUTEX_STATE_EXTRA_$(ARCH))
RUST_MUTEX_STATE_FLAGS_STAMP := $(BUILD_DIR)/rust/.mutex-state-flags
.PHONY: FORCE_RUST_MUTEX_STATE_FLAGS
FORCE_RUST_MUTEX_STATE_FLAGS:

$(RUST_MUTEX_STATE_FLAGS_STAMP): FORCE_RUST_MUTEX_STATE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_MUTEX_STATE_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_MUTEX_STATE_CODE_MODEL) $(RUST_MUTEX_STATE_EXTRA) -C target-feature=$(RUST_MUTEX_STATE_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_MUTEX_STATE_OBJECT): $(RUST_MUTEX_STATE_SRC) tools/rust-kernel.mk $(RUST_MUTEX_STATE_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_MUTEX_STATE_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_MUTEX_STATE_CODE_MODEL),-C code-model=$(RUST_MUTEX_STATE_CODE_MODEL)) \
		$(RUST_MUTEX_STATE_EXTRA) \
		$(if $(RUST_MUTEX_STATE_FEATURES),-C target-feature=$(RUST_MUTEX_STATE_FEATURES)) \
		$< -o $@
endif

# VMO page materialization is a Rust-owned transaction over narrow C
# primitives for the VMO lock, pager, frame allocator and cgroup charge.
RUST_VMO_PAGE_SRC := kernel/mm/vmo_page.rs
RUST_VMO_PAGE_TARGET_x86_64 := x86_64-unknown-none
RUST_VMO_PAGE_TARGET_aarch64 := aarch64-unknown-none
RUST_VMO_PAGE_TARGET_loongarch64 := loongarch64-unknown-none
RUST_VMO_PAGE_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_VMO_PAGE_TARGET_arm32 := armv7a-none-eabihf
RUST_VMO_PAGE_TARGET := $(RUST_VMO_PAGE_TARGET_$(ARCH))
RUST_VMO_PAGE_OBJECT := $(BUILD_DIR)/rust/vmo_page.o

ifeq ($(VMO_PAGE_IMPL),rust)
ifeq ($(strip $(RUST_VMO_PAGE_TARGET)),)
$(error VMO_PAGE_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_VMO_PAGE_CODE_MODEL_x86_64 := large
RUST_VMO_PAGE_CODE_MODEL_aarch64 := large
RUST_VMO_PAGE_CODE_MODEL_loongarch64 := medium
RUST_VMO_PAGE_CODE_MODEL_riscv64 := medium
RUST_VMO_PAGE_CODE_MODEL_arm32 := small
RUST_VMO_PAGE_FEATURES_aarch64 := -neon,-fp-armv8
RUST_VMO_PAGE_FEATURES_arm32 := -neon
RUST_VMO_PAGE_EXTRA_x86_64 := -C no-redzone=yes
RUST_VMO_PAGE_CODE_MODEL := $(RUST_VMO_PAGE_CODE_MODEL_$(ARCH))
RUST_VMO_PAGE_FEATURES := $(RUST_VMO_PAGE_FEATURES_$(ARCH))
RUST_VMO_PAGE_EXTRA := $(RUST_VMO_PAGE_EXTRA_$(ARCH))
RUST_VMO_PAGE_FLAGS_STAMP := $(BUILD_DIR)/rust/.vmo-page-flags
.PHONY: FORCE_RUST_VMO_PAGE_FLAGS
FORCE_RUST_VMO_PAGE_FLAGS:

$(RUST_VMO_PAGE_FLAGS_STAMP): FORCE_RUST_VMO_PAGE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_VMO_PAGE_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_VMO_PAGE_CODE_MODEL) $(RUST_VMO_PAGE_EXTRA) -C target-feature=$(RUST_VMO_PAGE_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_VMO_PAGE_OBJECT): $(RUST_VMO_PAGE_SRC) tools/rust-kernel.mk $(RUST_VMO_PAGE_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_VMO_PAGE_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_VMO_PAGE_CODE_MODEL),-C code-model=$(RUST_VMO_PAGE_CODE_MODEL)) \
		$(RUST_VMO_PAGE_EXTRA) \
		$(if $(RUST_VMO_PAGE_FEATURES),-C target-feature=$(RUST_VMO_PAGE_FEATURES)) \
		$< -o $@
endif


# VMO reference decisions and non-materializing canonical-page lookup are
# Rust-owned; C supplies atomic refcount and lock-protected field primitives.
RUST_VMO_LIFECYCLE_SRC := kernel/mm/vmo_lifecycle.rs
RUST_VMO_LIFECYCLE_TARGET_x86_64 := x86_64-unknown-none
RUST_VMO_LIFECYCLE_TARGET_aarch64 := aarch64-unknown-none
RUST_VMO_LIFECYCLE_TARGET_loongarch64 := loongarch64-unknown-none
RUST_VMO_LIFECYCLE_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_VMO_LIFECYCLE_TARGET_arm32 := armv7a-none-eabihf
RUST_VMO_LIFECYCLE_TARGET := $(RUST_VMO_LIFECYCLE_TARGET_$(ARCH))
RUST_VMO_LIFECYCLE_OBJECT := $(BUILD_DIR)/rust/vmo_lifecycle.o

ifeq ($(VMO_LIFECYCLE_IMPL),rust)
ifeq ($(strip $(RUST_VMO_LIFECYCLE_TARGET)),)
$(error VMO_LIFECYCLE_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_VMO_LIFECYCLE_CODE_MODEL_x86_64 := large
RUST_VMO_LIFECYCLE_CODE_MODEL_aarch64 := large
RUST_VMO_LIFECYCLE_CODE_MODEL_loongarch64 := medium
RUST_VMO_LIFECYCLE_CODE_MODEL_riscv64 := medium
RUST_VMO_LIFECYCLE_CODE_MODEL_arm32 := small
RUST_VMO_LIFECYCLE_FEATURES_aarch64 := -neon,-fp-armv8
RUST_VMO_LIFECYCLE_FEATURES_arm32 := -neon
RUST_VMO_LIFECYCLE_EXTRA_x86_64 := -C no-redzone=yes
RUST_VMO_LIFECYCLE_CODE_MODEL := $(RUST_VMO_LIFECYCLE_CODE_MODEL_$(ARCH))
RUST_VMO_LIFECYCLE_FEATURES := $(RUST_VMO_LIFECYCLE_FEATURES_$(ARCH))
RUST_VMO_LIFECYCLE_EXTRA := $(RUST_VMO_LIFECYCLE_EXTRA_$(ARCH))
RUST_VMO_LIFECYCLE_FLAGS_STAMP := $(BUILD_DIR)/rust/.vmo-lifecycle-flags
.PHONY: FORCE_RUST_VMO_LIFECYCLE_FLAGS
FORCE_RUST_VMO_LIFECYCLE_FLAGS:

$(RUST_VMO_LIFECYCLE_FLAGS_STAMP): FORCE_RUST_VMO_LIFECYCLE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_VMO_LIFECYCLE_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_VMO_LIFECYCLE_CODE_MODEL) $(RUST_VMO_LIFECYCLE_EXTRA) -C target-feature=$(RUST_VMO_LIFECYCLE_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_VMO_LIFECYCLE_OBJECT): $(RUST_VMO_LIFECYCLE_SRC) tools/rust-kernel.mk $(RUST_VMO_LIFECYCLE_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_VMO_LIFECYCLE_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_VMO_LIFECYCLE_CODE_MODEL),-C code-model=$(RUST_VMO_LIFECYCLE_CODE_MODEL)) \
		$(RUST_VMO_LIFECYCLE_EXTRA) \
		$(if $(RUST_VMO_LIFECYCLE_FEATURES),-C target-feature=$(RUST_VMO_LIFECYCLE_FEATURES)) \
		$< -o $@
endif


# Pager VMO association, weak registration, detach, and peer-close cancellation
# are Rust-owned; C retains channel/refcount/lock/wait-queue primitives.
RUST_VMO_PAGER_SRC := kernel/ipc/pager_vmo.rs
RUST_VMO_PAGER_TARGET_x86_64 := x86_64-unknown-none
RUST_VMO_PAGER_TARGET_aarch64 := aarch64-unknown-none
RUST_VMO_PAGER_TARGET_loongarch64 := loongarch64-unknown-none
RUST_VMO_PAGER_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_VMO_PAGER_TARGET_arm32 := armv7a-none-eabihf
RUST_VMO_PAGER_TARGET := $(RUST_VMO_PAGER_TARGET_$(ARCH))
RUST_VMO_PAGER_OBJECT := $(BUILD_DIR)/rust/pager_vmo.o

ifeq ($(VMO_PAGER_IMPL),rust)
ifeq ($(strip $(RUST_VMO_PAGER_TARGET)),)
$(error VMO_PAGER_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_VMO_PAGER_CODE_MODEL_x86_64 := large
RUST_VMO_PAGER_CODE_MODEL_aarch64 := large
RUST_VMO_PAGER_CODE_MODEL_loongarch64 := medium
RUST_VMO_PAGER_CODE_MODEL_riscv64 := medium
RUST_VMO_PAGER_CODE_MODEL_arm32 := small
RUST_VMO_PAGER_FEATURES_aarch64 := -neon,-fp-armv8
RUST_VMO_PAGER_FEATURES_arm32 := -neon
RUST_VMO_PAGER_EXTRA_x86_64 := -C no-redzone=yes
RUST_VMO_PAGER_CODE_MODEL := $(RUST_VMO_PAGER_CODE_MODEL_$(ARCH))
RUST_VMO_PAGER_FEATURES := $(RUST_VMO_PAGER_FEATURES_$(ARCH))
RUST_VMO_PAGER_EXTRA := $(RUST_VMO_PAGER_EXTRA_$(ARCH))
RUST_VMO_PAGER_FLAGS_STAMP := $(BUILD_DIR)/rust/.vmo-pager-flags
.PHONY: FORCE_RUST_VMO_PAGER_FLAGS
FORCE_RUST_VMO_PAGER_FLAGS:

$(RUST_VMO_PAGER_FLAGS_STAMP): FORCE_RUST_VMO_PAGER_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_VMO_PAGER_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_VMO_PAGER_CODE_MODEL) $(RUST_VMO_PAGER_EXTRA) -C target-feature=$(RUST_VMO_PAGER_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_VMO_PAGER_OBJECT): $(RUST_VMO_PAGER_SRC) tools/rust-kernel.mk $(RUST_VMO_PAGER_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_VMO_PAGER_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_VMO_PAGER_CODE_MODEL),-C code-model=$(RUST_VMO_PAGER_CODE_MODEL)) \
		$(RUST_VMO_PAGER_EXTRA) \
		$(if $(RUST_VMO_PAGER_FEATURES),-C target-feature=$(RUST_VMO_PAGER_FEATURES)) \
		$< -o $@
endif

# Completion owns its persistent count and wait-token lifecycle in Rust; C
# provides the IRQ-save lock and wait-queue primitives.
RUST_COMPLETION_STATE_SRC := kernel/core/completion_state.rs
RUST_COMPLETION_STATE_TARGET_x86_64 := x86_64-unknown-none
RUST_COMPLETION_STATE_TARGET_aarch64 := aarch64-unknown-none
RUST_COMPLETION_STATE_TARGET_loongarch64 := loongarch64-unknown-none
RUST_COMPLETION_STATE_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_COMPLETION_STATE_TARGET_arm32 := armv7a-none-eabihf
RUST_COMPLETION_STATE_TARGET := $(RUST_COMPLETION_STATE_TARGET_$(ARCH))
RUST_COMPLETION_STATE_OBJECT := $(BUILD_DIR)/rust/completion_state.o

ifeq ($(COMPLETION_IMPL),rust)
ifeq ($(strip $(RUST_COMPLETION_STATE_TARGET)),)
$(error COMPLETION_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_COMPLETION_STATE_CODE_MODEL_x86_64 := large
RUST_COMPLETION_STATE_CODE_MODEL_aarch64 := large
RUST_COMPLETION_STATE_CODE_MODEL_loongarch64 := medium
RUST_COMPLETION_STATE_CODE_MODEL_riscv64 := medium
RUST_COMPLETION_STATE_CODE_MODEL_arm32 := small
RUST_COMPLETION_STATE_FEATURES_aarch64 := -neon,-fp-armv8
RUST_COMPLETION_STATE_FEATURES_arm32 := -neon
RUST_COMPLETION_STATE_EXTRA_x86_64 := -C no-redzone=yes
RUST_COMPLETION_STATE_CODE_MODEL := $(RUST_COMPLETION_STATE_CODE_MODEL_$(ARCH))
RUST_COMPLETION_STATE_FEATURES := $(RUST_COMPLETION_STATE_FEATURES_$(ARCH))
RUST_COMPLETION_STATE_EXTRA := $(RUST_COMPLETION_STATE_EXTRA_$(ARCH))
RUST_COMPLETION_STATE_FLAGS_STAMP := $(BUILD_DIR)/rust/.completion-state-flags
.PHONY: FORCE_RUST_COMPLETION_STATE_FLAGS
FORCE_RUST_COMPLETION_STATE_FLAGS:

$(RUST_COMPLETION_STATE_FLAGS_STAMP): FORCE_RUST_COMPLETION_STATE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_COMPLETION_STATE_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_COMPLETION_STATE_CODE_MODEL) $(RUST_COMPLETION_STATE_EXTRA) -C target-feature=$(RUST_COMPLETION_STATE_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_COMPLETION_STATE_OBJECT): $(RUST_COMPLETION_STATE_SRC) tools/rust-kernel.mk $(RUST_COMPLETION_STATE_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_COMPLETION_STATE_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_COMPLETION_STATE_CODE_MODEL),-C code-model=$(RUST_COMPLETION_STATE_CODE_MODEL)) \
		$(RUST_COMPLETION_STATE_EXTRA) \
		$(if $(RUST_COMPLETION_STATE_FEATURES),-C target-feature=$(RUST_COMPLETION_STATE_FEATURES)) \
		$< -o $@
endif

# VMA region arithmetic and split/merge transaction decisions live in Rust;
# C callbacks are limited to segment allocation/ref primitives and list/PTE
# annotation updates.
RUST_VMA_REGION_SRC := kernel/mm/vma_region.rs
RUST_VMA_REGION_TARGET_x86_64 := x86_64-unknown-none
RUST_VMA_REGION_TARGET_aarch64 := aarch64-unknown-none
RUST_VMA_REGION_TARGET_loongarch64 := loongarch64-unknown-none
RUST_VMA_REGION_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_VMA_REGION_TARGET_arm32 := armv7a-none-eabihf
RUST_VMA_REGION_TARGET := $(RUST_VMA_REGION_TARGET_$(ARCH))
RUST_VMA_REGION_OBJECT := $(BUILD_DIR)/rust/vma_region.o

ifeq ($(VMA_IMPL),rust)
ifeq ($(strip $(RUST_VMA_REGION_TARGET)),)
$(error VMA_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_VMA_REGION_CODE_MODEL_x86_64 := large
RUST_VMA_REGION_CODE_MODEL_aarch64 := large
RUST_VMA_REGION_CODE_MODEL_loongarch64 := medium
RUST_VMA_REGION_CODE_MODEL_riscv64 := medium
RUST_VMA_REGION_CODE_MODEL_arm32 := small
RUST_VMA_REGION_FEATURES_aarch64 := -neon,-fp-armv8
RUST_VMA_REGION_FEATURES_arm32 := -neon
RUST_VMA_REGION_EXTRA_x86_64 := -C no-redzone=yes
RUST_VMA_REGION_CODE_MODEL := $(RUST_VMA_REGION_CODE_MODEL_$(ARCH))
RUST_VMA_REGION_FEATURES := $(RUST_VMA_REGION_FEATURES_$(ARCH))
RUST_VMA_REGION_EXTRA := $(RUST_VMA_REGION_EXTRA_$(ARCH))
RUST_VMA_REGION_FLAGS_STAMP := $(BUILD_DIR)/rust/.vma-region-flags
.PHONY: FORCE_RUST_VMA_REGION_FLAGS
FORCE_RUST_VMA_REGION_FLAGS:

$(RUST_VMA_REGION_FLAGS_STAMP): FORCE_RUST_VMA_REGION_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_VMA_REGION_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_VMA_REGION_CODE_MODEL) $(RUST_VMA_REGION_EXTRA) -C target-feature=$(RUST_VMA_REGION_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_VMA_REGION_OBJECT): $(RUST_VMA_REGION_SRC) tools/rust-kernel.mk $(RUST_VMA_REGION_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_VMA_REGION_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_VMA_REGION_CODE_MODEL),-C code-model=$(RUST_VMA_REGION_CODE_MODEL)) \
		$(RUST_VMA_REGION_EXTRA) \
		$(if $(RUST_VMA_REGION_FEATURES),-C target-feature=$(RUST_VMA_REGION_FEATURES)) \
		$< -o $@
endif

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

$(RUST_HANDLE_SLOT_OBJECT): $(RUST_HANDLE_SLOT_SRC) $(RUST_HANDLE_ENTRY_SRC) tools/rust-kernel.mk $(RUST_HANDLE_SLOT_FLAGS_STAMP)
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

$(RUST_HANDLE_TABLE_STATE_OBJECT): $(RUST_HANDLE_TABLE_STATE_SRC) $(RUST_HANDLE_ENTRY_SRC) $(RUST_HANDLE_TABLE_STATE_HDR) $(RUST_HANDLE_SLOT_OBJECT) tools/rust-kernel.mk $(RUST_HANDLE_SLOT_FLAGS_STAMP)
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

ifeq ($(USER_COPY_IMPL),rust)
RUST_USERCOPY_SRC := kernel/mm/usercopy.rs
RUST_USERCOPY_SUBMODULES := kernel/mm/usercopy_strings.rs
RUST_USERCOPY_TARGET_x86_64 := x86_64-unknown-none
RUST_USERCOPY_TARGET_aarch64 := aarch64-unknown-none
RUST_USERCOPY_TARGET_loongarch64 := loongarch64-unknown-none
RUST_USERCOPY_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_USERCOPY_TARGET_arm32 := armv7a-none-eabihf
RUST_USERCOPY_TARGET := $(RUST_USERCOPY_TARGET_$(ARCH))
ifeq ($(strip $(RUST_USERCOPY_TARGET)),)
$(error USER_COPY_IMPL=rust has no Rust target for ARCH=$(ARCH); keep the C implementation selected)
endif
RUST_USERCOPY_OBJECT := $(BUILD_DIR)/rust/usercopy.o
RUST_USERCOPY_CODE_MODEL_x86_64 := large
RUST_USERCOPY_CODE_MODEL_aarch64 := large
RUST_USERCOPY_CODE_MODEL_loongarch64 := medium
RUST_USERCOPY_CODE_MODEL_riscv64 := medium
RUST_USERCOPY_CODE_MODEL_arm32 := small
RUST_USERCOPY_FEATURES_aarch64 := -neon,-fp-armv8
RUST_USERCOPY_FEATURES_arm32 := -neon
RUST_USERCOPY_EXTRA_x86_64 := -C no-redzone=yes
RUST_USERCOPY_CODE_MODEL := $(RUST_USERCOPY_CODE_MODEL_$(ARCH))
RUST_USERCOPY_FEATURES := $(RUST_USERCOPY_FEATURES_$(ARCH))
RUST_USERCOPY_EXTRA := $(RUST_USERCOPY_EXTRA_$(ARCH))
RUST_USERCOPY_FLAGS_STAMP := $(BUILD_DIR)/rust/.usercopy-flags
.PHONY: FORCE_RUST_USERCOPY_FLAGS
FORCE_RUST_USERCOPY_FLAGS:

$(RUST_USERCOPY_FLAGS_STAMP): FORCE_RUST_USERCOPY_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_USERCOPY_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_USERCOPY_CODE_MODEL) $(RUST_USERCOPY_EXTRA) -C target-feature=$(RUST_USERCOPY_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_USERCOPY_OBJECT): $(RUST_USERCOPY_SRC) $(RUST_USERCOPY_SUBMODULES) tools/rust-kernel.mk $(RUST_USERCOPY_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_USERCOPY_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_USERCOPY_CODE_MODEL),-C code-model=$(RUST_USERCOPY_CODE_MODEL)) \
		$(RUST_USERCOPY_EXTRA) \
		$(if $(RUST_USERCOPY_FEATURES),-C target-feature=$(RUST_USERCOPY_FEATURES)) \
		$< -o $@
endif

ifeq ($(PROCESS_VM_IMPL),rust)
RUST_PROCESS_VM_SRC := kernel/mm/process_vm.rs
RUST_PROCESS_VM_TARGET_x86_64 := x86_64-unknown-none
RUST_PROCESS_VM_TARGET_aarch64 := aarch64-unknown-none
RUST_PROCESS_VM_TARGET_loongarch64 := loongarch64-unknown-none
RUST_PROCESS_VM_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_PROCESS_VM_TARGET_arm32 := armv7a-none-eabihf
RUST_PROCESS_VM_TARGET := $(RUST_PROCESS_VM_TARGET_$(ARCH))
ifeq ($(strip $(RUST_PROCESS_VM_TARGET)),)
$(error PROCESS_VM_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_PROCESS_VM_OBJECT := $(BUILD_DIR)/rust/process_vm.o
RUST_PROCESS_VM_CODE_MODEL_x86_64 := large
RUST_PROCESS_VM_CODE_MODEL_aarch64 := large
RUST_PROCESS_VM_CODE_MODEL_loongarch64 := medium
RUST_PROCESS_VM_CODE_MODEL_riscv64 := medium
RUST_PROCESS_VM_CODE_MODEL_arm32 := small
RUST_PROCESS_VM_FEATURES_aarch64 := -neon,-fp-armv8
RUST_PROCESS_VM_FEATURES_arm32 := -neon
RUST_PROCESS_VM_EXTRA_x86_64 := -C no-redzone=yes
RUST_PROCESS_VM_CODE_MODEL := $(RUST_PROCESS_VM_CODE_MODEL_$(ARCH))
RUST_PROCESS_VM_FEATURES := $(RUST_PROCESS_VM_FEATURES_$(ARCH))
RUST_PROCESS_VM_EXTRA := $(RUST_PROCESS_VM_EXTRA_$(ARCH))
$(RUST_PROCESS_VM_OBJECT): $(RUST_PROCESS_VM_SRC) tools/rust-kernel.mk
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_PROCESS_VM_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_PROCESS_VM_CODE_MODEL),-C code-model=$(RUST_PROCESS_VM_CODE_MODEL)) \
		$(RUST_PROCESS_VM_EXTRA) \
		$(if $(RUST_PROCESS_VM_FEATURES),-C target-feature=$(RUST_PROCESS_VM_FEATURES)) \
		$< -o $@
endif

# PARK_WAIT is independent of USER_COPY. Keep its target/object variables
# outside the USER_COPY_IMPL block so either Rust slice can be selected alone.
RUST_PARK_WAIT_SRC := kernel/proc/park_wait_state.rs
RUST_PARK_WAIT_HDR := kernel/proc/park_wait_state.h
RUST_PARK_WAIT_TARGET_x86_64 := x86_64-unknown-none
RUST_PARK_WAIT_TARGET_aarch64 := aarch64-unknown-none
RUST_PARK_WAIT_TARGET_loongarch64 := loongarch64-unknown-none
RUST_PARK_WAIT_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_PARK_WAIT_TARGET_arm32 := armv7a-none-eabihf
RUST_PARK_WAIT_TARGET := $(RUST_PARK_WAIT_TARGET_$(ARCH))
RUST_PARK_WAIT_OBJECT := $(BUILD_DIR)/rust/park_wait_state.o

ifeq ($(PARK_WAIT_IMPL),rust)
ifeq ($(strip $(RUST_PARK_WAIT_TARGET)),)
$(error PARK_WAIT_IMPL=rust has no Rust target for ARCH=$(ARCH); keep the C reference selected)
endif
RUST_PARK_WAIT_CODE_MODEL_x86_64 := large
RUST_PARK_WAIT_CODE_MODEL_aarch64 := large
RUST_PARK_WAIT_CODE_MODEL_loongarch64 := medium
RUST_PARK_WAIT_CODE_MODEL_riscv64 := medium
RUST_PARK_WAIT_CODE_MODEL_arm32 := small
RUST_PARK_WAIT_FEATURES_aarch64 := -neon,-fp-armv8
RUST_PARK_WAIT_FEATURES_arm32 := -neon
RUST_PARK_WAIT_EXTRA_x86_64 := -C no-redzone=yes
RUST_PARK_WAIT_CODE_MODEL := $(RUST_PARK_WAIT_CODE_MODEL_$(ARCH))
RUST_PARK_WAIT_FEATURES := $(RUST_PARK_WAIT_FEATURES_$(ARCH))
RUST_PARK_WAIT_EXTRA := $(RUST_PARK_WAIT_EXTRA_$(ARCH))
RUST_PARK_WAIT_FLAGS_STAMP := $(BUILD_DIR)/rust/.park-wait-flags
.PHONY: FORCE_RUST_PARK_WAIT_FLAGS
FORCE_RUST_PARK_WAIT_FLAGS:

$(RUST_PARK_WAIT_FLAGS_STAMP): FORCE_RUST_PARK_WAIT_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_PARK_WAIT_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_PARK_WAIT_CODE_MODEL) $(RUST_PARK_WAIT_EXTRA) -C target-feature=$(RUST_PARK_WAIT_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_PARK_WAIT_OBJECT): $(RUST_PARK_WAIT_SRC) $(RUST_PARK_WAIT_HDR) tools/rust-kernel.mk $(RUST_PARK_WAIT_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_PARK_WAIT_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_PARK_WAIT_CODE_MODEL),-C code-model=$(RUST_PARK_WAIT_CODE_MODEL)) \
		$(RUST_PARK_WAIT_EXTRA) \
		$(if $(RUST_PARK_WAIT_FEATURES),-C target-feature=$(RUST_PARK_WAIT_FEATURES)) \
		$< -o $@
endif

# WAIT_TIMER owns deadline heap membership/ref transitions; C retains the
# IRQ-safe spinlock and park wake callback. Keep this independently selectable.
RUST_WAIT_TIMER_SRC := kernel/proc/wait_timer.rs
RUST_WAIT_TIMER_TARGET_x86_64 := x86_64-unknown-none
RUST_WAIT_TIMER_TARGET_aarch64 := aarch64-unknown-none
RUST_WAIT_TIMER_TARGET_loongarch64 := loongarch64-unknown-none
RUST_WAIT_TIMER_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_WAIT_TIMER_TARGET_arm32 := armv7a-none-eabihf
RUST_WAIT_TIMER_TARGET := $(RUST_WAIT_TIMER_TARGET_$(ARCH))
RUST_WAIT_TIMER_OBJECT := $(BUILD_DIR)/rust/wait_timer.o

ifeq ($(WAIT_TIMER_IMPL),rust)
ifeq ($(strip $(RUST_WAIT_TIMER_TARGET)),)
$(error WAIT_TIMER_IMPL=rust has no Rust target for ARCH=$(ARCH); keep the C reference selected)
endif
RUST_WAIT_TIMER_CODE_MODEL_x86_64 := large
RUST_WAIT_TIMER_CODE_MODEL_aarch64 := large
RUST_WAIT_TIMER_CODE_MODEL_loongarch64 := medium
RUST_WAIT_TIMER_CODE_MODEL_riscv64 := medium
RUST_WAIT_TIMER_CODE_MODEL_arm32 := small
RUST_WAIT_TIMER_FEATURES_aarch64 := -neon,-fp-armv8
RUST_WAIT_TIMER_FEATURES_arm32 := -neon
RUST_WAIT_TIMER_EXTRA_x86_64 := -C no-redzone=yes
RUST_WAIT_TIMER_CODE_MODEL := $(RUST_WAIT_TIMER_CODE_MODEL_$(ARCH))
RUST_WAIT_TIMER_FEATURES := $(RUST_WAIT_TIMER_FEATURES_$(ARCH))
RUST_WAIT_TIMER_EXTRA := $(RUST_WAIT_TIMER_EXTRA_$(ARCH))
RUST_WAIT_TIMER_FLAGS_STAMP := $(BUILD_DIR)/rust/.wait-timer-flags
.PHONY: FORCE_RUST_WAIT_TIMER_FLAGS
FORCE_RUST_WAIT_TIMER_FLAGS:

$(RUST_WAIT_TIMER_FLAGS_STAMP): FORCE_RUST_WAIT_TIMER_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_WAIT_TIMER_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_WAIT_TIMER_CODE_MODEL) $(RUST_WAIT_TIMER_EXTRA) -C target-feature=$(RUST_WAIT_TIMER_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_WAIT_TIMER_OBJECT): $(RUST_WAIT_TIMER_SRC) tools/rust-kernel.mk $(RUST_WAIT_TIMER_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_WAIT_TIMER_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_WAIT_TIMER_CODE_MODEL),-C code-model=$(RUST_WAIT_TIMER_CODE_MODEL)) \
		$(RUST_WAIT_TIMER_EXTRA) \
		$(if $(RUST_WAIT_TIMER_FEATURES),-C target-feature=$(RUST_WAIT_TIMER_FEATURES)) \
		$< -o $@
endif

# WAIT_QUEUE owns intrusive entry membership and published queue-owner state;
# C keeps the IRQ-saving locks, references, and wake callbacks.
RUST_WAIT_QUEUE_STATE_SRC := kernel/core/wait_queue_state.rs
RUST_WAIT_QUEUE_STATE_TARGET_x86_64 := x86_64-unknown-none
RUST_WAIT_QUEUE_STATE_TARGET_aarch64 := aarch64-unknown-none
RUST_WAIT_QUEUE_STATE_TARGET_loongarch64 := loongarch64-unknown-none
RUST_WAIT_QUEUE_STATE_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_WAIT_QUEUE_STATE_TARGET_arm32 := armv7a-none-eabihf
RUST_WAIT_QUEUE_STATE_TARGET := $(RUST_WAIT_QUEUE_STATE_TARGET_$(ARCH))
RUST_WAIT_QUEUE_STATE_OBJECT := $(BUILD_DIR)/rust/wait_queue_state.o

ifeq ($(WAIT_QUEUE_IMPL),rust)
ifeq ($(strip $(RUST_WAIT_QUEUE_STATE_TARGET)),)
$(error WAIT_QUEUE_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_WAIT_QUEUE_STATE_CODE_MODEL_x86_64 := large
RUST_WAIT_QUEUE_STATE_CODE_MODEL_aarch64 := large
RUST_WAIT_QUEUE_STATE_CODE_MODEL_loongarch64 := medium
RUST_WAIT_QUEUE_STATE_CODE_MODEL_riscv64 := medium
RUST_WAIT_QUEUE_STATE_CODE_MODEL_arm32 := small
RUST_WAIT_QUEUE_STATE_FEATURES_aarch64 := -neon,-fp-armv8
RUST_WAIT_QUEUE_STATE_FEATURES_arm32 := -neon
RUST_WAIT_QUEUE_STATE_EXTRA_x86_64 := -C no-redzone=yes
RUST_WAIT_QUEUE_STATE_CODE_MODEL := $(RUST_WAIT_QUEUE_STATE_CODE_MODEL_$(ARCH))
RUST_WAIT_QUEUE_STATE_FEATURES := $(RUST_WAIT_QUEUE_STATE_FEATURES_$(ARCH))
RUST_WAIT_QUEUE_STATE_EXTRA := $(RUST_WAIT_QUEUE_STATE_EXTRA_$(ARCH))
RUST_WAIT_QUEUE_STATE_FLAGS_STAMP := $(BUILD_DIR)/rust/.wait-queue-state-flags
.PHONY: FORCE_RUST_WAIT_QUEUE_STATE_FLAGS
FORCE_RUST_WAIT_QUEUE_STATE_FLAGS:

$(RUST_WAIT_QUEUE_STATE_FLAGS_STAMP): FORCE_RUST_WAIT_QUEUE_STATE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_WAIT_QUEUE_STATE_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_WAIT_QUEUE_STATE_CODE_MODEL) $(RUST_WAIT_QUEUE_STATE_EXTRA) -C target-feature=$(RUST_WAIT_QUEUE_STATE_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_WAIT_QUEUE_STATE_OBJECT): $(RUST_WAIT_QUEUE_STATE_SRC) tools/rust-kernel.mk $(RUST_WAIT_QUEUE_STATE_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_WAIT_QUEUE_STATE_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_WAIT_QUEUE_STATE_CODE_MODEL),-C code-model=$(RUST_WAIT_QUEUE_STATE_CODE_MODEL)) \
		$(RUST_WAIT_QUEUE_STATE_EXTRA) \
		$(if $(RUST_WAIT_QUEUE_STATE_FEATURES),-C target-feature=$(RUST_WAIT_QUEUE_STATE_FEATURES)) \
		$< -o $@
endif

# RW_MUTEX owns reader/writer admission, waiting-writer priority, and owner
# transitions; C retains the IRQ-saving lock, task parking, and wait queues.
RUST_RW_MUTEX_STATE_SRC := kernel/core/rw_mutex_state.rs
RUST_RW_MUTEX_STATE_TARGET_x86_64 := x86_64-unknown-none
RUST_RW_MUTEX_STATE_TARGET_aarch64 := aarch64-unknown-none
RUST_RW_MUTEX_STATE_TARGET_loongarch64 := loongarch64-unknown-none
RUST_RW_MUTEX_STATE_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_RW_MUTEX_STATE_TARGET_arm32 := armv7a-none-eabihf
RUST_RW_MUTEX_STATE_TARGET := $(RUST_RW_MUTEX_STATE_TARGET_$(ARCH))
RUST_RW_MUTEX_STATE_OBJECT := $(BUILD_DIR)/rust/rw_mutex_state.o

ifeq ($(RW_MUTEX_IMPL),rust)
ifeq ($(strip $(RUST_RW_MUTEX_STATE_TARGET)),)
$(error RW_MUTEX_IMPL=rust has no Rust target for ARCH=$(ARCH))
endif
RUST_RW_MUTEX_STATE_CODE_MODEL_x86_64 := large
RUST_RW_MUTEX_STATE_CODE_MODEL_aarch64 := large
RUST_RW_MUTEX_STATE_CODE_MODEL_loongarch64 := medium
RUST_RW_MUTEX_STATE_CODE_MODEL_riscv64 := medium
RUST_RW_MUTEX_STATE_CODE_MODEL_arm32 := small
RUST_RW_MUTEX_STATE_FEATURES_aarch64 := -neon,-fp-armv8
RUST_RW_MUTEX_STATE_FEATURES_arm32 := -neon
RUST_RW_MUTEX_STATE_EXTRA_x86_64 := -C no-redzone=yes
RUST_RW_MUTEX_STATE_CODE_MODEL := $(RUST_RW_MUTEX_STATE_CODE_MODEL_$(ARCH))
RUST_RW_MUTEX_STATE_FEATURES := $(RUST_RW_MUTEX_STATE_FEATURES_$(ARCH))
RUST_RW_MUTEX_STATE_EXTRA := $(RUST_RW_MUTEX_STATE_EXTRA_$(ARCH))
RUST_RW_MUTEX_STATE_FLAGS_STAMP := $(BUILD_DIR)/rust/.rw-mutex-state-flags
.PHONY: FORCE_RUST_RW_MUTEX_STATE_FLAGS
FORCE_RUST_RW_MUTEX_STATE_FLAGS:

$(RUST_RW_MUTEX_STATE_FLAGS_STAMP): FORCE_RUST_RW_MUTEX_STATE_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RUSTC) $(shell $(RUSTC) --version --verbose 2>/dev/null) --target $(RUST_RW_MUTEX_STATE_TARGET) -C panic=abort -C opt-level=$(RUST_OPT_LEVEL) -C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 -C relocation-model=static -C code-model=$(RUST_RW_MUTEX_STATE_CODE_MODEL) $(RUST_RW_MUTEX_STATE_EXTRA) -C target-feature=$(RUST_RW_MUTEX_STATE_FEATURES)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(RUST_RW_MUTEX_STATE_OBJECT): $(RUST_RW_MUTEX_STATE_SRC) tools/rust-kernel.mk $(RUST_RW_MUTEX_STATE_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj \
		--target $(RUST_RW_MUTEX_STATE_TARGET) \
		-C panic=abort -C opt-level=$(RUST_OPT_LEVEL) \
		-C debuginfo=$(RUST_DEBUG_INFO) -C codegen-units=1 \
		-C relocation-model=static \
		$(if $(RUST_RW_MUTEX_STATE_CODE_MODEL),-C code-model=$(RUST_RW_MUTEX_STATE_CODE_MODEL)) \
		$(RUST_RW_MUTEX_STATE_EXTRA) \
		$(if $(RUST_RW_MUTEX_STATE_FEATURES),-C target-feature=$(RUST_RW_MUTEX_STATE_FEATURES)) \
		$< -o $@
endif

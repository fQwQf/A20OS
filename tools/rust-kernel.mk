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

ifeq ($(NETFILTER_REWRITE_IMPL),rust)
ifeq ($(strip $(RUST_NETFILTER_TARGET)),)
$(error NETFILTER_REWRITE_IMPL=rust has no target configured for ARCH=$(ARCH); use the C reference)
endif

RUST_OPT_LEVEL := $(if $(filter -Os,$(OPT)),s,$(patsubst -O%,%,$(OPT)))
ifeq ($(filter $(OPT),-O1 -O2 -O3 -Os),)
$(error NETFILTER_REWRITE_IMPL=rust supports OPT=-O1/-O2/-O3/-Os; -O0 leaves core panic/overflow symbols unresolved in this no_std kernel object)
endif
RUST_DEBUG_INFO := $(if $(findstring -g,$(OPT)),2,0)
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

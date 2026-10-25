# Explicitly selected user-space service. No MM/scheduler hooks or default
# startup work. User Rust uses the user ABI, not the kernel soft-float target.
CLUSTER_MEMORY ?= 0
CLUSTER_MEMORY_PAGES ?= 8
ifeq ($(filter $(CLUSTER_MEMORY),0 1),)
$(error CLUSTER_MEMORY must be 0 or 1)
endif
ifeq ($(filter $(CLUSTER_MEMORY_PAGES),1 2 4 8 16 32 64),)
$(error CLUSTER_MEMORY_PAGES must be one of 1 2 4 8 16 32 64)
endif
MEMORY_RUST_TARGET_x86_64 := x86_64-unknown-none
MEMORY_RUST_TARGET_riscv64 := riscv64gc-unknown-none-elf
MEMORY_RUST_TARGET_aarch64 := aarch64-unknown-none
MEMORY_RUST_TARGET_loongarch64 := loongarch64-unknown-none
MEMORY_RUST_TARGET := $(MEMORY_RUST_TARGET_$(ARCH))
ifeq ($(CLUSTER_MEMORY),1)
ifeq ($(MEMORY_RUST_TARGET),)
$(error cluster memory currently supports x86_64/riscv64/aarch64/loongarch64 user targets)
endif
endif
MEMORY_RUST_OBJECT := $(BUILD_DIR)/cluster-memory/pages-$(CLUSTER_MEMORY_PAGES).a
MEMORY_SERVICE_BIN := $(NATIVE_BUILD_DIR)/cluster-memory-$(NATIVE_TAG)
MEMORY_TEST_BIN := $(NATIVE_BUILD_DIR)/native-cluster-memory-$(NATIVE_TAG)
MEMORY_DEPS := $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) $(NATIVE_SDK_HDRS) $(NATIVE_LD)

.PHONY: native-cluster-memory check-cluster-memory check-cluster-memory-network FORCE_MEMORY_FLAGS print-cluster-memory-paths
print-cluster-memory-paths:
	@printf '%s\n' '$(MEMORY_SERVICE_BIN)' '$(MEMORY_TEST_BIN)'
native-cluster-memory:
ifeq ($(CLUSTER_MEMORY),1)
native-cluster-memory: $(MEMORY_SERVICE_BIN) $(MEMORY_TEST_BIN)
else
	@echo 'Enable explicitly: make CLUSTER_MEMORY=1 native-cluster-memory' >&2; exit 2
endif

# The service ELF has a stable deployment name. Track page-cap and toolchain
# changes even when revisiting an older .o after changing the cap back.
$(BUILD_DIR)/cluster-memory/.flags: FORCE_MEMORY_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' 'pages=$(CLUSTER_MEMORY_PAGES) target=$(MEMORY_RUST_TARGET) rustc=$(RUSTC) $(shell $(RUSTC) --version) cflags=$(NATIVE_CFLAGS)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(MEMORY_RUST_OBJECT): user/svc/cluster_memory_state.rs kernel/include/uapi/a20/cluster_memory.h $(BUILD_DIR)/cluster-memory/.flags tools/cluster-memory.mk
	@mkdir -p $(dir $@)
	A20_MEMORY_MAX_PAGES=$(CLUSTER_MEMORY_PAGES) $(RUSTC) --edition=2021 --crate-type staticlib \
		--target $(MEMORY_RUST_TARGET) -C panic=abort -C opt-level=2 -C codegen-units=1 \
		-C relocation-model=static -C code-model=$(if $(filter riscv64 loongarch64,$(ARCH)),medium,small) $< -o $@

$(MEMORY_SERVICE_BIN): user/svc/cluster_memory.c $(MEMORY_RUST_OBJECT) $(MEMORY_DEPS)
	$(call NATIVE_CLUSTER_APP_RECIPE,-Wl$(comma)--gc-sections -DA20_MEMORY_MAX_PAGES=$(CLUSTER_MEMORY_PAGES) user/svc/cluster_memory.c $(MEMORY_RUST_OBJECT),$@)

$(MEMORY_TEST_BIN): user/tests/test_native_cluster_memory.c $(MEMORY_DEPS)
	$(call NATIVE_CLUSTER_APP_RECIPE,user/tests/test_native_cluster_memory.c,$@)

ifeq ($(CLUSTER_MEMORY),1)
NATIVE_OUTPUTS += $(MEMORY_SERVICE_BIN) $(MEMORY_TEST_BIN)
USER_BUILD_ID := $(USER_BUILD_ID):CLUSTER_MEMORY=1:CLUSTER_MEMORY_PAGES=$(CLUSTER_MEMORY_PAGES)
native-programs: native-cluster-memory
endif

check-cluster-memory:
	$(PYTHON) tools/check-cluster-memory.py

check-cluster-memory-network:
	$(PYTHON) tools/check-cluster-memory-network.py

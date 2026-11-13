# Shared LLVM selection. GCC remains the default; LLVM uses LLD/compiler-rt.
TOOLCHAIN ?= gcc
ifneq ($(filter $(TOOLCHAIN),gcc llvm),$(TOOLCHAIN))
$(error TOOLCHAIN must be gcc or llvm)
endif
LLVM_PREFIX ?=
LLVM_SUFFIX ?=
CLANG ?= $(LLVM_PREFIX)clang$(LLVM_SUFFIX)
LLVM_TARGET_riscv64 := riscv64-unknown-linux-gnu
LLVM_TARGET_riscv32 := riscv32-unknown-linux-gnu
LLVM_TARGET_aarch64 := aarch64-unknown-linux-gnu
LLVM_TARGET_x86_64 := x86_64-unknown-linux-gnu
LLVM_TARGET_arm32 := arm-linux-gnueabihf
LLVM_TARGET_armv7m := arm-none-eabi
LLVM_TARGET_loongarch64 := loongarch64-unknown-linux-gnu
LLVM_TARGET_ppc64le := powerpc64le-unknown-linux-gnu
ifeq ($(TOOLCHAIN),llvm)
ifeq ($(LLVM_TARGET_$(ARCH)),)
$(error TOOLCHAIN=llvm is not supported for ARCH=$(ARCH))
endif
LLVM_CC = $(CCACHE_PREFIX)$(CLANG) --target=$(LLVM_TARGET_$(ARCH))
endif
# Python-driven recursive builds inherit these explicitly (their make command
# lines deliberately discard unrelated parent overrides).
export TOOLCHAIN LLVM_PREFIX LLVM_SUFFIX CLANG

# GCC accepts these existing GNU C idioms or emits a different warning set.
# Keep useful Clang diagnostics visible while preserving the other -Werror gates.
LLVM_KERNEL_WARNINGS := -Wno-typedef-redefinition -Wno-empty-body -Wno-c23-extensions \
    -Wno-error=unused-function -Wno-error=self-assign \
    -Wno-error=misleading-indentation -Wno-error=atomic-alignment \
    -Wno-error=tautological-constant-compare

A20_TOOLCHAIN_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_RT_ROOT ?= $(A20_TOOLCHAIN_ROOT)/build/toolchains/compiler-rt
LLVM_USER_BUILTINS = $(LLVM_RT_ROOT)/$(ARCH)-user/builtins.a
LLVM_KERNEL_RT_KIND = $(if $(and $(filter riscv32,$(ARCH)),$(filter mcu,$(PROFILE))),mcu,kernel)
LLVM_KERNEL_BUILTINS = $(LLVM_RT_ROOT)/$(ARCH)-$(LLVM_KERNEL_RT_KIND)/builtins.a
LLVM_LD = $(LLVM_PREFIX)ld.lld$(LLVM_SUFFIX)
LLVM_LINK = --ld-path=$(LLVM_LD)
export LLVM_RT_ROOT

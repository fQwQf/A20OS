# ----------------------------------------------------------------
# mlibc port (sysdeps/a20) — full libc on the native ABI
# ----------------------------------------------------------------
MLIBC_DIR      := user/external/mlibc
MLIBC_BUILD    := $(MLIBC_DIR)/build-a20-riscv64
MLIBC_SYSROOT  := user/build/mlibc-sysroot
MLIBC_HELLO_BIN := $(NATIVE_BUILD_DIR)/mlibc-hello-$(NATIVE_TAG)
MLIBC_CHILD_BIN := $(NATIVE_BUILD_DIR)/mlibc-child-$(NATIVE_TAG)
MLIBC_FAST_TYPE_FLAGS := \
	-D__INT_FAST8_TYPE__="signed char" -D__INT_FAST16_TYPE__=long -D__INT_FAST32_TYPE__=long \
	-D__UINT_FAST8_TYPE__="unsigned char" -D__UINT_FAST16_TYPE__="unsigned long" -D__UINT_FAST32_TYPE__="unsigned long"

$(MLIBC_SYSROOT)/lib/libc.a: $(wildcard $(MLIBC_DIR)/sysdeps/a20/*) \
	$(MLIBC_DIR)/options/posix/include/spawn.h \
	$(MLIBC_DIR)/ci/a20-riscv64.cross-file
	@test -d "$(MLIBC_BUILD)" || meson setup $(MLIBC_BUILD) $(MLIBC_DIR) \
		--cross-file $(MLIBC_DIR)/ci/a20-riscv64.cross-file \
		-Ddefault_library=static -Dbuild_tests=false --prefix=$(abspath $(MLIBC_SYSROOT))
	ninja -C $(MLIBC_BUILD)
	meson install -C $(MLIBC_BUILD) --no-rebuild --quiet

mlibc-sysroot: $(MLIBC_SYSROOT)/lib/libc.a

define MLIBC_LINK_RECIPE
@mkdir -p $(dir $(3))
$(RISCV_ELF_PREFIX)gcc -march=rv64gc -mabi=lp64d -mcmodel=medany -static -nostdlib \
	-D_GNU_SOURCE $(MLIBC_FAST_TYPE_FLAGS) \
	-isystem $(MLIBC_SYSROOT)/include \
	-T user/mlibc/a20-mlibc.ld \
	-Wl,-s \
	$(MLIBC_SYSROOT)/lib/crt1.o $(MLIBC_SYSROOT)/lib/a20_thread_entry.o \
	$(1) \
	-L$(MLIBC_SYSROOT)/lib $(2) -lgcc \
	-o $(3)
endef

$(MLIBC_HELLO_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_hello.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_hello.c,-lc -lpthread -lm -lrt,$@)

$(MLIBC_CHILD_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_child.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_child.c,-lc,$@)

MLIBC_FORK_BIN := $(NATIVE_BUILD_DIR)/mlibc-fork-$(NATIVE_TAG)
$(MLIBC_FORK_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_fork.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_fork.c,-lc,$@)

MLIBC_SIGCHLD_BIN := $(NATIVE_BUILD_DIR)/mlibc-sigchld-$(NATIVE_TAG)
$(MLIBC_SIGCHLD_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_sigchld.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_sigchld.c,-lc,$@)

MLIBC_PIPEEXEC_BIN := $(NATIVE_BUILD_DIR)/mlibc-pipeexec-$(NATIVE_TAG)
$(MLIBC_PIPEEXEC_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_pipeexec.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_pipeexec.c,-lc,$@)

# ------------------------------------------------------------------
# mksh on mlibc (native ABI)
#
# mksh is the reference POSIX shell; on the native ABI its fork/execve go
# through the capability-safe task_clone continuation + in-place execve,
# and its job wait goes through the checkpoint SIGCHLD model.  MKSH_UNEMPLOYED
# disables job control, so the shell needs no setpgid/tcsetpgrp.  mksh's own
# strlcpy.c is dropped (mlibc provides strlcpy).
# ------------------------------------------------------------------
MLIBC_MKSH_DIR     := user/external/mksh-cvs2git
MLIBC_MKSH_OBJDIR  := $(NATIVE_BUILD_DIR)/mlibc-mksh-obj
MLIBC_MKSH_BIN     := $(NATIVE_BUILD_DIR)/mlibc-mksh-$(NATIVE_TAG)
MLIBC_MKSH_SRCS    := edit eval exec expr funcs histrap jobs lalloc lex main misc shf syn tree ulimit var
MLIBC_MKSH_OBJS    := $(addprefix $(MLIBC_MKSH_OBJDIR)/,$(addsuffix .o,$(MLIBC_MKSH_SRCS)))
MLIBC_MKSH_CFLAGS  := -O2 -ffreestanding -std=gnu99 -D_GNU_SOURCE \
	-DMKSH_USE_AUTOCONF_H -DMKSH_DISABLE_DEPRECATED -DMKSH_UNEMPLOYED \
	-DMKSH_DONT_EMIT_IDSTRING -DMKSH_BUILDSH -DMKSH_BUILD_R=599 \
	$(MLIBC_FAST_TYPE_FLAGS) -I$(MLIBC_MKSH_DIR) -isystem $(MLIBC_SYSROOT)/include

$(MLIBC_MKSH_OBJDIR)/%.o: $(MLIBC_MKSH_DIR)/%.c | $(MLIBC_SYSROOT)/lib/libc.a
	@mkdir -p $(dir $@)
	$(RISCV_ELF_PREFIX)gcc $(MLIBC_MKSH_CFLAGS) -c $< -o $@

$(MLIBC_MKSH_BIN): $(MLIBC_MKSH_OBJS) $(MLIBC_SYSROOT)/lib/libc.a user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,$(MLIBC_MKSH_OBJS),-lc,$@)

mlibc-hello-rv: $(MLIBC_HELLO_BIN) $(MLIBC_CHILD_BIN) $(MLIBC_FORK_BIN) $(MLIBC_SIGCHLD_BIN) $(MLIBC_MKSH_BIN)

# Declared .PHONY; the shell was previously only reachable as a prerequisite of
# mlibc-hello-rv, which left `make mlibc-mksh` with no rule.
mlibc-mksh: $(MLIBC_MKSH_BIN)

# ------------------------------------------------------------------
# sbase coreutils on mlibc (native ABI)
#
# Ports a curated set of low-risk, deterministic sbase(1) tools from
# musl/Linux-ABI to the native ABI by compiling their single-file sources
# (plus the sbase libutil/libutf helpers) against the mlibc sysroot.
# They are launched unchanged from the mksh (Linux ABI) shell: the kernel
# detects the PT_A20_START_INFO phdr and switches the task to native mode.
# ------------------------------------------------------------------
MLIBC_SBASE_DIR       := user/external/sbase
MLIBC_SBASE_OBJDIR    := $(NATIVE_BUILD_DIR)/mlibc-sbase
MLIBC_SBASE_TOOLS     := true false sync echo uname seq yes cat mkdir rmdir ln sleep \
	basename dirname hostname head tail rm cp mv wc
MLIBC_SBASE_BINS      := $(addprefix $(NATIVE_BUILD_DIR)/mlibc-,$(MLIBC_SBASE_TOOLS))
MLIBC_SBASE_CFLAGS    := -O2 -ffreestanding -std=c99 -D_GNU_SOURCE -D_XOPEN_SOURCE=700 \
	-D_FILE_OFFSET_BITS=64 $(MLIBC_FAST_TYPE_FLAGS) \
	-isystem $(MLIBC_SYSROOT)/include -I$(MLIBC_SBASE_DIR)

# libutf helper objects (kept in sync with sbase Makefile LIBUTFOBJ)
MLIBC_SBASE_LIBUTF_SRC := \
	libutf/fgetrune.c libutf/fputrune.c libutf/isalnumrune.c libutf/isalpharune.c \
	libutf/isblankrune.c libutf/iscntrlrune.c libutf/isdigitrune.c libutf/isgraphrune.c \
	libutf/isprintrune.c libutf/ispunctrune.c libutf/isspacerune.c libutf/istitlerune.c \
	libutf/isxdigitrune.c libutf/lowerrune.c libutf/rune.c libutf/runetype.c \
	libutf/upperrune.c libutf/utf.c libutf/utftorunestr.c
MLIBC_SBASE_LIBUTIL_SRC := $(wildcard $(MLIBC_SBASE_DIR)/libutil/*.c)
MLIBC_SBASE_LIBUTF_OBJS := $(addprefix $(MLIBC_SBASE_OBJDIR)/,$(MLIBC_SBASE_LIBUTF_SRC:.c=.o))
MLIBC_SBASE_LIBUTIL_OBJS := $(patsubst $(MLIBC_SBASE_DIR)/%.c,$(MLIBC_SBASE_OBJDIR)/%.o,$(MLIBC_SBASE_LIBUTIL_SRC))
MLIBC_SBASE_LIBUTF_A   := $(MLIBC_SBASE_OBJDIR)/libutf.a
MLIBC_SBASE_LIBUTIL_A  := $(MLIBC_SBASE_OBJDIR)/libutil.a

# Generic object rule covers libutil/, libutf/ and the tool sources.
$(MLIBC_SBASE_OBJDIR)/%.o: $(MLIBC_SBASE_DIR)/%.c | $(MLIBC_SYSROOT)/lib/libc.a
	@mkdir -p $(dir $@)
	$(RISCV_ELF_PREFIX)gcc $(MLIBC_SBASE_CFLAGS) -c $< -o $@

$(MLIBC_SBASE_LIBUTF_A): $(MLIBC_SBASE_LIBUTF_OBJS)
	$(RISCV_ELF_PREFIX)ar rcs $@ $^

$(MLIBC_SBASE_LIBUTIL_A): $(MLIBC_SBASE_LIBUTIL_OBJS)
	$(RISCV_ELF_PREFIX)ar rcs $@ $^

# Pattern rule links every mlibc-<tool> against the sbase helpers and mlibc.
# (mlibc-hello-*/mlibc-child-* have explicit rules above and win over this
# pattern because explicit rules take precedence.)
$(NATIVE_BUILD_DIR)/mlibc-%: $(MLIBC_SBASE_OBJDIR)/%.o $(MLIBC_SBASE_LIBUTIL_A) \
	$(MLIBC_SBASE_LIBUTF_A) $(MLIBC_SYSROOT)/lib/libc.a user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,$(MLIBC_SBASE_OBJDIR)/$*.o,$(MLIBC_SBASE_LIBUTIL_A) $(MLIBC_SBASE_LIBUTF_A) -lc,$@)

mlibc-sbase: $(MLIBC_SBASE_BINS)

# Keep the per-tool and helper objects: they are produced by pattern rules
# and would otherwise be treated as intermediate files and deleted.
.PRECIOUS: $(MLIBC_SBASE_LIBUTIL_OBJS) $(MLIBC_SBASE_LIBUTF_OBJS) \
	$(addprefix $(MLIBC_SBASE_OBJDIR)/,$(addsuffix .o,$(MLIBC_SBASE_TOOLS)))

# Copy the sbase-on-mlibc tools into the FAT32 rootfs.  Used by the smoke
# target below and useful on its own after a dev-build.
mlibc-sbase-rootfs: mlibc-sbase
	@$(PYTHON) tools/img.py sbase-rootfs \
		--fat32-img "$(FAT32_IMG)" \
		--native-build-dir "$(NATIVE_BUILD_DIR)" \
		--tools "$(MLIBC_SBASE_TOOLS)"

smoke-mlibc-sbase:
	$(PYTHON) tools/smoke.py smoke-mlibc-sbase

smoke-mlibc:
	$(PYTHON) tools/smoke.py smoke-mlibc

smoke-mlibc-fork:
	$(PYTHON) tools/smoke.py smoke-mlibc-fork

smoke-mlibc-mksh:
	$(PYTHON) tools/smoke.py smoke-mlibc-mksh

smoke-native-libc:
	$(PYTHON) tools/smoke.py smoke-native-libc


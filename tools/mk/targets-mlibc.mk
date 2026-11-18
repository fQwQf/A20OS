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

MLIBC_ENV := $(if $(filter llvm,$(TOOLCHAIN)),env CC="$(CLANG)" CXX="$(CLANG) --driver-mode=g++",)
MLIBC_SOURCE_DIR := $(MLIBC_DIR)
MLIBC_CROSS_FILE := $(MLIBC_DIR)/ci/a20-riscv64.cross-file
MLIBC_LIBRT := -lgcc
MLIBC_CC := $(RISCV_ELF_PREFIX)gcc
ifeq ($(TOOLCHAIN),llvm)
MLIBC_BUILD := build/toolchains/mlibc-static
MLIBC_SOURCE_DIR := build/toolchains/mlibc-source
MLIBC_SOURCE_STAMP := $(MLIBC_SOURCE_DIR)/.prepared
MLIBC_SYSROOT := user/build/mlibc-sysroot-llvm
MLIBC_CROSS_FILE := build/toolchains/mlibc-llvm.cross-file
MLIBC_LIBRT := $(LLVM_RT_ROOT)/riscv64-user/builtins.a
MLIBC_CC := $(CLANG) --target=riscv64-linux-gnu $(LLVM_LINK)
RISCV_GNU_CC := $(MLIBC_CC)
$(MLIBC_CROSS_FILE): tools/toolchain/mlibc-cross.py
	$(PYTHON) tools/toolchain/build-builtins.py --arch riscv64 --output $(MLIBC_LIBRT)
	$(PYTHON) tools/toolchain/mlibc-cross.py $@
endif

# Let Ninja see generic libc/header changes too; depending only on the port
# sources could silently keep an old installed archive after a common fix.
MLIBC_SOURCE_DEPS := $(shell find $(MLIBC_DIR)/options $(MLIBC_DIR)/sysdeps/a20 $(MLIBC_DIR)/subprojects -type f 2>/dev/null) \
                     $(wildcard $(MLIBC_DIR)/meson*.build $(MLIBC_DIR)/meson*.txt $(MLIBC_DIR)/meson.options)
MLIBC_NATIVE_HEADERS := $(wildcard user/liba20rt/*.h kernel/include/uapi/a20/*.h)

ifeq ($(TOOLCHAIN),llvm)
$(MLIBC_SOURCE_STAMP): $(MLIBC_SOURCE_DEPS) tools/toolchain/prepare-mlibc.py
	$(PYTHON) tools/toolchain/prepare-mlibc.py $(MLIBC_SOURCE_DIR)
endif

$(MLIBC_SYSROOT)/lib/libc.a: $(MLIBC_SOURCE_STAMP) $(MLIBC_SOURCE_DEPS) \
	$(MLIBC_CROSS_FILE)
	@test -d "$(MLIBC_BUILD)" || $(MLIBC_ENV) meson setup $(MLIBC_BUILD) $(MLIBC_SOURCE_DIR) \
		--cross-file $(MLIBC_CROSS_FILE) \
		$(if $(filter llvm,$(TOOLCHAIN)),-Dlibgcc_dependency=false,) -Ddefault_library=static -Dbuild_tests=false --prefix=$(abspath $(MLIBC_SYSROOT))
	$(MLIBC_ENV) ninja -C $(MLIBC_BUILD)
	$(MLIBC_ENV) meson install -C $(MLIBC_BUILD) --no-rebuild --quiet

mlibc-sysroot: $(MLIBC_SYSROOT)/lib/libc.a

# ------------------------------------------------------------------
# Dynamic mlibc artifacts (ld.so + libc.so) for the real rtld gate.
#
# The shared link needs a toolchain whose ld supports -shared; the bare-metal
# riscv64-unknown-elf ld does not.  The linux-gnu driver is used only to
# link: the C library itself is still the A20 sysdeps build, and every link
# runs with -nostdlib so no host libc is involved.
# ------------------------------------------------------------------
MLIBC_SHARED_BUILD    := $(MLIBC_DIR)/build-a20-shared$(if $(filter llvm,$(TOOLCHAIN)),-llvm,)
MLIBC_SHARED_SYSROOT  := user/build/mlibc-shared-sysroot$(if $(filter llvm,$(TOOLCHAIN)),-llvm,)
MLIBC_SHARED_CROSS    := $(if $(filter llvm,$(TOOLCHAIN)),$(MLIBC_CROSS_FILE),user/mlibc/a20-shared.cross-file)
MLIBC_SHARED_DEPS     := $(MLIBC_SOURCE_DEPS) \
                         $(wildcard $(MLIBC_DIR)/options/rtld/riscv64/*) \
                         $(MLIBC_DIR)/meson.build $(MLIBC_SHARED_CROSS)
MLIBC_DYNAMIC_CFLAGS  := -march=rv64gc -mabi=lp64d -mcmodel=medany -O2 \
                         -D_GNU_SOURCE $(MLIBC_FAST_TYPE_FLAGS) \
                         -isystem $(MLIBC_SHARED_SYSROOT)/include
MLIBC_DYNAMIC_APP     := $(NATIVE_BUILD_DIR)/mlibc-dynamic-$(NATIVE_TAG)
MLIBC_DYNAMIC_PLUGIN  := $(NATIVE_BUILD_DIR)/libmlibcplug.so

$(MLIBC_SHARED_SYSROOT)/lib/libc.so: $(MLIBC_SOURCE_STAMP) $(MLIBC_SHARED_DEPS)
	@test -d "$(MLIBC_SHARED_BUILD)" || $(MLIBC_ENV) meson setup $(MLIBC_SHARED_BUILD) $(MLIBC_SOURCE_DIR) \
		--cross-file $(MLIBC_CROSS_FILE) \
		--cross-file $(MLIBC_SHARED_CROSS) \
		$(if $(filter llvm,$(TOOLCHAIN)),-Dlibgcc_dependency=false,) -Ddefault_library=both -Dbuild_tests=false \
		--prefix=$(abspath $(MLIBC_SHARED_SYSROOT))
	$(MLIBC_ENV) ninja -C $(MLIBC_SHARED_BUILD)
	$(MLIBC_ENV) meson install -C $(MLIBC_SHARED_BUILD) --no-rebuild --quiet

mlibc-shared-sysroot: $(MLIBC_SHARED_SYSROOT)/lib/libc.so

$(MLIBC_DYNAMIC_PLUGIN): user/tests/test_mlibc_dynamic_plugin.c \
		$(MLIBC_SHARED_SYSROOT)/lib/libc.so
	@mkdir -p $(dir $@)
	$(RISCV_GNU_CC) $(MLIBC_DYNAMIC_CFLAGS) -fPIC -shared -nostdlib \
		-Wl,-soname,libmlibcplug.so $< -o $@

$(MLIBC_DYNAMIC_APP): user/tests/test_mlibc_dynamic.c user/mlibc/a20-mlibc-dyn.ld \
		$(MLIBC_SHARED_SYSROOT)/lib/libc.so $(MLIBC_SHARED_SYSROOT)/lib/crt1.o
	@mkdir -p $(dir $@)
	$(RISCV_GNU_CC) $(MLIBC_DYNAMIC_CFLAGS) -fPIE -c $< -o $@.o
	$(RISCV_GNU_CC) -march=rv64gc -mabi=lp64d -mcmodel=medany -nostdlib -pie \
		-Wl,--allow-shlib-undefined -Wl,-dynamic-linker,/lib/ld.so \
		-Wl,-rpath,'$$ORIGIN/lib' -T user/mlibc/a20-mlibc-dyn.ld \
		$(MLIBC_SHARED_SYSROOT)/lib/crt1.o $@.o \
		-L$(MLIBC_SHARED_SYSROOT)/lib -lc $(MLIBC_LIBRT) -o $@
	@rm -f $@.o

.PHONY: mlibc-dynamic mlibc-dynamic-rootfs
mlibc-dynamic: $(MLIBC_DYNAMIC_APP) $(MLIBC_DYNAMIC_PLUGIN)

mlibc-dynamic-rootfs: mlibc-dynamic
	@$(PYTHON) tools/img.py mlibc-dynamic-rootfs \
		--fat32-img "$(FAT32_IMG)" --native-build-dir "$(NATIVE_BUILD_DIR)" \
		--mlibc-sysroot "$(abspath $(MLIBC_SHARED_SYSROOT))" --tag "$(NATIVE_TAG)"

# Optional full-libc concurrency workload; not part of ordinary rootfs builds.
MLIBC_THREADS_BIN := $(NATIVE_BUILD_DIR)/mlibc-threads-$(NATIVE_TAG)
.PHONY: mlibc-threads
ifeq ($(ARCH),riscv64)
$(MLIBC_THREADS_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_threads.c user/mlibc/a20-mlibc.ld $(wildcard user/liba20rt/*.h) kernel/include/uapi/a20/execution.h kernel/include/uapi/a20/execution_domain.h
	$(call MLIBC_LINK_RECIPE,-O2 -Iuser user/tests/test_mlibc_threads.c,-lc -lpthread -lm -lrt,$@)

mlibc-threads: $(MLIBC_THREADS_BIN)
else
mlibc-threads:
	@echo "mlibc-threads requires the existing riscv64 full-libc port" >&2
	@exit 1
endif

# Full-libc thread signal coordination workload consumed by
# tools/mlibc/check-mlibc-signals.py (Signal module C/R pairing evidence).
MLIBC_SIGNALS_BIN := $(NATIVE_BUILD_DIR)/mlibc-signals-$(NATIVE_TAG)
.PHONY: mlibc-signals
ifeq ($(ARCH),riscv64)
$(MLIBC_SIGNALS_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_signals.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,-O2 user/tests/test_mlibc_signals.c,-lc -lpthread -lm -lrt,$@)

mlibc-signals: $(MLIBC_SIGNALS_BIN)
else
mlibc-signals:
	@echo "mlibc-signals requires the existing riscv64 full-libc port" >&2
	@exit 1
endif

# Bare-metal GCC does not forward -static to ld. Select archives explicitly
# when a sysroot built with --library both also contains shared libraries.
define MLIBC_LINK_RECIPE
@mkdir -p $(dir $(3))
$(MLIBC_CC) -march=rv64gc -mabi=lp64d -mcmodel=medany -static -nostdlib \
	-Wl,-Bstatic \
	-D_GNU_SOURCE $(MLIBC_FAST_TYPE_FLAGS) \
	-isystem $(MLIBC_SYSROOT)/include \
	-T user/mlibc/a20-mlibc.ld \
	-Wl,-s \
	$(MLIBC_SYSROOT)/lib/crt1.o $(MLIBC_SYSROOT)/lib/a20_thread_entry.o \
	$(1) \
	-L$(MLIBC_SYSROOT)/lib $(2) $(MLIBC_LIBRT) \
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

MLIBC_SEMANTICS_BIN := $(NATIVE_BUILD_DIR)/mlibc-semantics-$(NATIVE_TAG)
$(MLIBC_SEMANTICS_BIN): $(MLIBC_NATIVE_HEADERS) $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_semantics.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_semantics.c,-lc -lpthread,$@)

.PHONY: mlibc-semantics
mlibc-semantics: $(MLIBC_SEMANTICS_BIN)

MLIBC_NETWORK_BIN := $(NATIVE_BUILD_DIR)/mlibc-network-$(NATIVE_TAG)
$(MLIBC_NETWORK_BIN): $(MLIBC_SYSROOT)/lib/libc.a user/tests/test_mlibc_network.c user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,user/tests/test_mlibc_network.c,-lc -lpthread,$@)
.PHONY: mlibc-network
mlibc-network: $(MLIBC_NETWORK_BIN)

# Compile unmodified upstream tests as Native ELFs. These are run in the
# guest, so a Linux sysdep or host-libc pass cannot substitute for acceptance.
MLIBC_UPSTREAM_CASES := ansi/alloc ansi/sscanf ansi/snprintf ansi/sprintf \
	ansi/strtol ansi/strtof ansi/longjmp ansi/qsort ansi/calloc ansi/strcmp \
	ansi/strchr ansi/strrchr ansi/ungetc ansi/fopen ansi/fputs ansi/ftell \
	ansi/fseek-eof ansi/fgetpos ansi/threads-mtx ansi/threads-cnd \
	ansi/threads-once ansi/threads-tss ansi/threads-thrd
MLIBC_UPSTREAM_BINS := $(addprefix $(NATIVE_BUILD_DIR)/mlibc-upstream-,$(subst /,-,$(MLIBC_UPSTREAM_CASES)))
define MLIBC_UPSTREAM_template
$(NATIVE_BUILD_DIR)/mlibc-upstream-$(subst /,-,$(1)): $(MLIBC_SYSROOT)/lib/libc.a $(MLIBC_DIR)/tests/$(1).c user/mlibc/a20-mlibc.ld
	$$(call MLIBC_LINK_RECIPE,$(MLIBC_DIR)/tests/$(1).c,-lc -lpthread -lm -lrt,$$@)
endef
$(foreach test_case,$(MLIBC_UPSTREAM_CASES),$(eval $(call MLIBC_UPSTREAM_template,$(test_case))))
.PHONY: mlibc-upstream-tests
mlibc-upstream-tests: $(MLIBC_UPSTREAM_BINS)

# ------------------------------------------------------------------
# mksh on mlibc (native ABI)
#
# mksh is the reference POSIX shell; on the native ABI its fork/execve go
# through the capability-safe task_clone continuation + in-place execve,
# and its job wait goes through the checkpoint SIGCHLD model.  MKSH_UNEMPLOYED
# disables job control, so the shell needs no setpgid/tcsetpgrp.  mksh's own
# strlcpy.c is dropped (mlibc provides strlcpy).
# ------------------------------------------------------------------
MLIBC_MKSH_DIR     := build/cache/test-sources/mksh
# Optional upstream application fixtures are fetched only when these ABI tests
# are selected; production builds do not initialize shell/coreutils submodules.
$(MLIBC_MKSH_DIR)/.a20-fixture-ready: tools/packaging/fetch_test_source.py
	$(PYTHON) tools/packaging/fetch_test_source.py mksh --output $(MLIBC_MKSH_DIR)
$(MLIBC_MKSH_DIR)/%.c: $(MLIBC_MKSH_DIR)/.a20-fixture-ready
	@test -f "$@"

MLIBC_MKSH_OBJDIR  := $(NATIVE_BUILD_DIR)/mlibc-mksh-obj
MLIBC_MKSH_BIN     := $(NATIVE_BUILD_DIR)/mlibc-mksh-$(NATIVE_TAG)
MLIBC_MKSH_SRCS    := edit eval exec expr funcs histrap jobs lalloc lex main misc shf syn tree ulimit var
MLIBC_MKSH_OBJS    := $(addprefix $(MLIBC_MKSH_OBJDIR)/,$(addsuffix .o,$(MLIBC_MKSH_SRCS)))
MLIBC_MKSH_CFLAGS  := -O2 -ffreestanding -std=gnu99 -D_GNU_SOURCE \
	-DMKSH_USE_AUTOCONF_H -DMKSH_DISABLE_DEPRECATED -DMKSH_UNEMPLOYED \
	-DMKSH_DONT_EMIT_IDSTRING -DMKSH_BUILDSH -DMKSH_BUILD_R=599 \
	$(MLIBC_FAST_TYPE_FLAGS) -Itests/tools/test-fixtures/mksh -I$(MLIBC_MKSH_DIR) -isystem $(MLIBC_SYSROOT)/include

MLIBC_MKSH_PORT_HEADERS := $(wildcard tests/tools/test-fixtures/mksh/*.h tests/tools/test-fixtures/mksh/*.gen tests/tools/test-fixtures/mksh/*.inc)
$(MLIBC_MKSH_OBJDIR)/%.o: $(MLIBC_MKSH_DIR)/%.c $(MLIBC_MKSH_PORT_HEADERS) | $(MLIBC_SYSROOT)/lib/libc.a
	@mkdir -p $(dir $@)
	$(MLIBC_CC) $(MLIBC_MKSH_CFLAGS) -c $< -o $@

$(MLIBC_MKSH_BIN): $(MLIBC_MKSH_OBJS) $(MLIBC_SYSROOT)/lib/libc.a user/mlibc/a20-mlibc.ld
	$(call MLIBC_LINK_RECIPE,$(MLIBC_MKSH_OBJS),-lc,$@)

mlibc-hello-rv: $(MLIBC_HELLO_BIN) $(MLIBC_CHILD_BIN) $(MLIBC_FORK_BIN) $(MLIBC_SIGCHLD_BIN) $(MLIBC_PIPEEXEC_BIN) $(MLIBC_MKSH_BIN)

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
MLIBC_SBASE_DIR       := build/cache/test-sources/sbase
$(MLIBC_SBASE_DIR)/.a20-fixture-ready: tools/packaging/fetch_test_source.py
	$(PYTHON) tools/packaging/fetch_test_source.py sbase --output $(MLIBC_SBASE_DIR)
$(MLIBC_SBASE_DIR)/%.c: $(MLIBC_SBASE_DIR)/.a20-fixture-ready
	@test -f "$@"

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
MLIBC_SBASE_LIBUTIL_SRC := $(addprefix $(MLIBC_SBASE_DIR)/,libutil/concat.c libutil/confirm.c libutil/cp.c libutil/crypt.c libutil/ealloc.c libutil/enmasse.c libutil/eprintf.c libutil/eregcomp.c libutil/estrtod.c libutil/fnck.c libutil/fshut.c libutil/getlines.c libutil/human.c libutil/linecmp.c libutil/md5.c libutil/memmem.c libutil/mkdirp.c libutil/mode.c libutil/parseoffset.c libutil/putword.c libutil/reallocarray.c libutil/recurse.c libutil/rm.c libutil/sha1.c libutil/sha224.c libutil/sha256.c libutil/sha384.c libutil/sha512-224.c libutil/sha512-256.c libutil/sha512.c libutil/strcasestr.c libutil/strlcat.c libutil/strlcpy.c libutil/strnsubst.c libutil/strsep.c libutil/strtonum.c libutil/unescape.c libutil/writeall.c)
MLIBC_SBASE_LIBUTF_OBJS := $(addprefix $(MLIBC_SBASE_OBJDIR)/,$(MLIBC_SBASE_LIBUTF_SRC:.c=.o))
MLIBC_SBASE_LIBUTIL_OBJS := $(patsubst $(MLIBC_SBASE_DIR)/%.c,$(MLIBC_SBASE_OBJDIR)/%.o,$(MLIBC_SBASE_LIBUTIL_SRC))
MLIBC_SBASE_LIBUTF_A   := $(MLIBC_SBASE_OBJDIR)/libutf.a
MLIBC_SBASE_LIBUTIL_A  := $(MLIBC_SBASE_OBJDIR)/libutil.a

# Generic object rule covers libutil/, libutf/ and the tool sources.
$(MLIBC_SBASE_OBJDIR)/%.o: $(MLIBC_SBASE_DIR)/%.c | $(MLIBC_SYSROOT)/lib/libc.a
	@mkdir -p $(dir $@)
	$(MLIBC_CC) $(MLIBC_SBASE_CFLAGS) -c $< -o $@

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

# Build the mlibc payload and inject it into the dev FAT32 image.  The image
# is composed during dev-build -- long before mlibc exists -- so the mlibc
# smoke cases inject afterwards (see img.py mlibc-rootfs).
.PHONY: mlibc-rootfs
mlibc-rootfs: mlibc-hello-rv mlibc-mksh
	@$(PYTHON) tools/img.py mlibc-rootfs --fat32-img "$(FAT32_IMG)" \
		--native-build-dir "$(NATIVE_BUILD_DIR)" --tag "$(NATIVE_TAG)"


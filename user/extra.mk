# extra.mk — Build selected packages for the A20OS "extra" disk
#
# Invoked from the top-level Makefile as:
#   $(MAKE) -f user/extra.mk ARCH=riscv64
#
# Output goes to user/build/extra/<arch>/ and its obj directory.

ARCH ?= riscv64
OPT ?= -O2
PACKAGES ?= vim git gcc rust
EXTRA_MAKEFILE := $(lastword $(MAKEFILE_LIST))

.DEFAULT_GOAL := all
SUPPORTED_ARCHES := riscv64 loongarch64 aarch64
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH '$(ARCH)'; expected one of: $(SUPPORTED_ARCHES))
endif

# ----------------------------------------------------------------
# Paths — resolve everything to absolute paths
# ----------------------------------------------------------------
USER_DIR     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MUSL_BASE    := $(USER_DIR)/external/musl
MUSL_BUILD   := $(MUSL_BASE)/build-$(ARCH)
MUSL_CRT     := $(MUSL_BUILD)/lib
MUSL_INC     := -isystem $(MUSL_BUILD)/obj/include \
                -isystem $(MUSL_BASE)/include \
                -isystem $(MUSL_BASE)/arch/$(ARCH) \
                -isystem $(MUSL_BASE)/arch/generic

BUILD_ROOT   := $(USER_DIR)/build/extra
BUILD_DIR    := $(BUILD_ROOT)/$(ARCH)
STAMP_DIR    := $(BUILD_DIR)/stamp
OBJ_DIR      := $(BUILD_DIR)/obj
EXTRA_DIR    := $(USER_DIR)/extra
VIM_SRC      := $(USER_DIR)/external/vim/src
VIM_BUILD    := $(OBJ_DIR)/vim
VIM_WORK_SRC := $(VIM_BUILD)/src
GIT_SRC      := $(USER_DIR)/external/git
GIT_BUILD    := $(OBJ_DIR)/git
GIT_WORK_SRC := $(GIT_BUILD)/src
TERMCAP_LIB  := $(OBJ_DIR)/libtermcap.a
ZLIB_SRC     := $(USER_DIR)/external/zlib
ZLIB_BUILD   := $(OBJ_DIR)/zlib
ZLIB_LIB     := $(ZLIB_BUILD)/libz.a
ZLIB_CFLAGS  = $(CFLAGS) -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN -include errno.h
BINUTILS_SRC := $(USER_DIR)/external/binutils
GCC_SRC      := $(USER_DIR)/external/gcc
MCM_OUTPUT   := $(USER_DIR)/external/musl-cross-make/output
RUST_INSTALL := $(OBJ_DIR)/rust
RUST_STAMP   := $(STAMP_DIR)/.rust-built
RUST_SYSROOT := $(RUST_INSTALL)/a20-sysroot

# ----------------------------------------------------------------
# Toolchain (mirrors user/Makefile)
# ----------------------------------------------------------------
RISCV_ELF_PREFIX ?= $(if $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null),riscv64-unknown-elf-,$(if $(shell command -v riscv64-elf-gcc 2>/dev/null),riscv64-elf-,riscv64-unknown-elf-))
CROSS_COMPILE_riscv64      := $(RISCV_ELF_PREFIX)
CROSS_COMPILE_loongarch64  := loongarch64-linux-gnu-
CROSS_COMPILE_aarch64      := aarch64-linux-gnu-
CROSS_COMPILE ?= $(CROSS_COMPILE_$(ARCH))

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
AR      := $(CROSS_COMPILE)ar
RANLIB  := $(CROSS_COMPILE)ranlib

ARCH_CFLAGS_riscv64     := -mabi=lp64d -march=rv64g -mcmodel=medany
ARCH_CFLAGS_loongarch64 := -mabi=lp64d -march=loongarch64 -mcmodel=normal -fno-pic
ARCH_CFLAGS_aarch64     := -march=armv8-a -fno-pic -fno-tree-vectorize
ARCH_CFLAGS := $(ARCH_CFLAGS_$(ARCH))

ARCH_LDFLAGS_loongarch64 := -no-pie
ARCH_LDFLAGS_aarch64     := -no-pie
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH))

CFLAGS  := -Wall -Wextra $(OPT) -ffreestanding -nostdinc $(MUSL_INC) \
           -static -D_GNU_SOURCE $(ARCH_CFLAGS)
LDFLAGS := -static -nostdlib $(ARCH_LDFLAGS)

LIBGCC := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)

CRT_START := $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o
CRT_END   := $(MUSL_CRT)/crtn.o
LIBC      := $(MUSL_CRT)/libc.a $(LIBGCC)

# ----------------------------------------------------------------
# musl dependency check
# ----------------------------------------------------------------
MUSL_CHECK_FILES := $(MUSL_CRT)/crt1.o \
                    $(MUSL_CRT)/crti.o \
                    $(MUSL_CRT)/crtn.o \
                    $(MUSL_CRT)/libc.a

musl_check:
	@missing=0; \
	for f in $(MUSL_CHECK_FILES); do \
		[ -f "$$f" ] || { echo "[EXTRA] missing $$f"; missing=1; }; \
	done; \
	if [ "$$missing" -eq 1 ]; then \
		echo "[EXTRA] Run 'make -C user' first to build musl for $(ARCH)"; \
		exit 1; \
	fi

$(TERMCAP_LIB): $(EXTRA_DIR)/termcap_stub.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(dir $@)termcap_stub.o
	$(AR) rcs $@ $(dir $@)termcap_stub.o

$(ZLIB_LIB): $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
	@mkdir -p $(ZLIB_BUILD)
	@echo "[EXTRA] Building zlib for $(ARCH)..."
	cd $(ZLIB_BUILD) && \
	  CHOST=$(CROSS_COMPILE:%-=%) \
	  CC=$(CC) AR=$(AR) RANLIB=$(RANLIB) \
	  CFLAGS="$(ZLIB_CFLAGS)" \
	  $(ZLIB_SRC)/configure --static
	# Cross-link probes cannot use our bare-metal compiler command directly;
	# override their NO_STRERROR/NO_vsnprintf result for the real musl build.
	$(MAKE) -C $(ZLIB_BUILD) libz.a CFLAGS="$(ZLIB_CFLAGS)"
	mkdir -p $(ZLIB_BUILD)/include
	cp $(ZLIB_SRC)/zlib.h $(ZLIB_BUILD)/zconf.h $(ZLIB_BUILD)/include/
	@echo "[EXTRA] zlib -> $@"

# Source-availability guards: skip building a package if its upstream source tree is missing.
VIM_AVAILABLE := $(if $(wildcard $(VIM_SRC)/main.c),1)
GIT_AVAILABLE := $(if $(wildcard $(GIT_SRC)/Makefile),1)
# build-gcc.sh currently creates a RISC-V Canadian cross and needs all three
# source/toolchain inputs.  Keep it disabled for other architectures.
GCC_AVAILABLE := $(if $(and $(filter riscv64,$(ARCH)), \
                            $(wildcard $(BINUTILS_SRC)/configure), \
                            $(wildcard $(GCC_SRC)/configure), \
                            $(wildcard $(MCM_OUTPUT)/bin/riscv64-linux-musl-gcc)),1)

VALID_PACKAGES := vim git gcc cc rust rustc cargo rustfmt
UNKNOWN_PACKAGES := $(filter-out $(VALID_PACKAGES),$(PACKAGES))
ifneq ($(strip $(UNKNOWN_PACKAGES)),)
$(error Unknown extra package(s): $(UNKNOWN_PACKAGES); expected: $(VALID_PACKAGES))
endif

REQUESTED_TARGETS := $(sort \
  $(if $(filter vim,$(PACKAGES)),vim) \
  $(if $(filter git,$(PACKAGES)),git) \
  $(if $(filter gcc cc,$(PACKAGES)),gcc) \
  $(if $(and $(filter riscv64,$(ARCH)),$(filter rust rustc cargo rustfmt,$(PACKAGES))),rust))

# ================================================================
# vim
# ================================================================
VIM_BIN := $(BUILD_DIR)/vim

$(VIM_BIN): $(STAMP_DIR)/.vim-built
	@mkdir -p $(BUILD_DIR)
	cp $(VIM_WORK_SRC)/vim $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] vim -> $@"

$(STAMP_DIR)/.vim-built: $(TERMCAP_LIB) $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
ifeq ($(VIM_AVAILABLE),)
	@echo "[EXTRA] vim source not found in $(VIM_SRC); skipping vim"
	@mkdir -p $(STAMP_DIR)
	@touch $@
else
	@mkdir -p $(VIM_BUILD) $(STAMP_DIR)
	@echo "[EXTRA] Configuring vim for $(ARCH)..."
	@rm -rf $(VIM_WORK_SRC)
	@mkdir -p $(VIM_WORK_SRC)
	@cp -a $(VIM_SRC)/. $(VIM_WORK_SRC)/
	@$(MAKE) -C $(VIM_WORK_SRC) distclean >/dev/null 2>&1 || true
	cd $(VIM_WORK_SRC) && \
	  vim_cv_tgetent=ok \
	  ./configure \
	    --srcdir=$(VIM_WORK_SRC) \
	    --host=$(CROSS_COMPILE:%-=%) \
	    CC=$(CC) \
	    CFLAGS="$(CFLAGS) -I$(VIM_WORK_SRC) -I$(EXTRA_DIR)" \
	    LDFLAGS="$(LDFLAGS) -L$(dir $(TERMCAP_LIB))" \
	    --disable-gui \
	    --without-x \
	    --without-wayland \
	    --disable-nls \
	    --disable-netbeans \
	    --disable-channel \
	    --disable-terminal \
	    --disable-selinux \
	    --disable-canberra \
	    --disable-darwin \
	    --disable-xsmp \
	    --disable-xsmp-interact \
	    --with-tlib=termcap \
	    --with-features=normal \
	    --with-compiledby=A20OS \
	    --prefix=/usr \
	    vim_cv_toupper_broken=no \
	    vim_cv_terminfo=no \
	    vim_cv_tty_group=world \
	    vim_cv_tty_mode=0620 \
	    vim_cv_getoutstr=yes \
	    ; rc=$$?; \
	    tail -20 $(VIM_WORK_SRC)/auto/config.log 2>/dev/null; \
	    exit $$rc
	@echo "[EXTRA] Fixing cross-compile config for musl..."
	sed '/^#define rlim_t /d' $(VIM_WORK_SRC)/auto/config.h > $(VIM_WORK_SRC)/auto/config.h.tmp
	mv $(VIM_WORK_SRC)/auto/config.h.tmp $(VIM_WORK_SRC)/auto/config.h
	sed '/^#define stack_t /d' $(VIM_WORK_SRC)/auto/config.h > $(VIM_WORK_SRC)/auto/config.h.tmp
	mv $(VIM_WORK_SRC)/auto/config.h.tmp $(VIM_WORK_SRC)/auto/config.h
	for fn in FCHDIR FCHMOD FCHOWN FSYNC FTRUNCATE GETCWD GETPGID GETPWENT \
	          GETPWNAM GETPWUID GETRLIMIT GETTIMEOFDAY GETWD INET_NTOP \
	          LOCALTIME_R LSTAT MEMSET MKDTEMP NANOSLEEP PUTENV QSORT \
	          READLINK REMOVE SELECT SETENV SETPGID SETSID SHMAT SIGACTION \
	          SIGALTSTACK SIGPROCMASK SIGSET SIGSETJMP SIGSTACK STRCASECMP \
	          STRCOLL STRERROR STRFTIME STRNCASECMP STRPBRK STRPTIME STRTOL \
	          SYNC TOWLOWER TOWUPPER TZSET UNSETENV USLEEP UTIME UTIMES \
	          WAITPID; do \
	  sed "s,/\* #undef HAVE_$${fn} \*/,#define HAVE_$${fn} 1," \
	    $(VIM_WORK_SRC)/auto/config.h > $(VIM_WORK_SRC)/auto/config.h.tmp; \
	  mv $(VIM_WORK_SRC)/auto/config.h.tmp $(VIM_WORK_SRC)/auto/config.h; \
	done
	sed 's,/\* #undef HAVE_TERMCAP_H \*/,#define HAVE_TERMCAP_H 1,' \
	  $(VIM_WORK_SRC)/auto/config.h > $(VIM_WORK_SRC)/auto/config.h.tmp
	mv $(VIM_WORK_SRC)/auto/config.h.tmp $(VIM_WORK_SRC)/auto/config.h
	: > $(VIM_WORK_SRC)/auto/osdef.h
	@echo "[EXTRA] Building vim..."
	$(MAKE) -C $(VIM_WORK_SRC) \
	  VIMRCLOC=$(VIM_BUILD) \
	  VIMRUNTIMEDIR=/test/share/vim/vim92 \
	  DESTDIR=$(VIM_BUILD)/install \
	  srcdir=$(VIM_WORK_SRC) \
	  CC=$(CC) \
	  CFLAGS="$(CFLAGS) -I$(VIM_WORK_SRC) -I$(EXTRA_DIR)" \
	  LDFLAGS="$(LDFLAGS) -L$(dir $(TERMCAP_LIB)) $(CRT_START)" \
	  LIBS="-ltermcap $(LIBC) $(CRT_END)"
	@touch $@
endif

# ================================================================
# git
# ================================================================
GIT_BIN := $(BUILD_DIR)/git

$(GIT_BIN): $(STAMP_DIR)/.git-built
	@mkdir -p $(BUILD_DIR)
	cp $(GIT_WORK_SRC)/git $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] git -> $@"

$(STAMP_DIR)/.git-built: $(ZLIB_LIB) $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
ifeq ($(GIT_AVAILABLE),)
	@echo "[EXTRA] git source not found in $(GIT_SRC); skipping git"
	@mkdir -p $(STAMP_DIR)
	@touch $@
else
	@mkdir -p $(GIT_BUILD) $(STAMP_DIR)
	@echo "[EXTRA] Building git for $(ARCH)..."
	@rm -rf $(GIT_WORK_SRC)
	@mkdir -p $(GIT_WORK_SRC)
	@cp -a $(GIT_SRC)/. $(GIT_WORK_SRC)/
	@$(MAKE) -C $(GIT_WORK_SRC) clean >/dev/null 2>&1 || true
	$(MAKE) -C $(GIT_WORK_SRC) \
	  prefix=/usr \
	  template_dir=/test/share/git-core/templates \
	  CC=$(CC) \
	  AR=$(AR) \
	  CFLAGS="$(CFLAGS) -I$(ZLIB_BUILD)/include" \
	  LDFLAGS="$(LDFLAGS) $(CRT_START)" \
	  EXTLIBS="$(ZLIB_LIB) $(LIBC) $(CRT_END)" \
	  NO_OPENSSL=YesPlease \
	  NO_CURL=YesPlease \
	  NO_EXPAT=YesPlease \
	  NO_GETTEXT=YesPlease \
	  NO_TCLTK=YesPlease \
	  NO_ICONV=YesPlease \
	  NO_SVN_TESTS=YesPlease \
	  NO_REGEX=NeedsStartEnd \
	  NO_PTHREADS=YesPlease \
	  NO_UNIX_SOCKETS=YesPlease \
	  NO_SHA1_DC=YesPlease \
	  BLK_SHA1=YesPlease \
	  NO_PYTHON=YesPlease \
	  NO_PERL=YesPlease \
	  DEFAULT_PAGER=cat \
	  STATIC=YesPlease \
	  DESTDIR=$(GIT_BUILD)/install
	@touch $@
endif

# ================================================================
# gcc (binutils + GCC canadian-cross)
# ================================================================
GCC_BUILD_DIR := $(OBJ_DIR)/gcc-toolchain
GCC_INSTALL   := $(OBJ_DIR)/gcc-install
GCC_BIN       := $(BUILD_DIR)/gcc
CC_BIN        := $(BUILD_DIR)/cc

$(GCC_BIN): $(STAMP_DIR)/.gcc-built
	@mkdir -p $(BUILD_DIR)
	cp $(GCC_INSTALL)/bin/gcc $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] gcc -> $@"

$(CC_BIN): $(STAMP_DIR)/.gcc-built
	@mkdir -p $(BUILD_DIR)
	cp $(GCC_INSTALL)/bin/cc $@
	@echo "[EXTRA] cc -> $@"

$(STAMP_DIR)/.gcc-built: $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
ifeq ($(GCC_AVAILABLE),)
	@echo "[EXTRA] GCC source not found in $(GCC_SRC); skipping gcc/cc"
	@mkdir -p $(STAMP_DIR)
	@touch $@
else
	@mkdir -p $(GCC_BUILD_DIR) $(GCC_INSTALL) $(STAMP_DIR)
	@echo "[EXTRA] Building GCC toolchain for $(ARCH)..."
	$(EXTRA_DIR)/build-gcc.sh $(ARCH) $(MUSL_BUILD) $(GCC_BUILD_DIR) $(GCC_INSTALL)
endif

# ================================================================
# Rust (official RISC-V distribution)
# ================================================================
$(RUST_STAMP): $(EXTRA_DIR)/build-rust.sh $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
ifeq ($(ARCH),riscv64)
	@mkdir -p $(STAMP_DIR)
	@$(MAKE) --no-print-directory -f $(USER_DIR)/extra.mk ARCH=$(ARCH) musl_check
	$(EXTRA_DIR)/build-rust.sh $(RUST_INSTALL)
	@mkdir -p $(RUST_SYSROOT)/lib
	cp $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $(MUSL_CRT)/crtn.o $(MUSL_CRT)/libc.a $(RUST_SYSROOT)/lib/
	cp $(LIBGCC) $(RUST_SYSROOT)/lib/libgcc_s.a
	@printf 'GROUP ( libc.a )\n' > $(RUST_SYSROOT)/lib/libc.so
	@printf 'GROUP ( libgcc_s.a )\n' > $(RUST_SYSROOT)/lib/libgcc_s.so
	@touch $@
else
	@echo "[EXTRA] rust is currently supported only on riscv64"
	@false
endif

rust: $(RUST_STAMP)
rustc: rust
cargo: rust
rustfmt: rust

# ================================================================
# Top-level targets
# ================================================================
all: $(REQUESTED_TARGETS)

vim: $(if $(VIM_AVAILABLE),$(VIM_BIN))
git: $(if $(GIT_AVAILABLE),$(GIT_BIN))
gcc: $(if $(GCC_AVAILABLE),$(GCC_BIN) $(CC_BIN))

clean:
	rm -rf $(BUILD_ROOT)

.PHONY: all vim git gcc rust rustc cargo rustfmt clean musl_check

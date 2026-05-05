# extra.mk — Build vim / git / gcc as static binaries for A20OS "extra" disk
#
# Invoked from the top-level Makefile as:
#   $(MAKE) -f user/extra.mk ARCH=riscv64
#
# Output goes to user/build/extra/{vim,git,gcc,cc}

ARCH ?= riscv64
OPT ?= -O2

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

BUILD_DIR    := $(USER_DIR)/build/extra
STAMP_DIR    := $(USER_DIR)/build/extra/stamp/$(ARCH)
EXTRA_DIR    := $(USER_DIR)/extra
VIM_SRC      := $(USER_DIR)/external/vim/src
VIM_BUILD    := $(USER_DIR)/build/extra/obj/$(ARCH)/vim
GIT_SRC      := $(USER_DIR)/external/git
GIT_BUILD    := $(USER_DIR)/build/extra/obj/$(ARCH)/git
TERMCAP_LIB  := $(USER_DIR)/build/extra/obj/$(ARCH)/libtermcap.a
ZLIB_SRC     := $(USER_DIR)/external/zlib
ZLIB_BUILD   := $(USER_DIR)/build/extra/obj/$(ARCH)/zlib
ZLIB_LIB     := $(ZLIB_BUILD)/libz.a

# ----------------------------------------------------------------
# Toolchain (mirrors user/Makefile)
# ----------------------------------------------------------------
CROSS_COMPILE_riscv64      := riscv64-unknown-elf-
CROSS_COMPILE_loongarch64  := loongarch64-linux-gnu-
CROSS_COMPILE_aarch64      := aarch64-linux-gnu-
CROSS_COMPILE ?= $(CROSS_COMPILE_$(ARCH))

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
AR      := $(CROSS_COMPILE)ar
RANLIB  := $(CROSS_COMPILE)ranlib

ARCH_CFLAGS_riscv64     := -mabi=lp64 -march=rv64g -mcmodel=medany
ARCH_CFLAGS_loongarch64 := -mabi=lp64d -march=loongarch64 -mcmodel=normal -fno-pic
ARCH_CFLAGS_aarch64     := -march=armv8-a -fno-pic -fno-tree-vectorize
ARCH_CFLAGS := $(ARCH_CFLAGS_$(ARCH))

ARCH_LDFLAGS_loongarch64 := -no-pie
ARCH_LDFLAGS_aarch64     := -no-pie
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH))

CFLAGS  := -Wall -Wextra $(OPT) -ffreestanding -nostdinc $(MUSL_INC) \
           -static -D_GNU_SOURCE $(ARCH_CFLAGS)
LDFLAGS := -static -nostdlib $(ARCH_LDFLAGS)

LIBGCC_DEFAULT := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)
ifeq ($(ARCH),riscv64)
LIBGCC_LP64 := $(dir $(LIBGCC_DEFAULT))rv64i/lp64/libgcc.a
LIBGCC := $(if $(wildcard $(LIBGCC_LP64)),$(LIBGCC_LP64),$(LIBGCC_DEFAULT))
else
LIBGCC := $(LIBGCC_DEFAULT)
endif

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

$(ZLIB_LIB): musl_check
	@mkdir -p $(ZLIB_BUILD)
	@echo "[EXTRA] Building zlib for $(ARCH)..."
	$(MAKE) -C $(ZLIB_SRC) libz.a \
	  CC=$(CC) \
	  AR=$(AR) \
	  RANLIB=$(RANLIB) \
	  CFLAGS="$(CFLAGS) -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN"
	cp $(ZLIB_SRC)/libz.a $(ZLIB_LIB)
	mkdir -p $(ZLIB_BUILD)/include
	cp $(ZLIB_SRC)/zlib.h $(ZLIB_SRC)/zconf.h $(ZLIB_BUILD)/include/
	@echo "[EXTRA] zlib -> $@"

# ================================================================
# vim
# ================================================================
VIM_BIN := $(BUILD_DIR)/vim

$(VIM_BIN): $(STAMP_DIR)/.vim-built
	@mkdir -p $(BUILD_DIR)
	cp $(VIM_SRC)/vim $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] vim -> $@"

$(STAMP_DIR)/.vim-built: musl_check $(TERMCAP_LIB)
	@mkdir -p $(VIM_BUILD) $(STAMP_DIR)
	@echo "[EXTRA] Configuring vim for $(ARCH)..."
	cd $(VIM_SRC) && \
	  vim_cv_tgetent=ok \
	  ./configure \
	    --srcdir=$(VIM_SRC) \
	    --host=$(CROSS_COMPILE:%-=%) \
	    CC=$(CC) \
	    CFLAGS="$(CFLAGS) -I$(VIM_SRC) -I$(EXTRA_DIR)" \
	    LDFLAGS="$(LDFLAGS) -L$(dir $(TERMCAP_LIB))" \
	    --disable-gui \
	    --without-x \
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
	    --with-features=small \
	    --with-compiledby=A20OS \
	    --prefix=/usr \
	    vim_cv_toupper_broken=no \
	    vim_cv_terminfo=no \
	    vim_cv_tty_group=world \
	    vim_cv_tty_mode=0620 \
	    vim_cv_getoutstr=yes \
	    ; rc=$$?; \
	    tail -20 $(VIM_SRC)/auto/config.log 2>/dev/null; \
	    exit $$rc
	@echo "[EXTRA] Fixing cross-compile config for musl..."
	sed -i '/^#define rlim_t /d' $(VIM_SRC)/auto/config.h
	sed -i '/^#define stack_t /d' $(VIM_SRC)/auto/config.h
	for fn in FCHDIR FCHMOD FCHOWN FSYNC FTRUNCATE GETCWD GETPGID GETPWENT \
	          GETPWNAM GETPWUID GETRLIMIT GETTIMEOFDAY GETWD INET_NTOP \
	          LOCALTIME_R LSTAT MEMSET MKDTEMP NANOSLEEP PUTENV QSORT \
	          READLINK REMOVE SELECT SETENV SETPGID SETSID SHMAT SIGACTION \
	          SIGALTSTACK SIGPROCMASK SIGSET SIGSETJMP SIGSTACK STRCASECMP \
	          STRCOLL STRERROR STRFTIME STRNCASECMP STRPBRK STRPTIME STRTOL \
	          SYNC TOWLOWER TOWUPPER TZSET UNSETENV USLEEP UTIME UTIMES \
	          WAITPID; do \
	  sed -i "s,/\* #undef HAVE_$${fn} \*/,#define HAVE_$${fn} 1," \
	    $(VIM_SRC)/auto/config.h; \
	done
	sed -i 's,/\* #undef HAVE_TERMCAP_H \*/,#define HAVE_TERMCAP_H 1,' \
	  $(VIM_SRC)/auto/config.h
	: > $(VIM_SRC)/auto/osdef.h
	@echo "[EXTRA] Building vim..."
	$(MAKE) -C $(VIM_SRC) \
	  VIMRCLOC=$(VIM_BUILD) \
	  VIMRUNTIMEDIR=$(VIM_BUILD)/runtime \
	  DESTDIR=$(VIM_BUILD)/install \
	  srcdir=$(VIM_SRC) \
	  CC=$(CC) \
	  CFLAGS="$(CFLAGS) -I$(VIM_SRC) -I$(EXTRA_DIR)" \
	  LDFLAGS="$(LDFLAGS) -L$(dir $(TERMCAP_LIB)) $(CRT_START)" \
	  LIBS="-ltermcap $(LIBC) $(CRT_END)"
	@touch $@

# ================================================================
# git
# ================================================================
GIT_BIN := $(BUILD_DIR)/git

$(GIT_BIN): $(STAMP_DIR)/.git-built
	@mkdir -p $(BUILD_DIR)
	cp $(GIT_SRC)/git $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] git -> $@"

$(STAMP_DIR)/.git-built: musl_check $(ZLIB_LIB)
	@mkdir -p $(GIT_BUILD) $(STAMP_DIR)
	@echo "[EXTRA] Building git for $(ARCH)..."
	$(MAKE) -C $(GIT_SRC) \
	  prefix=/usr \
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
	  STATIC=YesPlease \
	  DESTDIR=$(GIT_BUILD)/install
	@touch $@

# ================================================================
# gcc (binutils + GCC canadian-cross)
# ================================================================
GCC_BUILD_DIR := $(BUILD_DIR)/obj/$(ARCH)/gcc-toolchain
GCC_INSTALL   := $(BUILD_DIR)/obj/$(ARCH)/gcc-install
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

$(STAMP_DIR)/.gcc-built: musl_check
	@mkdir -p $(GCC_BUILD_DIR) $(GCC_INSTALL) $(STAMP_DIR)
	@echo "[EXTRA] Building GCC toolchain for $(ARCH)..."
	$(EXTRA_DIR)/build-gcc.sh $(ARCH) $(MUSL_BUILD) $(GCC_BUILD_DIR) $(GCC_INSTALL)
	@touch $@

# ================================================================
# Top-level targets
# ================================================================
all: $(BUILD_DIR)/vim $(BUILD_DIR)/git $(BUILD_DIR)/gcc $(BUILD_DIR)/cc

vim: $(VIM_BIN)
git: $(GIT_BIN)
gcc: $(GCC_BIN) $(CC_BIN)

clean:
	rm -rf $(USER_DIR)/build/extra

.PHONY: all vim git gcc clean musl_check

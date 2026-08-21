# extra.mk — Build selected packages for the A20OS "extra" disk
#
# Invoked from the top-level Makefile as:
#   $(MAKE) -f user/extra.mk ARCH=riscv64
#
# Output goes to user/build/extra/<arch>/ and its obj directory.

ARCH ?= riscv64
OPT ?= -O2
PACKAGES ?= vim git gcc
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
VIM_SRC      := $(USER_DIR)/external/apps/vim/src
VIM_BUILD    := $(OBJ_DIR)/vim
VIM_WORK_SRC := $(VIM_BUILD)/src
GIT_SRC      := $(USER_DIR)/external/apps/git
GIT_BUILD    := $(OBJ_DIR)/git
GIT_WORK_SRC := $(GIT_BUILD)/src
TERMCAP_LIB  := $(OBJ_DIR)/libtermcap.a
ZLIB_SRC     := $(USER_DIR)/external/libs/zlib
ZLIB_BUILD   := $(OBJ_DIR)/zlib
ZLIB_LIB     := $(ZLIB_BUILD)/libz.a
ZLIB_CFLAGS  = $(CFLAGS) -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN -include errno.h
DOWNLOAD_DIR := $(OBJ_DIR)/downloads
MBEDTLS_VERSION := 3.6.7
MBEDTLS_SHA256 := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBEDTLS_ARCHIVE := $(DOWNLOAD_DIR)/mbedtls-$(MBEDTLS_VERSION).tar.bz2
MBEDTLS_SRC := $(OBJ_DIR)/mbedtls-$(MBEDTLS_VERSION)
MBEDTLS_PREFIX := $(OBJ_DIR)/mbedtls-prefix
MBEDTLS_STAMP := $(MBEDTLS_PREFIX)/.built
MBEDTLS_LIBS := $(MBEDTLS_PREFIX)/lib/libmbedtls.a \
                $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
                $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a
CURL_VERSION := 8.21.0
CURL_SHA256 := aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
CURL_ARCHIVE := $(DOWNLOAD_DIR)/curl-$(CURL_VERSION).tar.xz
CURL_SRC := $(OBJ_DIR)/curl-$(CURL_VERSION)
CURL_BUILD := $(OBJ_DIR)/curl-build
CURL_STAMP := $(CURL_BUILD)/.built
CURL_LIB := $(CURL_BUILD)/lib/.libs/libcurl.a
CA_CERT_BUNDLE ?= $(firstword $(wildcard /etc/ssl/certs/ca-certificates.crt /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem /etc/ssl/cert.pem))
BINUTILS_SRC := $(USER_DIR)/external/toolchain/binutils
GCC_SRC      := $(USER_DIR)/external/gcc
MCM_DIR      := $(USER_DIR)/external/toolchain/musl-cross-make
MCM_OUTPUT   := $(MCM_DIR)/output
MCM_TARGET   := riscv64-linux-musl
MCM_CC       := $(MCM_OUTPUT)/bin/$(MCM_TARGET)-gcc
MCM_BUILD_DIR ?= build/a20/$(MCM_TARGET)
MCM_GCC_CONFIG ?= --disable-shared
MCM_GNU_SITE ?= https://ftp.gnu.org/gnu
MCM_GCC_SRC  := $(MCM_DIR)/$(MCM_BUILD_DIR)/src_gcc
RUST_INSTALL := $(OBJ_DIR)/rust
RUST_STAMP   := $(STAMP_DIR)/.rust-built
RUST_SYSROOT := $(RUST_INSTALL)/a20-sysroot
LAMINA_SRC         := $(USER_DIR)/external/toolchain/Lamina1
LAMINA_BUILD       := $(OBJ_DIR)/lamina
LAMINA_WORK_SRC    := $(LAMINA_BUILD)/src
LAMINA_TOOLCHAIN   := $(EXTRA_DIR)/lamina-toolchain-riscv64.cmake
LAMINA_CROSS_CC    := riscv64-linux-gnu-gcc
LAMINA_CROSS_CXX   := riscv64-linux-gnu-g++
LAMINA_CROSS_STRIP := riscv64-linux-gnu-strip
LAMINA_BIN         := $(BUILD_DIR)/lamina
LAMINA_STAMP       := $(STAMP_DIR)/.lamina-built

# ----------------------------------------------------------------
# Toolchain (mirrors user/Makefile)
# ----------------------------------------------------------------
CCACHE ?= $(shell command -v ccache 2>/dev/null)
CCACHE_PREFIX := $(if $(CCACHE),$(CCACHE) ,)
RISCV_ELF_PREFIX ?= $(if $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null),riscv64-unknown-elf-,$(if $(shell command -v riscv64-elf-gcc 2>/dev/null),riscv64-elf-,riscv64-unknown-elf-))
CROSS_COMPILE_riscv64      := $(RISCV_ELF_PREFIX)
CROSS_COMPILE_loongarch64  := loongarch64-linux-gnu-
CROSS_COMPILE_aarch64      := aarch64-linux-gnu-
CROSS_COMPILE ?= $(CROSS_COMPILE_$(ARCH))

CC      := $(CCACHE_PREFIX)$(CROSS_COMPILE)gcc
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
	  CC="$(CC)" AR="$(AR)" RANLIB="$(RANLIB)" \
	  CFLAGS="$(ZLIB_CFLAGS)" \
	  $(ZLIB_SRC)/configure --static
	# Cross-link probes cannot use our bare-metal compiler command directly;
	# override their NO_STRERROR/NO_vsnprintf result for the real musl build.
	$(MAKE) -C $(ZLIB_BUILD) libz.a CFLAGS="$(ZLIB_CFLAGS)"
	mkdir -p $(ZLIB_BUILD)/include
	cp $(ZLIB_SRC)/zlib.h $(ZLIB_BUILD)/zconf.h $(ZLIB_BUILD)/include/
	@echo "[EXTRA] zlib -> $@"

$(MBEDTLS_ARCHIVE):
	@mkdir -p $(DOWNLOAD_DIR)
	curl -fL --retry 3 -o $@.tmp \
		https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBEDTLS_VERSION)/mbedtls-$(MBEDTLS_VERSION).tar.bz2
	@echo "$(MBEDTLS_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(MBEDTLS_STAMP): $(MBEDTLS_ARCHIVE) $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
	@echo "[EXTRA] Building Mbed TLS $(MBEDTLS_VERSION) for $(ARCH)..."
	rm -rf $(MBEDTLS_SRC) $(MBEDTLS_PREFIX)
	tar -xf $(MBEDTLS_ARCHIVE) -C $(OBJ_DIR)
	$(MAKE) -C $(MBEDTLS_SRC) lib \
		CC="$(CC)" AR="$(AR)" \
		CFLAGS="$(CFLAGS) -D__unix__"
	mkdir -p $(MBEDTLS_PREFIX)/include $(MBEDTLS_PREFIX)/lib
	cp -a $(MBEDTLS_SRC)/include/. $(MBEDTLS_PREFIX)/include/
	cp $(MBEDTLS_SRC)/library/libmbedtls.a \
		$(MBEDTLS_SRC)/library/libmbedx509.a \
		$(MBEDTLS_SRC)/library/libmbedcrypto.a $(MBEDTLS_PREFIX)/lib/
	@touch $@

$(CURL_ARCHIVE):
	@mkdir -p $(DOWNLOAD_DIR)
	curl -fL --retry 3 -o $@.tmp https://curl.se/download/curl-$(CURL_VERSION).tar.xz
	@echo "$(CURL_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(CURL_STAMP): $(CURL_ARCHIVE) $(MBEDTLS_STAMP) $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
	@echo "[EXTRA] Building curl $(CURL_VERSION) with Mbed TLS for $(ARCH)..."
	rm -rf $(CURL_SRC) $(CURL_BUILD)
	tar -xf $(CURL_ARCHIVE) -C $(OBJ_DIR)
	mkdir -p $(CURL_BUILD)
	cd $(CURL_BUILD) && \
		CC="$(CC)" AR="$(AR)" RANLIB="$(RANLIB)" \
		CPPFLAGS="$(MUSL_INC) -D_GNU_SOURCE -D__unix__ -I$(MBEDTLS_PREFIX)/include" \
		CFLAGS="$(OPT) -ffreestanding -static $(ARCH_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(CRT_START) -L$(MBEDTLS_PREFIX)/lib" \
		LIBS="$(MBEDTLS_LIBS) $(LIBC) $(CRT_END)" \
		$(CURL_SRC)/configure \
			--host=$(ARCH)-unknown-linux-musl --prefix=/extra \
			--disable-shared --enable-static --disable-threaded-resolver \
			--disable-ipv6 --disable-ldap --disable-ldaps --disable-rtsp \
			--disable-dict --disable-telnet --disable-tftp --disable-pop3 \
			--disable-imap --disable-smb --disable-smtp --disable-gopher \
			--disable-mqtt --disable-manual --disable-docs --without-zlib \
			--without-libpsl --without-brotli --without-zstd --without-nghttp2 \
			--without-libidn2 --without-libssh2 --with-mbedtls=$(MBEDTLS_PREFIX) \
			--with-ca-bundle=/extra/etc/ssl/certs/ca-certificates.crt
	$(MAKE) -C $(CURL_BUILD)/lib
	@test -f $(CURL_LIB)
	@touch $@

# Source-availability guards: skip building a package if its upstream source tree is missing.
VIM_AVAILABLE := $(if $(wildcard $(VIM_SRC)/main.c),1)
GIT_AVAILABLE := $(if $(wildcard $(GIT_SRC)/Makefile),1)
# build-gcc.sh creates a RISC-V Canadian cross.  The musl-cross-make output is
# bootstrapped below, so source availability -- rather than an already-built
# cross compiler -- decides whether the native GCC package can be requested.
GCC_AVAILABLE := $(if $(and $(filter riscv64,$(ARCH)), \
                            $(wildcard $(BINUTILS_SRC)/configure), \
                            $(wildcard $(GCC_SRC)/configure), \
                            $(wildcard $(MCM_DIR)/Makefile)),1)

VALID_PACKAGES := vim git gcc cc rust rustc cargo rustfmt lamina
UNKNOWN_PACKAGES := $(filter-out $(VALID_PACKAGES),$(PACKAGES))
ifneq ($(strip $(UNKNOWN_PACKAGES)),)
$(error Unknown extra package(s): $(UNKNOWN_PACKAGES); expected: $(VALID_PACKAGES))
endif

REQUESTED_TARGETS := $(sort \
  $(if $(filter vim,$(PACKAGES)),vim) \
  $(if $(filter git,$(PACKAGES)),git) \
  $(if $(filter gcc cc,$(PACKAGES)),gcc) \
  $(if $(and $(filter riscv64,$(ARCH)),$(filter rust rustc cargo rustfmt,$(PACKAGES))),rust) \
  $(if $(filter lamina,$(PACKAGES)),lamina))

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
	    'CC=$(CC)' \
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
	  VIMRUNTIMEDIR=/extra/share/vim/vim92 \
	  DESTDIR=$(VIM_BUILD)/install \
	  srcdir=$(VIM_WORK_SRC) \
	  'CC=$(CC)' \
	  CFLAGS="$(CFLAGS) -I$(VIM_WORK_SRC) -I$(EXTRA_DIR)" \
	  LDFLAGS="$(LDFLAGS) -L$(dir $(TERMCAP_LIB)) $(CRT_START)" \
	  LIBS="-ltermcap $(LIBC) $(CRT_END)"
	@touch $@
endif

# ================================================================
# git
# ================================================================
GIT_BIN := $(BUILD_DIR)/git
GIT_REMOTE_HTTP_BIN := $(BUILD_DIR)/git-remote-http
GIT_REMOTE_HTTPS_BIN := $(BUILD_DIR)/git-remote-https

$(GIT_BIN) $(GIT_REMOTE_HTTP_BIN) $(GIT_REMOTE_HTTPS_BIN): $(STAMP_DIR)/.git-built
	@mkdir -p $(BUILD_DIR)
	cp $(GIT_WORK_SRC)/$(notdir $@) $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] $(notdir $@) -> $@"

$(STAMP_DIR)/.git-built: $(ZLIB_LIB) $(CURL_STAMP) $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
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
	  gitexecdir=/extra/bin \
	  template_dir=/extra/share/git-core/templates \
	  'CC=$(CC)' \
	  'AR=$(AR)' \
	  CFLAGS="$(CFLAGS) -D__unix__ -I$(ZLIB_BUILD)/include" \
	  LDFLAGS="$(LDFLAGS) $(CRT_START)" \
	  EXTLIBS="$(ZLIB_LIB) $(LIBC) $(CRT_END)" \
	  CURL_CFLAGS="-I$(CURL_SRC)/include -I$(CURL_BUILD)/include" \
	  CURL_LIBCURL="$(CURL_LIB) $(MBEDTLS_LIBS)" \
	  NO_OPENSSL=YesPlease \
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
	  DESTDIR=$(GIT_BUILD)/install \
	  git git-remote-http git-remote-https
	@touch $@
endif

# ================================================================
# gcc (binutils + GCC canadian-cross)
# ================================================================
GCC_BUILD_DIR := $(OBJ_DIR)/gcc-toolchain
GCC_INSTALL   := $(OBJ_DIR)/gcc-install
GCC_BIN       := $(BUILD_DIR)/gcc
CC_BIN        := $(BUILD_DIR)/cc

# musl-cross-make first produces an x86_64-hosted RISC-V cross compiler.  That
# compiler is then used by build-gcc.sh to produce GCC binaries which run on
# RISC-V itself.  The bootstrap only needs static libgcc; disabling shared
# avoids unsupported R_RISCV_JAL relocations when GCC 9 builds libgcc_s.so
# with newer binutils.  A dedicated build directory also isolates these
# settings from any prior default musl-cross-make build.
# Use recursive $(MAKE) so the top-level -j jobserver is shared.
$(MCM_CC): $(MCM_DIR)/Makefile
	@echo "[EXTRA] Bootstrapping $(MCM_TARGET) with musl-cross-make..."
	$(MAKE) -C $(MCM_DIR) TARGET=$(MCM_TARGET) BUILD_DIR=$(MCM_BUILD_DIR) \
		GNU_SITE="$(MCM_GNU_SITE)" GCC_CONFIG="$(MCM_GCC_CONFIG)" install
	@test -x "$@" || { echo "[EXTRA] musl-cross-make did not produce $@"; exit 1; }

$(GCC_BIN): $(STAMP_DIR)/.gcc-built
	@mkdir -p $(BUILD_DIR)
	cp $(GCC_INSTALL)/bin/gcc $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true
	@echo "[EXTRA] gcc -> $@"

$(CC_BIN): $(STAMP_DIR)/.gcc-built
	@mkdir -p $(BUILD_DIR)
	cp $(GCC_INSTALL)/bin/cc $@
	@echo "[EXTRA] cc -> $@"

$(STAMP_DIR)/.gcc-built: $(MUSL_CHECK_FILES) $(if $(GCC_AVAILABLE),$(MCM_CC)) \
                        $(EXTRA_DIR)/build-gcc.sh $(EXTRA_MAKEFILE) | musl_check
ifeq ($(GCC_AVAILABLE),)
	@echo "[EXTRA] GCC prerequisites unavailable; skipping gcc/cc"
	@echo "[EXTRA] Need $(GCC_SRC)/configure, $(BINUTILS_SRC)/configure, and $(MCM_DIR)/Makefile"
	@mkdir -p $(STAMP_DIR)
	@touch $@
else
	@mkdir -p $(GCC_BUILD_DIR) $(GCC_INSTALL) $(STAMP_DIR)
	@echo "[EXTRA] Building GCC toolchain for $(ARCH)..."
	$(EXTRA_DIR)/build-gcc.sh $(ARCH) $(MUSL_BUILD) $(GCC_BUILD_DIR) $(GCC_INSTALL) $(MCM_GCC_SRC)
	@touch $@
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
# lamina (Lamina1 language toolchain: compiler + register VM)
# ================================================================
# Lamina1 is a C++23/CMake project (compiler/ + runtime/ plus the dyncall
# and LMCAS submodules) providing the `lamina` CLI.  There is no musl C++
# standard library in the build environment, so we cross-build with the
# host Debian riscv64 glibc toolchain (riscv64-linux-gnu-g++).
#
# lamina is shipped as a DYNAMIC executable: its runtime loads the stdlib
# module "laminaCore" through dlopen() at runtime, which cannot work in a
# fully static binary (glibc >= 2.34 rejects dlopen in static binaries).
# The executable and its four shared libraries (laminaCore, lmcas, lmmc,
# LammpCore) are therefore copied together into $(BUILD_DIR) and installed
# side by side on the extra disk; the glibc runtime itself is already
# staged for the rust package (see tools/targets-extra.mk).
#
# The toolchain file (lamina-toolchain-riscv64.cmake) links every object
# with -Wl,--disable-new-dtags so rpaths become DT_RPATH, which glibc's
# dlopen() honors — the ModuleLoader then finds liblaminaCore.so in the
# same directory as the lamina binary via $ORIGIN.
#
# The upstream Release block injects '-march=native' (rejected by the
# riscv64 backend), so we build with MinSizeRel.  Sources are copied to
# the obj dir first, keeping the vendored tree pristine (same pattern as
# the vim/git recipes).  The build is serial: the LMCAS/LAMMP C++ TU
# chain is memory-hungry and parallel jobs OOM the build host.
#
# Currently supported only on riscv64 (glibc cross C++ toolchain); other
# architectures skip lamina gracefully so the extra image still builds.
LAMINA_AVAILABLE := $(if $(and $(filter riscv64,$(ARCH)), \
                               $(wildcard $(LAMINA_SRC)/CMakeLists.txt), \
                               $(wildcard $(LAMINA_SRC)/external/LMCAS/CMakeLists.txt), \
                               $(wildcard $(LAMINA_SRC)/external/dyncall/CMakeLists.txt), \
                               $(shell command -v $(LAMINA_CROSS_CXX) 2>/dev/null)),1)

$(LAMINA_BIN): $(LAMINA_STAMP)
	@mkdir -p $(BUILD_DIR)
	cp $(LAMINA_BUILD)/lamina $@
	@for so in $$(find $(LAMINA_BUILD) \( -name 'liblaminaCore.so*' -o -name 'liblmcas.so*' -o -name 'liblmmc.so*' -o -name 'libLammpCore.so*' \)); do \
		cp -P "$$so" "$(BUILD_DIR)/$$(basename "$$so")"; \
	done
	@for so in /usr/riscv64-linux-gnu/lib/libstdc++.so.6*; do \
		cp -P "$$so" "$(BUILD_DIR)/$$(basename "$$so")"; \
	done
	@find $(BUILD_DIR) -maxdepth 1 \( -name 'liblaminaCore.so*' -o -name 'liblmcas.so*' -o -name 'liblmmc.so*' -o -name 'libLammpCore.so*' -o -name 'libstdc++.so*' \) -type f -exec $(LAMINA_CROSS_STRIP) {} \; 2>/dev/null || true
	$(LAMINA_CROSS_STRIP) $@ 2>/dev/null || true
	@echo "[EXTRA] lamina + shared libs -> $(BUILD_DIR)"

$(LAMINA_STAMP): $(EXTRA_MAKEFILE) $(LAMINA_TOOLCHAIN)
ifeq ($(LAMINA_AVAILABLE),)
	@echo "[EXTRA] lamina unavailable: needs ARCH=riscv64, $(LAMINA_CROSS_CXX) and the Lamina1 tree with dyncall/LMCAS submodules; skipping lamina"
	@mkdir -p $(STAMP_DIR)
	@touch $@
else
	@mkdir -p $(LAMINA_BUILD) $(STAMP_DIR)
	@echo "[EXTRA] Building lamina for $(ARCH)..."
	@rm -rf $(LAMINA_WORK_SRC)
	@mkdir -p $(LAMINA_WORK_SRC)
	@cp -a $(LAMINA_SRC)/. $(LAMINA_WORK_SRC)/
	cd $(LAMINA_BUILD) && cmake -S $(LAMINA_WORK_SRC) -B . \
	  -DCMAKE_TOOLCHAIN_FILE=$(LAMINA_TOOLCHAIN) \
	  -DCMAKE_BUILD_TYPE=MinSizeRel \
	  -DLMX_BUILD_TESTS=OFF \
	  -DLMCAS_BUILD_STANDALONE_TESTS=OFF -DLMCAS_BUILD_BENCHMARKS=OFF \
	  -DLMMC_BUILD_TESTS=OFF -DLMMC_BUILD_EXAMPLES=OFF \
	  -DDYNCALL_BUILD_TESTS=OFF -DDYNCALL_BUILD_EXAMPLES=OFF
	cmake --build $(LAMINA_BUILD) --target lamina
	@for so in liblaminaCore.so liblmcas.so.2 liblmmc.so libLammpCore.so.1; do \
		[ -n "$$(find $(LAMINA_BUILD) -name "$$so" | head -1)" ] || \
		{ echo "[EXTRA] ERROR: $$so not produced by the Lamina1 build" >&2; exit 1; }; \
	done
	@touch $@
endif

# ================================================================
# Top-level targets
# ================================================================
all: $(REQUESTED_TARGETS)

vim: $(if $(VIM_AVAILABLE),$(VIM_BIN))
git: $(if $(GIT_AVAILABLE),$(GIT_BIN) $(GIT_REMOTE_HTTP_BIN) $(GIT_REMOTE_HTTPS_BIN))
gcc: $(if $(GCC_AVAILABLE),$(GCC_BIN) $(CC_BIN))
lamina: $(if $(LAMINA_AVAILABLE),$(LAMINA_BIN))

clean:
	rm -rf $(BUILD_ROOT)

.PHONY: all vim git gcc rust rustc cargo rustfmt lamina clean musl_check

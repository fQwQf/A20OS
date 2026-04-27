ARCH ?= riscv64
CONTEST ?= 0
OPT ?= -O3

.DEFAULT_GOAL := all

SUPPORTED_ARCHES := riscv64 loongarch64 aarch64
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH '$(ARCH)'; expected one of: $(SUPPORTED_ARCHES))
endif

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj/$(ARCH)-$(CONTEST)
MUSL_BASE := $(CURDIR)/external/musl
MUSL_BUILD := $(MUSL_BASE)/build-$(ARCH)
MUSL_CRT := $(MUSL_BUILD)/lib

CROSS_COMPILE_riscv64 := riscv64-unknown-elf-
CROSS_COMPILE_loongarch64 := loongarch64-linux-gnu-
CROSS_COMPILE_aarch64 := aarch64-linux-gnu-
CROSS_COMPILE ?= $(CROSS_COMPILE_$(ARCH))

MUSL_ARCH_riscv64 := riscv64
MUSL_ARCH_loongarch64 := loongarch64
MUSL_ARCH_aarch64 := aarch64
MUSL_ARCH := $(MUSL_ARCH_$(ARCH))

MUSL_TARGET_riscv64 := riscv64
MUSL_TARGET_loongarch64 := loongarch64-linux-gnu
MUSL_TARGET_aarch64 := aarch64-linux-gnu
MUSL_TARGET := $(MUSL_TARGET_$(ARCH))

ARCH_CFLAGS_riscv64 := -mabi=lp64 -march=rv64g -mcmodel=medany
ARCH_CFLAGS_loongarch64 := -mabi=lp64d -march=loongarch64 -mcmodel=normal -fno-pic
ARCH_CFLAGS_aarch64 := -march=armv8-a -fno-pic -fno-tree-vectorize
ARCH_CFLAGS := $(ARCH_CFLAGS_$(ARCH))

ARCH_LDFLAGS_riscv64 :=
ARCH_LDFLAGS_loongarch64 := -no-pie
ARCH_LDFLAGS_aarch64 := -no-pie
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH))

MUSL_CONFIG_CFLAGS_riscv64 := $(OPT) -mcmodel=medany -march=rv64g -mabi=lp64
MUSL_CONFIG_CFLAGS_loongarch64 := $(OPT) -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
MUSL_CONFIG_CFLAGS_aarch64 := $(OPT) -fno-pic -static -fno-tree-vectorize
MUSL_CONFIG_CFLAGS := $(MUSL_CONFIG_CFLAGS_$(ARCH))

CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib

MUSL_INC := -isystem $(MUSL_BUILD)/obj/include \
            -isystem $(MUSL_BASE)/include \
            -isystem $(MUSL_BASE)/arch/$(MUSL_ARCH) \
            -isystem $(MUSL_BASE)/arch/generic

CFLAGS := -Wall -Wextra $(OPT) -ffreestanding -nostdinc $(MUSL_INC) \
          -Iinit_common $(ARCH_CFLAGS)
LDFLAGS := -static -nostdlib -Ttext 0x10000 $(ARCH_LDFLAGS)

ifeq ($(CONTEST),1)
CFLAGS += -DCONTEST
endif

LIBGCC_DEFAULT := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)
ifeq ($(ARCH),riscv64)
LIBGCC_LP64 := $(dir $(LIBGCC_DEFAULT))rv64i/lp64/libgcc.a
LIBGCC := $(if $(wildcard $(LIBGCC_LP64)),$(LIBGCC_LP64),$(LIBGCC_DEFAULT))
MUSL_CONFIG_LDFLAGS := -L$(dir $(LIBGCC))
else
LIBGCC := $(LIBGCC_DEFAULT)
MUSL_CONFIG_LDFLAGS :=
endif

CRT_START := $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o
CRT_END := $(MUSL_CRT)/crtn.o
LIB_A20OS := $(OBJ_DIR)/lib/brk.o
LIBC := $(LIB_A20OS) $(MUSL_CRT)/libc.a $(LIBGCC)

MUSL_CHECK_FILES := $(MUSL_CRT)/crt1.o \
                    $(MUSL_CRT)/crti.o \
                    $(MUSL_CRT)/crtn.o \
                    $(MUSL_CRT)/libc.a \
                    $(MUSL_BUILD)/obj/include/bits/alltypes.h \
                    $(MUSL_BUILD)/obj/include/bits/syscall.h

SBASE_DIR := external/sbase
WGET_CMD_SRC := cmds/wget.c
WGET_OBJ := $(OBJ_DIR)/cmds/wget.o
WGET_CMD := $(BUILD_DIR)/wget
LOCAL_CMD_SRCS := $(filter-out $(WGET_CMD_SRC),$(wildcard cmds/*.c))
LOCAL_CMD_NAMES := $(basename $(notdir $(LOCAL_CMD_SRCS)))

SBASE_ALL_BINS := basename cat chgrp chmod chown cksum cmp comm cp cut date dd \
                  dirname du echo env expand expr false find fold grep head hostname \
                  join kill link ln logger logname ls mkdir mkfifo mknod mktemp mv \
                  nice nl nohup od paste pathchk printenv printf pwd readlink rev rm \
                  rmdir sed seq setsid sleep sort split sponge strings sync tail tar \
                  tee test time touch tr true tty uname unexpand uniq unlink uudecode \
                  uuencode wc which whoami xargs xinstall yes
SBASE_BINS := $(filter-out $(LOCAL_CMD_NAMES),$(SBASE_ALL_BINS))
SBASE_CMDS := $(addprefix $(BUILD_DIR)/,$(SBASE_BINS))
SBASE_OBJS := $(addprefix $(OBJ_DIR)/sbase/,$(addsuffix .o,$(SBASE_BINS)))
SBASE_LIBUTF_SRCS := $(wildcard $(SBASE_DIR)/libutf/*.c)
SBASE_LIBUTIL_SRCS := $(wildcard $(SBASE_DIR)/libutil/*.c)
SBASE_LIBUTF_OBJS := $(patsubst $(SBASE_DIR)/%.c,$(OBJ_DIR)/sbase/%.o,$(SBASE_LIBUTF_SRCS))
SBASE_LIBUTIL_OBJS := $(patsubst $(SBASE_DIR)/%.c,$(OBJ_DIR)/sbase/%.o,$(SBASE_LIBUTIL_SRCS))
SBASE_LIBUTF := $(OBJ_DIR)/sbase/libutf.a
SBASE_LIBUTIL := $(OBJ_DIR)/sbase/libutil.a
SBASE_CPPFLAGS := -DPREFIX=\"/\" \
                  -D_DEFAULT_SOURCE \
                  -D_NETBSD_SOURCE \
                  -D_BSD_SOURCE \
                  -D_XOPEN_SOURCE=700 \
                  -D_FILE_OFFSET_BITS=64 \
                  -I$(SBASE_DIR)
SBASE_CFLAGS := $(CFLAGS) $(SBASE_CPPFLAGS)

TLSE_DIR := external/tlse
TLSE_OBJ := $(OBJ_DIR)/tlse/tlse.o
TLSE_ARCH_CFLAGS_aarch64 := -mgeneral-regs-only -DLTC_NO_TABLES -DNO_TLS_FORWARD_SECRECY \
                             -DA20OS_TLS_NO_GCM -DA20OS_TLS_RSA_CBC_SHA1_ONLY
TLSE_ARCH_CFLAGS := $(TLSE_ARCH_CFLAGS_$(ARCH))
TLSE_BASE_CFLAGS := $(filter-out -O0 -O1 -O2 -O3 -Os -Oz,$(CFLAGS))
TLSE_OPT_aarch64 := -O0
TLSE_OPT := $(if $(TLSE_OPT_$(ARCH)),$(TLSE_OPT_$(ARCH)),-O1)
TLSE_CFLAGS := $(TLSE_BASE_CFLAGS) $(TLSE_OPT) $(TLSE_ARCH_CFLAGS) -fno-strict-aliasing -fno-strict-overflow -fwrapv \
                -I$(TLSE_DIR) -fsigned-char \
                -DTLS_AMALGAMATION \
                -DNO_SSL_COMPATIBLE_INTERFACE \
                -DNO_TLS_WITH_CHACHA20_POLY1305 \
                -DNO_TLS_LEGACY_SUPPORT \
                -DNO_TLS_13 \
                -DNO_TLS_CLIENT_ECDHE \
                -DNO_TLS_ECDSA_SUPPORTED \
                -DNO_TLS_CURVE25519 \
                -DNO_TLS_ROBOT_MITIGATION \
                -DA20OS_NO_FAST_MONTGOMERY \
                -DA20OS_NO_FAST_MUL \
                -DA20OS_NO_FAST_MUL_HIGH \
                -DA20OS_NO_FAST_SQR

LOCAL_CMD_OBJS := $(patsubst cmds/%.c,$(OBJ_DIR)/cmds/%.o,$(LOCAL_CMD_SRCS))
LOCAL_CMDS := $(patsubst cmds/%.c,$(BUILD_DIR)/%,$(LOCAL_CMD_SRCS))

CONTEST_INIT_MAIN := contest_init/contest_init.c
CONTEST_INIT_SRCS := $(filter-out $(CONTEST_INIT_MAIN),$(wildcard contest_init/*.c))
CONTEST_INIT_OBJS := $(patsubst contest_init/%.c,$(OBJ_DIR)/contest_init/%.o,$(CONTEST_INIT_SRCS))
INIT_COMMON_SRCS := $(wildcard init_common/*.c)
INIT_COMMON_OBJS := $(patsubst init_common/%.c,$(OBJ_DIR)/init_common/%.o,$(INIT_COMMON_SRCS))

ifeq ($(CONTEST),1)
INIT_MAIN_SRC := $(CONTEST_INIT_MAIN)
INIT_EXTRA_OBJS := $(INIT_COMMON_OBJS) $(CONTEST_INIT_OBJS)
else
INIT_MAIN_SRC := shell/init.c
INIT_EXTRA_OBJS := $(INIT_COMMON_OBJS)
endif
INIT_OBJ := $(OBJ_DIR)/init.o

MKSH_DIR := external/mksh-cvs2git
MKSH_SRCS := $(MKSH_DIR)/edit.c $(MKSH_DIR)/eval.c $(MKSH_DIR)/exec.c $(MKSH_DIR)/expr.c \
             $(MKSH_DIR)/funcs.c $(MKSH_DIR)/histrap.c $(MKSH_DIR)/jobs.c $(MKSH_DIR)/lalloc.c \
             $(MKSH_DIR)/lex.c $(MKSH_DIR)/main.c $(MKSH_DIR)/misc.c $(MKSH_DIR)/shf.c \
             $(MKSH_DIR)/syn.c $(MKSH_DIR)/tree.c $(MKSH_DIR)/ulimit.c $(MKSH_DIR)/var.c \
             $(MKSH_DIR)/strlcpy.c
MKSH_OBJS := $(patsubst $(MKSH_DIR)/%.c,$(OBJ_DIR)/mksh/%.o,$(MKSH_SRCS))
MKSH_CFLAGS := $(CFLAGS) -I$(MKSH_DIR) \
               -DMKSH_USE_AUTOCONF_H -DMKSH_DISABLE_DEPRECATED \
               -DMKSH_UNEMPLOYED \
               -DMKSH_DONT_EMIT_IDSTRING -DMKSH_BUILDSH -DMKSH_BUILD_R=599

USER_INTERMEDIATE_TARGETS := $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(LIB_A20OS) $(MKSH_OBJS) \
                             $(LOCAL_CMD_OBJS) $(SBASE_OBJS) $(SBASE_LIBUTF_OBJS) \
                             $(SBASE_LIBUTIL_OBJS) $(SBASE_LIBUTF) $(SBASE_LIBUTIL) \
                             $(WGET_OBJ) $(TLSE_OBJ)

all: build_dir $(BUILD_DIR)/init $(BUILD_DIR)/mksh $(BUILD_DIR)/sh $(SBASE_CMDS) $(LOCAL_CMDS) $(WGET_CMD)

build_dir:
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR)

musl_prepare:
	@set -e; \
	missing=0; \
	for f in $(MUSL_CHECK_FILES); do \
		if [ ! -f "$$f" ]; then missing=1; break; fi; \
	done; \
	if [ "$$missing" -ne 0 ]; then \
		echo "[MUSL] build-$(ARCH) missing, rebuilding..."; \
		rm -rf "$(MUSL_BUILD)"; \
		mkdir -p "$(MUSL_BUILD)"; \
		cd "$(MUSL_BUILD)" && ../configure \
			--target=$(MUSL_TARGET) \
			--prefix=/opt/musl-$(ARCH) \
			--disable-shared --disable-wrapper \
			CC=$(CC) CROSS_COMPILE=$(CROSS_COMPILE) \
			CFLAGS='$(MUSL_CONFIG_CFLAGS)' \
			LDFLAGS='$(MUSL_CONFIG_LDFLAGS)'; \
		$(MAKE) -C "$(MUSL_BUILD)" -j$$(nproc); \
	else \
		echo "[MUSL] build-$(ARCH) ready"; \
	fi

$(BUILD_DIR)/mksh: $(MKSH_OBJS) $(LIB_A20OS) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(MKSH_OBJS) $(CRT_END) $(LIBC)

$(BUILD_DIR)/sh: $(BUILD_DIR)/mksh | build_dir
	cp $< $@

$(BUILD_DIR)/init: $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(LIB_A20OS) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(CRT_END) $(LIBC)

$(LOCAL_CMDS): $(BUILD_DIR)/%: $(OBJ_DIR)/cmds/%.o $(LIB_A20OS) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $< $(CRT_END) $(LIBC)

$(SBASE_CMDS): $(BUILD_DIR)/%: $(OBJ_DIR)/sbase/%.o $(SBASE_LIBUTIL) $(SBASE_LIBUTF) $(LIB_A20OS) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $< $(CRT_END) $(SBASE_LIBUTIL) $(SBASE_LIBUTF) $(LIBC)

$(WGET_CMD): $(WGET_OBJ) $(TLSE_OBJ) $(LIB_A20OS) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(WGET_OBJ) $(TLSE_OBJ) $(CRT_END) $(LIBC)

$(INIT_OBJ): $(INIT_MAIN_SRC) Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/cmds/%.o: cmds/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/sbase/%.o: $(SBASE_DIR)/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(SBASE_CFLAGS) -c $< -o $@

$(OBJ_DIR)/sbase/libutf/%.o: $(SBASE_DIR)/libutf/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(SBASE_CFLAGS) -c $< -o $@

$(OBJ_DIR)/sbase/libutil/%.o: $(SBASE_DIR)/libutil/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(SBASE_CFLAGS) -c $< -o $@

$(WGET_OBJ): $(WGET_CMD_SRC) Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(TLSE_DIR) -c $< -o $@

$(OBJ_DIR)/tlse/%.o: $(TLSE_DIR)/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(TLSE_CFLAGS) -c $< -o $@

$(SBASE_LIBUTF): $(SBASE_LIBUTF_OBJS) | build_dir
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SBASE_LIBUTIL): $(SBASE_LIBUTIL_OBJS) | build_dir
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/contest_init/%.o: contest_init/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/init_common/%.o: init_common/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/lib/%.o: lib/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/mksh/%.o: $(MKSH_DIR)/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(MKSH_CFLAGS) -c $< -o $@

clean:
	@for i in 1 2 3; do \
		rm -rf $(BUILD_DIR) && break; \
		sleep 0.1; \
	done
	@test ! -e $(BUILD_DIR)
	rm -f $(MKSH_DIR)/*.o lib/*.o

distclean: clean
	rm -rf $(MUSL_BASE)/build-riscv64 $(MUSL_BASE)/build-loongarch64 $(MUSL_BASE)/build-aarch64

.PHONY: all clean distclean build_dir musl_prepare
.PRECIOUS: $(USER_INTERMEDIATE_TARGETS)
.SECONDARY: $(USER_INTERMEDIATE_TARGETS)

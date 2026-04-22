ARCH ?= riscv64
CONTEST ?= 0

MUSL_BASE  := $(shell pwd)/external/musl
MUSL_BUILD := $(MUSL_BASE)/build-riscv64

ifeq ($(ARCH), riscv64)
CROSS_COMPILE ?= riscv64-unknown-elf-

MUSL_CRT  := $(MUSL_BUILD)/lib
MUSL_INC  := -isystem $(MUSL_BUILD)/obj/include \
             -isystem $(MUSL_BASE)/include \
             -isystem $(MUSL_BASE)/arch/riscv64 \
             -isystem $(MUSL_BASE)/arch/generic

CFLAGS  = -Wall -Wextra -O2 -ffreestanding -nostdinc $(MUSL_INC) \
          -mabi=lp64 -march=rv64g -mcmodel=medany
LDFLAGS = -static -nostdlib -Ttext 0x10000

GCC_VER := $(shell $(CROSS_COMPILE)gcc -dumpversion 2>/dev/null)
LIBGCC_DEFAULT := $(shell $(CROSS_COMPILE)gcc -print-libgcc-file-name 2>/dev/null)
LIBGCC_LP64 := $(dir $(LIBGCC_DEFAULT))rv64i/lp64/libgcc.a
LIBGCC  := $(if $(wildcard $(LIBGCC_LP64)),$(LIBGCC_LP64),$(LIBGCC_DEFAULT))
LIBGCC_DIR := $(dir $(LIBGCC))

LIB_A20OS := lib/brk.o
CRT_START = $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o
CRT_END   = $(MUSL_CRT)/crtn.o
LIBC      = $(LIB_A20OS) $(MUSL_CRT)/libc.a $(LIBGCC)

else ifeq ($(ARCH), loongarch64)
CROSS_COMPILE ?= loongarch64-unknown-elf-
CFLAGS = -Wall -Wextra -O2 -fno-builtin -nostdlib -mabi=lp64d -march=loongarch64 -I./lib
LDFLAGS = -static -nostdlib -Ttext 0x10000

CRT_START = lib/crt0.o lib/syscall.o lib/libc.o
CRT_END   =
LIBC      =

endif

ifeq ($(CONTEST),1)
CFLAGS += -DCONTEST
endif

CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

# Shell and Commands
SH_SRC = shell/sh.c
CMD_SRCS = $(wildcard cmds/*.c)
CMDS = $(patsubst cmds/%.c, build/%, $(CMD_SRCS))

# mksh
MKSH_DIR = external/mksh-cvs2git
MKSH_SRCS = $(MKSH_DIR)/edit.c $(MKSH_DIR)/eval.c $(MKSH_DIR)/exec.c $(MKSH_DIR)/expr.c \
            $(MKSH_DIR)/funcs.c $(MKSH_DIR)/histrap.c $(MKSH_DIR)/jobs.c $(MKSH_DIR)/lalloc.c \
            $(MKSH_DIR)/lex.c $(MKSH_DIR)/main.c $(MKSH_DIR)/misc.c $(MKSH_DIR)/shf.c \
            $(MKSH_DIR)/syn.c $(MKSH_DIR)/tree.c $(MKSH_DIR)/ulimit.c $(MKSH_DIR)/var.c \
            $(MKSH_DIR)/strlcpy.c
MKSH_OBJS = $(MKSH_SRCS:.c=.o)
MKSH_CFLAGS = $(CFLAGS) -I$(MKSH_DIR) \
              -DMKSH_USE_AUTOCONF_H -DMKSH_DISABLE_DEPRECATED \
              -DMKSH_UNEMPLOYED \
              -DMKSH_DONT_EMIT_IDSTRING -DMKSH_BUILDSH -DMKSH_BUILD_R=599

all: build_dir build/init build/mksh build/sh $(CMDS)

ifeq ($(ARCH),riscv64)
all: musl_prepare
MUSL_DEPS := musl_prepare
endif

build_dir:
	@mkdir -p build

musl_prepare:
ifeq ($(ARCH),riscv64)
	@if [ ! -f "$(MUSL_BUILD)/lib/crt1.o" ] || [ ! -f "$(MUSL_BUILD)/lib/libc.a" ] || [ ! -f "$(MUSL_BUILD)/obj/include/bits/alltypes.h" ]; then \
		echo "[MUSL] build-riscv64 missing, rebuilding..."; \
		rm -rf "$(MUSL_BUILD)"; \
		mkdir -p "$(MUSL_BUILD)"; \
		cd "$(MUSL_BUILD)" && ../configure \
			--target=riscv64 \
			--prefix=/opt/musl-riscv64 \
			--disable-shared \
			--disable-wrapper \
			CC=$(CROSS_COMPILE)gcc \
			CROSS_COMPILE=$(CROSS_COMPILE) \
			CFLAGS='-O2 -mcmodel=medany -march=rv64g -mabi=lp64' \
			LDFLAGS='-L$(LIBGCC_DIR)'; \
		$(MAKE) -C "$(MUSL_BUILD)" -j$$(nproc); \
	else \
		echo "[MUSL] build-riscv64 ready"; \
	fi
endif

%.o: %.c $(MUSL_DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(MUSL_DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c $(MUSL_DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

build/sh: $(SH_SRC) $(LIB_A20OS) $(MUSL_DEPS)
	$(CC) $(CFLAGS) -c $< -o build/sh.o
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) build/sh.o $(CRT_END) $(LIBC)

build/mksh: $(MKSH_OBJS) $(LIB_A20OS) $(MUSL_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(MKSH_OBJS) $(CRT_END) $(LIBC)

$(MKSH_DIR)/%.o: $(MKSH_DIR)/%.c $(MUSL_DEPS)
	$(CC) $(MKSH_CFLAGS) -c $< -o $@

build/init: shell/init.c $(LIB_A20OS) $(MUSL_DEPS)
	$(CC) $(CFLAGS) -c $< -o build/init.o
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) build/init.o $(CRT_END) $(LIBC)

build/%: cmds/%.c $(LIB_A20OS) $(MUSL_DEPS)
	$(CC) $(CFLAGS) -c $< -o build/$*.o
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) build/$*.o $(CRT_END) $(LIBC)

clean:
	rm -rf build $(MKSH_DIR)/*.o lib/*.o

distclean: clean
	rm -rf $(MUSL_BASE)/build-riscv64 $(MUSL_BASE)/build-loongarch64

.PHONY: all clean distclean build_dir musl_prepare

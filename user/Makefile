ARCH ?= riscv64
CONTEST ?= 0

ifeq ($(ARCH), riscv64)
CROSS_COMPILE ?= riscv64-unknown-elf-
CFLAGS = -Wall -Wextra -O2 -fno-builtin -nostdlib -mabi=lp64 -march=rv64g -mcmodel=medany -I./lib
    LDFLAGS = -static -nostdlib -Ttext 0x10000
else ifeq ($(ARCH), loongarch64)
CROSS_COMPILE ?= loongarch64-unknown-elf-
CFLAGS = -Wall -Wextra -O2 -fno-builtin -nostdlib -mabi=lp64d -march=loongarch64 -I./lib
LDFLAGS = -static -nostdlib -Ttext 0x10000
endif

ifeq ($(CONTEST),1)
CFLAGS += -DCONTEST
endif

CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

LIB_SRCS = lib/crt0.S lib/syscall.S lib/libc.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB_OBJS := $(LIB_OBJS:.S=.o)

# Shell and Commands
SH_SRC = shell/sh.c
CMD_SRCS = $(wildcard cmds/*.c)
CMDS = $(patsubst cmds/%.c, build/%, $(CMD_SRCS))

all: build_dir build/init build/sh $(CMDS)

build_dir:
	@mkdir -p build

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

build/sh: $(SH_SRC) $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

build/init: shell/init.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

build/%: cmds/%.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

clean:
	rm -rf build lib/*.o

.PHONY: all clean build_dir

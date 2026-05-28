# A20OS Makefile

# Parallel build
NPROC ?= $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)

# Architecture selection
ARCH ?= riscv64
ABI ?= both
MODE ?= release
BRINGUP ?= 0
OPT ?= -O3

# ABI selection: linux, native, both (compile both ABI layers simultaneously)
ifeq ($(filter $(ABI),linux native both),)
$(error Unsupported ABI '$(ABI)'; supported: linux, native, both)
endif

.DEFAULT_GOAL := all

# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include
BUILD_VARIANT = $(ABI)-$(if $(filter 1,$(BRINGUP)),bringup,dev)
BUILD_DIR = .kernel-build/$(ARCH)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img
EXT4_IMG = $(BUILD_DIR)/ext4.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
USER_BUILD_STAMP = user/build/.build-id
ARCH_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/include
EXT4_STAGING_DIR = $(BUILD_DIR)/ext4-staging
BUILD_TIME_HDR = $(BUILD_DIR)/generated/build_time.h
FAT32_IMAGE_MB ?= 128
EXT4_IMAGE_MB ?= 128
EXTRA_IMAGE_MB ?= 256
EXTRA_IMG = $(BUILD_DIR)/extra.img
EXTRA_STAGING_DIR = $(BUILD_DIR)/extra-staging
EXTRA_PACKAGES = vim git gcc cc
USER_BUILD_ID = $(ARCH):$(OPT)
USER_BUILD_CHECK_DIRS = user/cmds user/init_common user/lib user/shell \
                        user/external/musl user/external/sbase user/external/mksh-cvs2git \
                        user/external/tlse user/external/fastfetch
comma := ,
NET_HOSTFWD ?= hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
NETDEV_USER = -netdev user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
SMOKE_TIMEOUT ?= 20s
SMOKE_INPUT_DELAY ?= 2
SMOKE_LOG_DIR ?= .kernel-build/smoke

PROTOCOLS_LINES = \
    'hopopt 0 HOPOPT' \
    'icmp 1 ICMP' \
    'igmp 2 IGMP' \
    'tcp 6 TCP' \
    'udp 17 UDP' \
    'ipv6 41 IPv6' \
    'ipv6-route 43 IPv6-Route' \
    'ipv6-frag 44 IPv6-Frag' \
    'esp 50 ESP' \
    'ah 51 AH' \
    'ipv6-icmp 58 IPv6-ICMP' \
    'ipv6-nonxt 59 IPv6-NoNxt' \
    'ipv6-opts 60 IPv6-Opts'

LIBGCC_S_riscv64 := /usr/riscv64-linux-gnu/lib/libgcc_s.so.1
LIBGCC_S_loongarch64 := /usr/loongarch64-linux-gnu/lib/libgcc_s.so.1
LIBGCC_S_aarch64 := /usr/lib/aarch64-linux-gnu/libgcc_s.so.1
LIBGCC_S_ARCH := $(LIBGCC_S_$(ARCH))

# Compiler and tools
ifeq ($(ARCH), riscv64)
    CROSS_PREFIX = riscv64-unknown-elf-
    ARCH_CFLAGS = -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany
    ARCH_LDFLAGS =
    QEMU = qemu-system-riscv64
    QEMU_FLAGS = -machine virt -m 1G -nographic -smp 1 -bios default -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), loongarch64)
    CROSS_PREFIX = loongarch64-linux-gnu-
    ARCH_CFLAGS = -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-loongarch64
    QEMU_FLAGS = -machine virt -m 1G -nographic -smp 1
else ifeq ($(ARCH), aarch64)
    CROSS_PREFIX = aarch64-linux-gnu-
    ARCH_CFLAGS = -march=armv8-a -mgeneral-regs-only -fno-pic -mcmodel=large -mno-outline-atomics
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-aarch64
    QEMU_FLAGS = -machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 -global virtio-mmio.force-legacy=false
endif

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
ifeq ($(ARCH), riscv64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
endif

else ifeq ($(ARCH), loongarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-pci,netdev=net

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x1 -device virtio-blk-pci,drive=x1
endif

else ifeq ($(ARCH), aarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4

endif
endif

# Compiler flags
CFLAGS = -Wall -Wextra $(OPT) -ffreestanding -nostdlib \
         -nostartfiles -fno-builtin -fno-common -std=gnu99 \
         -MMD -MP \
         -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(KERNEL_DIR)/net/lwip_port \
         -I$(KERNEL_DIR)/external/lwip/src/include \
         -I$(ARCH_INCLUDE_DIR) -I$(BUILD_DIR)/generated $(ARCH_CFLAGS) \
         -D$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_$(shell echo $(ARCH) | tr a-z A-Z) \
         -DCONFIG_ABI_$(shell echo $(ABI) | tr a-z A-Z)
ifeq ($(ABI),both)
CFLAGS += -DCONFIG_ABI_NATIVE
endif

# Bringup / contest mode markers for conditional compilation.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif

LDFLAGS = -nostdlib -nostartfiles -Wl,--build-id=none -T $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld $(ARCH_LDFLAGS)

# Source files
# ABI-specific source directories
ifeq ($(ABI),both)
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/linux/*.c) \
           $(wildcard $(KERNEL_DIR)/abi/native/*.c)
else
ABI_SRCS = $(wildcard $(KERNEL_DIR)/abi/$(ABI)/*.c)
endif

KERNEL_SRC = $(wildcard $(KERNEL_DIR)/*.c) \
             $(wildcard $(KERNEL_DIR)/core/*.c) \
             $(wildcard $(KERNEL_DIR)/mm/*.c) \
             $(wildcard $(KERNEL_DIR)/proc/*.c) \
             $(wildcard $(KERNEL_DIR)/fs/*.c) \
             $(wildcard $(KERNEL_DIR)/fs/vfs/*.c) \
             $(wildcard $(KERNEL_DIR)/ipc/*.c) \
             $(wildcard $(KERNEL_DIR)/net/*.c) \
             $(wildcard $(KERNEL_DIR)/bpf/*.c) \
             $(wildcard $(KERNEL_DIR)/drv/*.c) \
             $(ABI_SRCS) \
             $(wildcard $(KERNEL_DIR)/syscall/*.c) \
             $(wildcard $(KERNEL_DIR)/shell/*.c) \
             $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.c' | sort) \
             $(LWIP_SRC)

LWIPDIR := $(KERNEL_DIR)/external/lwip/src
LWIP_SRC = \
    $(LWIPDIR)/core/init.c \
    $(LWIPDIR)/core/def.c \
    $(LWIPDIR)/core/dns.c \
    $(LWIPDIR)/core/inet_chksum.c \
    $(LWIPDIR)/core/ip.c \
    $(LWIPDIR)/core/mem.c \
    $(LWIPDIR)/core/memp.c \
    $(LWIPDIR)/core/netif.c \
    $(LWIPDIR)/core/pbuf.c \
    $(LWIPDIR)/core/raw.c \
    $(LWIPDIR)/core/stats.c \
    $(LWIPDIR)/core/sys.c \
    $(LWIPDIR)/core/altcp.c \
    $(LWIPDIR)/core/altcp_alloc.c \
    $(LWIPDIR)/core/altcp_tcp.c \
    $(LWIPDIR)/core/tcp.c \
    $(LWIPDIR)/core/tcp_in.c \
    $(LWIPDIR)/core/tcp_out.c \
    $(LWIPDIR)/core/timeouts.c \
    $(LWIPDIR)/core/udp.c \
    $(LWIPDIR)/core/ipv4/acd.c \
    $(LWIPDIR)/core/ipv4/autoip.c \
    $(LWIPDIR)/core/ipv4/dhcp.c \
    $(LWIPDIR)/core/ipv4/etharp.c \
    $(LWIPDIR)/core/ipv4/icmp.c \
    $(LWIPDIR)/core/ipv4/igmp.c \
    $(LWIPDIR)/core/ipv4/ip4.c \
    $(LWIPDIR)/core/ipv4/ip4_addr.c \
    $(LWIPDIR)/core/ipv4/ip4_frag.c \
    $(LWIPDIR)/core/ipv6/dhcp6.c \
    $(LWIPDIR)/core/ipv6/ethip6.c \
    $(LWIPDIR)/core/ipv6/icmp6.c \
    $(LWIPDIR)/core/ipv6/inet6.c \
    $(LWIPDIR)/core/ipv6/ip6.c \
    $(LWIPDIR)/core/ipv6/ip6_addr.c \
    $(LWIPDIR)/core/ipv6/ip6_frag.c \
    $(LWIPDIR)/core/ipv6/mld6.c \
    $(LWIPDIR)/core/ipv6/nd6.c \
    $(LWIPDIR)/netif/ethernet.c

# Object files
KERNEL_OBJ = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRC))

# ASM sources
ASM_SRC = $(shell find $(KERNEL_DIR)/arch/$(ARCH) -type f -name '*.S' | sort)
ASM_OBJ = $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC))
DEP_FILES = $(KERNEL_OBJ:.o=.d) $(ASM_OBJ:.o=.d)

# Kernel image
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin

# ================================================================
# Targets
# ================================================================

.PHONY: all clean run-riscv64 run-loongarch64 run-arm64 debug-riscv64 debug-loongarch64 debug-arm64 \
        check-kernel-build check-user-build check-dev-build check-contest-build \
        check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup \
        check-riscv64-user check-loongarch64-user check-aarch64-user \
        smoke-riscv64 smoke-loongarch64 smoke-aarch64 smoke-abi-linux smoke-proc-a20 \
        FORCE \
        user_apps fs_img kernel-only dev-build contest-rv contest-la \
        eval-dev-build-rv eval-dev-build-la \
        extra-img extra-user-apps run-riscv64-extra run-loongarch64-extra run-arm64-extra \
        native-test-rv native-test-la native-test native-minimal-rv native-minimal-la native-minimal \
        eval eval-rv eval-la

FORCE:

$(BUILD_TIME_HDR):
	@mkdir -p $(dir $@)
	@printf '#ifndef A20_BUILD_UNIX_TIME\n#define A20_BUILD_UNIX_TIME %sULL\n#endif\n' "$$(date -u +%s)" > $@

# ----------------------------------------------------------------
# Competition build: produces kernel-rv, kernel-la, disk.img,
# disk-la.img (what the judge expects from `make all`).
# ----------------------------------------------------------------
all:
	$(MAKE) contest-rv
	$(MAKE) contest-la
	@echo "=== Competition build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

check-kernel-build: check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup

check-riscv64-bringup:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only

check-loongarch64-bringup:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-aarch64-bringup:
	$(MAKE) ARCH=aarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-user-build: check-riscv64-user check-loongarch64-user check-aarch64-user

check-riscv64-user:
	$(MAKE) -C user ARCH=riscv64 OPT="$(OPT)"

check-loongarch64-user:
	$(MAKE) -C user ARCH=loongarch64 OPT="$(OPT)"

check-aarch64-user:
	$(MAKE) -C user ARCH=aarch64 OPT="$(OPT)"

check-dev-build:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=0 dev-build

check-contest-build:
	$(MAKE) all

smoke-riscv64:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/riscv64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/riscv64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-riscv64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-riscv64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-riscv64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-loongarch64:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/loongarch64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-loongarch64 \
		-machine virt -m 1G -nographic -smp 1 \
		-kernel .kernel-build/loongarch64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-loongarch64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-loongarch64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-loongarch64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-aarch64:
	$(MAKE) ARCH=aarch64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/aarch64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-aarch64 \
		-machine virt -cpu cortex-a57 -m 1G -nographic -smp 1 \
		-global virtio-mmio.force-legacy=false \
		-kernel .kernel-build/aarch64-linux-bringup/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "smoke-aarch64: timeout reached; log saved to $$log"; \
	elif [ "$$status" -eq 0 ]; then \
		echo "smoke-aarch64: QEMU exited normally; log saved to $$log"; \
	else \
		echo "smoke-aarch64: QEMU failed with status $$status; tail of $$log:"; \
		tail -n 40 "$$log"; \
		exit "$$status"; \
	fi

smoke-abi-linux:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/abi-linux-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'syscall_smoke\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SYSCALL_SMOKE: PASS' "$$log"; then \
		echo "smoke-abi-linux: PASS; log saved to $$log"; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-abi-linux: timeout without PASS; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-abi-linux: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit "$$status"; \
	fi

smoke-proc-a20:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/proc-a20-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'cat /proc/a20/bcache\ncat /proc/a20/page_cache\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q '^valid_pages:' "$$log" && grep -q '^capacity:' "$$log"; then \
		echo "smoke-proc-a20: PASS; log saved to $$log"; \
	else \
		echo "smoke-proc-a20: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

contest-rv:
	@echo "--- Building RISC-V 64 (contest) ---"
	$(MAKE) ARCH=riscv64 _contest_build KERNEL_OUT=kernel-rv DISK_OUT=disk.img

contest-la:
	@echo "--- Building LoongArch 64 (contest) ---"
	$(MAKE) ARCH=loongarch64 _contest_build KERNEL_OUT=kernel-la DISK_OUT=disk-la.img

_reset_obj:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	$(MAKE) -C user clean

_contest_build: $(KERNEL_ELF) $(USER_BUILD_STAMP)
ifeq ($(ARCH), riscv64)
	$(MAKE) native-test-rv 2>/dev/null || true
else ifeq ($(ARCH), loongarch64)
	$(MAKE) native-test-la 2>/dev/null || true
endif
	$(MAKE) ARCH=$(ARCH) ABI=$(ABI) _contest_disk
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_contest_disk: $(USER_BUILD_STAMP)
	rm -f $(DISK_OUT)
	mkfs.fat -C -F 32 $(DISK_OUT) 131072
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(DISK_OUT) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(DISK_OUT) user/build/mksh ::/sh
	mcopy -o -i $(DISK_OUT) user/build/mksh ::/bash
	-mmd -i $(DISK_OUT) ::/etc >/dev/null 2>&1
	-mmd -i $(DISK_OUT) ::/lib >/dev/null 2>&1
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(DISK_OUT) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(DISK_OUT) - ::/etc/protocols
	mcopy -o -i $(DISK_OUT) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt
	mcopy -o -i $(DISK_OUT) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(DISK_OUT) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	@printf 'auto\n' | mcopy -o -i $(DISK_OUT) - ::/etc/contest-mode

# ----------------------------------------------------------------
# Development build (for `make run-riscv64` / `make run-loongarch64`)
# ----------------------------------------------------------------

dev-build: $(KERNEL_BIN) $(USER_BUILD_STAMP) $(FS_TEST_IMG) $(EXT4_IMG)
	@echo "Dev build complete: $(KERNEL_BIN), $(FAT32_IMG), $(EXT4_IMG)"

user_apps: $(USER_BUILD_STAMP)

$(USER_BUILD_STAMP): FORCE
	@set -e; \
	mkdir -p $(dir $@); \
	current=""; \
	if [ -f "$@" ]; then current=$$(cat "$@"); fi; \
	need_build=0; \
	need_clean=0; \
	if [ "$$current" != "$(USER_BUILD_ID)" ]; then \
		need_build=1; \
		need_clean=1; \
	elif [ ! -x user/build/init ] || [ ! -x user/build/mksh ]; then \
		need_build=1; \
	elif find user/build -maxdepth 1 -type f ! -name '.build-id' -newer "$@" \
		-print -quit | grep -q .; then \
		need_build=1; \
		need_clean=1; \
	elif find user/Makefile $(USER_BUILD_CHECK_DIRS) \
		\( -path '*/.git' -o -path 'user/build' -o -path 'user/external/musl/build-*' \) -prune -o \
		-type f -newer "$@" -print -quit | grep -q .; then \
		need_build=1; \
	fi; \
	if [ "$$need_build" -eq 1 ]; then \
		if [ "$$need_clean" -eq 1 ]; then $(MAKE) -C user clean; fi; \
		$(MAKE) -C user ARCH=$(ARCH) OPT="$(OPT)"; \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[USER] $(USER_BUILD_ID) up to date"; \
	fi

fs_img: $(FS_TEST_IMG)

$(FAT32_IMG): $(USER_BUILD_STAMP)
	@echo "Building FAT32 image..."
	@mkdir -p $(BUILD_DIR)
ifeq ($(ARCH), riscv64)
	$(MAKE) native-test-rv 2>/dev/null || true
else ifeq ($(ARCH), loongarch64)
	$(MAKE) native-test-la 2>/dev/null || true
endif
	dd if=/dev/zero of=$(FAT32_IMG) bs=1M count=$(FAT32_IMAGE_MB)
	mkfs.fat -F 32 $(FAT32_IMG)
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(FAT32_IMG) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(FAT32_IMG) user/build/mksh ::/sh
	mcopy -o -i $(FAT32_IMG) user/build/mksh ::/bash
	-mmd -i $(FAT32_IMG) ::/etc >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/lib >/dev/null 2>&1
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(FAT32_IMG) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(FAT32_IMG) - ::/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' | mcopy -o -i $(FAT32_IMG) - ::/etc/os-release
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i $(FAT32_IMG) - ::/test.txt
	mcopy -o -i $(FAT32_IMG) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt

$(FS_TEST_IMG): $(FAT32_IMG)
	cp $(FAT32_IMG) $(FS_TEST_IMG)

ext4_img_only: $(EXT4_IMG)

$(EXT4_IMG): $(USER_BUILD_STAMP)
	@echo "Building ext4 image..."
	@rm -rf $(EXT4_STAGING_DIR) && mkdir -p $(EXT4_STAGING_DIR)
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		cp "$$f" "$(EXT4_STAGING_DIR)/$$(basename "$$f")"; \
	done
	cp user/build/mksh $(EXT4_STAGING_DIR)/sh
	cp user/build/mksh $(EXT4_STAGING_DIR)/bash
	printf 'Hello from ext4!\nThis file is on the ext4 filesystem.\n' > $(EXT4_STAGING_DIR)/test.txt
	@mkdir -p $(EXT4_STAGING_DIR)/etc
	@printf '%s\n' $(PROTOCOLS_LINES) > $(EXT4_STAGING_DIR)/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' > $(EXT4_STAGING_DIR)/etc/os-release
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXT4_IMG) bs=1M count=$(EXT4_IMAGE_MB)
	mkfs.ext4 -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index -d $(EXT4_STAGING_DIR) $(EXT4_IMG)
	@rm -rf $(EXT4_STAGING_DIR)

ext4_img: $(USER_BUILD_STAMP) ext4_img_only
	cp $(EXT4_IMG) $(FS_TEST_IMG)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(CROSS_PREFIX)objcopy -O binary $< $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c Makefile | $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S Makefile
	@mkdir -p $(dir $@)
	$(CROSS_PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	rm -f kernel.elf kernel.bin fat32.img ext4.img
	rm -f kernel-rv kernel-la disk.img disk-la.img
	$(MAKE) -C user clean
	$(MAKE) -f user/extra.mk clean 2>/dev/null || true

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"

# ----------------------------------------------------------------
# Run targets (development mode)
# ----------------------------------------------------------------

run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _run_impl

run-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _run_impl

run-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _run_impl

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) dev-build
endif
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# --- Debug Targets ---

DEBUG_CFLAGS = $(filter-out -O0 -O1 -O2 -O3 -Os -Oz,$(CFLAGS)) -O0 -g -DDEBUG

debug-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) _debug_impl

debug-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) _debug_impl

debug-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) _debug_impl

_debug_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 OPT="-O0 -g -DDEBUG" kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) OPT="-O0 -g -DDEBUG" dev-build
endif
	@echo "Waiting for GDB connection on port 1234..."
	@echo "=========================================================="
	@echo "Please run in another terminal:"
	@echo "  gdb-multiarch $(KERNEL_ELF)"
	@echo "  (gdb) target remote :1234"
	@echo "=========================================================="
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s

# ----------------------------------------------------------------
# Extra packages (vim / git / gcc) on a separate ext4 disk
# ----------------------------------------------------------------

extra-user-apps:
	$(MAKE) -f user/extra.mk ARCH=$(ARCH) OPT="$(OPT)"

extra-img: extra-user-apps
	@echo "Building extra packages image..."
	@rm -rf $(EXTRA_STAGING_DIR) && mkdir -p $(EXTRA_STAGING_DIR)/bin
	@set -e; \
	for f in user/build/extra/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		cp "$$f" "$(EXTRA_STAGING_DIR)/bin/$$name"; \
	done; \
	[ -f user/build/fastfetch ] && cp user/build/fastfetch "$(EXTRA_STAGING_DIR)/bin/fastfetch" || true
	@set -e; \
	if [ -d user/build/extra/obj/$(ARCH)/gcc-install ]; then \
		cp -a user/build/extra/obj/$(ARCH)/gcc-install/libexec "$(EXTRA_STAGING_DIR)/libexec"; \
		cp -a user/build/extra/obj/$(ARCH)/gcc-install/lib "$(EXTRA_STAGING_DIR)/lib"; \
		for t in user/build/extra/obj/$(ARCH)/gcc-install/bin/*; do \
			[ -f "$$t" ] && cp "$$t" "$(EXTRA_STAGING_DIR)/bin/$$(basename $$t)"; \
		done; \
		mv "$(EXTRA_STAGING_DIR)/bin/gcc" "$(EXTRA_STAGING_DIR)/bin/gcc-real"; \
		printf '#!/bin/sh\nexec /usr/bin/gcc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/gcc"; \
		mv "$(EXTRA_STAGING_DIR)/bin/cc" "$(EXTRA_STAGING_DIR)/bin/cc-real"; \
		printf '#!/bin/sh\nexec /usr/bin/cc-real -fno-lto -fno-use-linker-plugin "$$@"\n' > "$(EXTRA_STAGING_DIR)/bin/cc"; \
	fi
	@MCM_LIB=user/external/musl-cross-make/output/riscv64-linux-musl/lib; \
	if [ -f "$$MCM_LIB/libc.so" ]; then \
		mkdir -p "$(EXTRA_STAGING_DIR)/lib"; \
		cp "$$MCM_LIB/libc.so" "$(EXTRA_STAGING_DIR)/lib/libc.so"; \
		ln -sf libc.so "$(EXTRA_STAGING_DIR)/lib/ld-musl-riscv64.so.1"; \
	fi
	@MCM_INC=user/external/musl-cross-make/output/riscv64-linux-musl/include; \
	if [ -d "$$MCM_INC" ]; then \
		mkdir -p "$(EXTRA_STAGING_DIR)/include"; \
		cp -a $$MCM_INC/* "$(EXTRA_STAGING_DIR)/include/"; \
		rm -rf "$(EXTRA_STAGING_DIR)/include/c++"; \
	fi
	@MCM_GCC_INC=user/external/musl-cross-make/output/lib/gcc/riscv64-linux-musl/14.2.0/include; \
	GCC_VER=17.0.0; \
	if [ -d "$$MCM_GCC_INC" ]; then \
		mkdir -p "$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/$$GCC_VER/include"; \
		cp -a $$MCM_GCC_INC/* "$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/$$GCC_VER/include/"; \
	fi
	@MCM_GCC_LIB=user/external/musl-cross-make/output/lib/gcc/riscv64-linux-musl/14.2.0; \
	GCC_VER=17.0.0; \
	for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o; do \
		[ -f "$$MCM_GCC_LIB/$$f" ] && cp "$$MCM_GCC_LIB/$$f" "$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/$$GCC_VER/$$f"; \
	done
	@GCC_SPECS_DIR="$(EXTRA_STAGING_DIR)/lib/gcc/riscv64-linux-musl/17.0.0"; \
	printf '*cc1_options:+ -fno-lto\n' > "$$GCC_SPECS_DIR/specs"
	@VIM_RT="$(EXTRA_STAGING_DIR)/share/vim/vim92"; \
	VIM_SRC=user/external/vim/runtime; \
	mkdir -p "$$VIM_RT"; \
	for f in defaults.vim filetype.vim ftoff.vim ftplugin.vim ftplugof.vim; do \
		[ -f "$$VIM_SRC/$$f" ] && cp "$$VIM_SRC/$$f" "$$VIM_RT/$$f"; \
	done; \
	for d in syntax indent; do \
		mkdir -p "$$VIM_RT/$$d"; \
		cp -a "$$VIM_SRC/$$d/"*.vim "$$VIM_RT/$$d/" 2>/dev/null || true; \
	done
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXTRA_IMG) bs=1M count=$(EXTRA_IMAGE_MB) 2>/dev/null
	mkfs.ext4 -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index \
		-d $(EXTRA_STAGING_DIR) $(EXTRA_IMG)
	@rm -rf $(EXTRA_STAGING_DIR)
	@echo "Extra image: $(EXTRA_IMG) ($(EXTRA_IMAGE_MB)MB)"

# Helper: QEMU flags for the extra disk (appended conditionally)
ifeq ($(ARCH), riscv64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
else ifeq ($(ARCH), loongarch64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-pci,drive=xextra
else ifeq ($(ARCH), aarch64)
EXTRA_QEMU_BLK = -drive file=$(EXTRA_IMG),if=none,format=raw,id=xextra -device virtio-blk-device,drive=xextra,bus=virtio-mmio-bus.5
endif

run-riscv64-extra:
	$(MAKE) ARCH=riscv64 BRINGUP=0 _run_extra_impl

run-loongarch64-extra:
	$(MAKE) ARCH=loongarch64 BRINGUP=0 _run_extra_impl

run-arm64-extra:
	$(MAKE) ARCH=aarch64 BRINGUP=0 _run_extra_impl

_run_extra_impl:
	$(MAKE) ARCH=$(ARCH) BRINGUP=0 dev-build
	$(MAKE) -C user ARCH=$(ARCH) fastfetch || true
	$(MAKE) ARCH=$(ARCH) EXTRA_IMG=$(EXTRA_IMG) extra-img
	$(QEMU) $(QEMU_FLAGS) $(EXTRA_QEMU_BLK) -kernel $(KERNEL_ELF)

NATIVE_TEST_DIR  := user/tests
NATIVE_LD        := user/liba20rt/a20-generic.ld
NATIVE_CRT0_RV   := user/liba20rt/crt0_rv64.S
NATIVE_CRT0_LA   := user/liba20rt/crt0_la64.S
NATIVE_SDK_SRC   := user/liba20rt/a20_malloc.c

define NATIVE_TEST_RECIPE
@mkdir -p user/build
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    user/tests/test_native_hello.c \
    -o $(4)
endef

define NATIVE_MINIMAL_RECIPE
@mkdir -p user/build
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    user/tests/test_native_minimal.c \
    -o $(4)
endef

native-test-rv:
	$(call NATIVE_TEST_RECIPE,riscv64-unknown-elf-gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/native-hello-rv)

native-test-la:
	$(call NATIVE_TEST_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/native-hello-la)

native-test: native-test-rv native-test-la

native-minimal-rv:
	$(call NATIVE_MINIMAL_RECIPE,riscv64-unknown-elf-gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/native-minimal-rv)
	@file user/build/native-minimal-rv

native-minimal-la:
	$(call NATIVE_MINIMAL_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/native-minimal-la)
	@file user/build/native-minimal-la

native-minimal: native-minimal-rv native-minimal-la

# ----------------------------------------------------------------
# Local evaluation: make eval-rv / make eval-la / make eval
# ----------------------------------------------------------------
EVAL_DIR   := .eval-state
EVAL_LOGS  := $(EVAL_DIR)/logs
EVAL_TIMEOUT ?= 3600
JUDGE_DIR  := judge

SDCARD_RV_URL := https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-rv.img.xz
SDCARD_LA_URL := https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-la.img.xz

$(EVAL_DIR) $(EVAL_LOGS):
	mkdir -p $@

# --- sdcard images (download if missing, prefer project-root copy) ---
$(EVAL_DIR)/sdcard-rv.img: | $(EVAL_DIR)
	@if [ -f sdcard-rv.img ]; then \
		ln -sf "$$(pwd)/sdcard-rv.img" $@; \
	elif [ -f $@ ]; then \
		echo "[eval] reusing cached sdcard-rv.img"; \
	else \
		echo "[eval] downloading sdcard-rv.img ..."; \
		wget -q -O $(EVAL_DIR)/sdcard-rv.img.xz $(SDCARD_RV_URL); \
		xz -dc $(EVAL_DIR)/sdcard-rv.img.xz > $@; \
	fi

$(EVAL_DIR)/sdcard-la.img: | $(EVAL_DIR)
	@if [ -f sdcard-la.img ]; then \
		ln -sf "$$(pwd)/sdcard-la.img" $@; \
	elif [ -f $@ ]; then \
		echo "[eval] reusing cached sdcard-la.img"; \
	else \
		echo "[eval] downloading sdcard-la.img ..."; \
		wget -q -O $(EVAL_DIR)/sdcard-la.img.xz $(SDCARD_LA_URL); \
		xz -dc $(EVAL_DIR)/sdcard-la.img.xz > $@; \
	fi

# --- eval dev-build targets (match run-*, add contest-mode + 128 MB) ---
EVAL_KERNEL_RV  = .kernel-build/riscv64-both-dev/kernel.elf
EVAL_FAT32_RV   = .kernel-build/riscv64-both-dev/fat32.img
EVAL_KERNEL_LA  = .kernel-build/loongarch64-both-dev/kernel.elf
EVAL_FAT32_LA   = .kernel-build/loongarch64-both-dev/fat32.img

eval-dev-build-rv:
	$(MAKE) ARCH=riscv64 FAT32_IMAGE_MB=128 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_RV) - ::/etc/contest-mode

eval-dev-build-la:
	$(MAKE) ARCH=loongarch64 FAT32_IMAGE_MB=128 dev-build
	@printf 'auto\n' | mcopy -o -i $(EVAL_FAT32_LA) - ::/etc/contest-mode

# --- QEMU launch ---
define RUN_QEMU_RV
	timeout --foreground $(EVAL_TIMEOUT) \
	qemu-system-riscv64 -machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-kernel $(EVAL_KERNEL_RV) \
		-drive 'file=$(EVAL_FAT32_RV),if=none,format=raw,id=x0' \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-drive 'file=$(EVAL_DIR)/sdcard-rv.img,if=none,format=raw,id=x1' \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		-no-reboot \
	2>&1 | tee $(EVAL_LOGS)/serial-rv.txt || true
endef

define RUN_QEMU_LA
	timeout --foreground $(EVAL_TIMEOUT) \
	qemu-system-loongarch64 -machine virt -m 1G -nographic -smp 1 \
		-kernel $(EVAL_KERNEL_LA) \
		-drive 'file=$(EVAL_FAT32_LA),if=none,format=raw,id=x0' \
		-device virtio-blk-pci,drive=x0 \
		$(NETDEV_USER) -device virtio-net-pci,netdev=net \
		-drive 'file=$(EVAL_DIR)/sdcard-la.img,if=none,format=raw,id=x1' \
		-device virtio-blk-pci,drive=x1 \
		-no-reboot \
	2>&1 | tee $(EVAL_LOGS)/serial-la.txt || true
endef

# --- Judge (inline Python) ---
define JUDGE_PY
import sys, os, re, json, subprocess

serial_path = sys.argv[1]
judge_dir   = sys.argv[2]
arch_label  = sys.argv[3]

with open(serial_path, "r", errors="ignore") as f:
    content = f.read()

judges = {}
for fname in os.listdir(judge_dir):
    if fname.startswith("judge_") and fname.endswith(".py"):
        name = fname[len("judge_"):-len(".py")]
        judges[name] = os.path.join(judge_dir, fname)

start_re  = re.compile(r"#### OS COMP TEST GROUP START ([a-zA-Z0-9_-]+) ####")
end_marker = "#### OS COMP TEST GROUP END"

total_pass = 0
total_all  = 0
group_results = []

lines = content.split("\n")
i = 0
while i < len(lines):
    m = start_re.search(lines[i])
    if m:
        group = m.group(1)
        collected = []
        i += 1
        while i < len(lines):
            if end_marker in lines[i]:
                break
            m2 = start_re.search(lines[i])
            if m2:
                collected = []
                group = m2.group(1)
                i += 1
                continue
            collected.append(lines[i])
            i += 1
        if group in judges:
            try:
                proc = subprocess.run(
                    [sys.executable, judges[group]],
                    input="\n".join(collected),
                    capture_output=True, text=True, timeout=30
                )
                results = json.loads(proc.stdout)
                if isinstance(results, list):
                    for r in results:
                        p = r.get("pass", 0)
                        a = r.get("all", 0)
                        s = r.get("score", p)
                        total_pass += s
                        total_all  += a
                        group_results.append(dict(group=group, name=r.get("name",""),
                                                  pass=p, all=a, score=s))
            except Exception as e:
                print(f"  JUDGE ERROR [{group}]: {e}", file=sys.stderr)
    i += 1

print(f"\n{'='*60}")
print(f"  {arch_label.upper()} RESULTS")
print(f"{'='*60}")

agg = {}
for r in sorted(group_results, key=lambda x: (x["group"], x["name"])):
    g = r["group"]
    if g not in agg:
        agg[g] = {"pass": 0, "all": 0, "score": 0, "count": 0}
    agg[g]["pass"]  += r["pass"]
    agg[g]["all"]   += r["all"]
    agg[g]["score"] += r["score"]
    agg[g]["count"] += 1

for g, d in sorted(agg.items()):
    print(f"  {g:30s}  score={d['score']:4d}/{d['all']:4d}  cases={d['count']}")

print(f"{'='*60}")
print(f"  TOTAL                         score={total_pass:4d}/{total_all:4d}")
print(f"{'='*60}")
if total_all > 0:
    print(f"  Score percentage: {100.0*total_pass/total_all:.1f}%")
else:
    print("  No test cases matched.")
endef

# --- Top-level eval targets ---
eval-rv: eval-dev-build-rv $(EVAL_DIR)/sdcard-rv.img | $(EVAL_LOGS)
	@echo "[eval] launching RISC-V QEMU (timeout=$(EVAL_TIMEOUT)s) ..."
	$(RUN_QEMU_RV)
	@echo "[eval] judging RISC-V output ..."
	@python3 -c "$$JUDGE_PY" "$(EVAL_LOGS)/serial-rv.txt" "$(JUDGE_DIR)" "rv"

eval-la: eval-dev-build-la $(EVAL_DIR)/sdcard-la.img | $(EVAL_LOGS)
	@echo "[eval] launching LoongArch QEMU (timeout=$(EVAL_TIMEOUT)s) ..."
	$(RUN_QEMU_LA)
	@echo "[eval] judging LoongArch output ..."
	@python3 -c "$$JUDGE_PY" "$(EVAL_LOGS)/serial-la.txt" "$(JUDGE_DIR)" "la"

eval: eval-rv eval-la
	@echo "[eval] complete"

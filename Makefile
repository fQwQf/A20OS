# A20OS Makefile

# Architecture selection
ARCH ?= riscv64
ABI ?= linux
MODE ?= release
BRINGUP ?= 0
CONTEST ?= 0
OPT ?= -O3

ifneq ($(ABI),linux)
$(error Unsupported ABI '$(ABI)'; supported ABI: linux)
endif

.DEFAULT_GOAL := all

# Directories
KERNEL_DIR = kernel
INCLUDE_DIR = $(KERNEL_DIR)/include
BUILD_VARIANT = $(ABI)-$(if $(filter 1,$(CONTEST)),contest,$(if $(filter 1,$(BRINGUP)),bringup,dev))
BUILD_DIR = .kernel-build/$(ARCH)-$(BUILD_VARIANT)
FAT32_IMG = $(BUILD_DIR)/fat32.img
EXT4_IMG = $(BUILD_DIR)/ext4.img
FS_TEST_IMG = $(BUILD_DIR)/fs_test.img
USER_BUILD_STAMP = user/build/.build-id
ARCH_INCLUDE_DIR = $(KERNEL_DIR)/arch/$(ARCH)/include
EXT4_STAGING_DIR = $(BUILD_DIR)/ext4-staging
BUILD_TIME_HDR = $(BUILD_DIR)/generated/build_time.h
FAT32_IMAGE_MB ?= 32
EXT4_IMAGE_MB ?= 32
CONTEST_DISK_MB ?= $(FAT32_IMAGE_MB)
USER_BUILD_ID = $(ARCH):$(CONTEST):$(OPT)
USER_BUILD_CHECK_DIRS = user/cmds user/contest_init user/init_common user/lib user/shell \
                        user/external/musl user/external/sbase user/external/mksh-cvs2git \
                        user/external/tlse
comma := ,
NET_HOSTFWD ?= hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
NETDEV_USER = -netdev user,id=net$(if $(strip $(NET_HOSTFWD)),$(comma)$(NET_HOSTFWD),)
SMOKE_TIMEOUT ?= 20s
SMOKE_INPUT_DELAY ?= 2
SMOKE_LOG_DIR ?= .kernel-build/smoke

# Compiler and tools
ifeq ($(ARCH), riscv64)
    CROSS_PREFIX = riscv64-unknown-elf-
    ARCH_CFLAGS = -march=rv64gc -mabi=lp64d -mcmodel=medany
    ARCH_LDFLAGS =
    QEMU = qemu-system-riscv64
    QEMU_FLAGS = -machine virt -m 512M -nographic -smp 1 -bios default -global virtio-mmio.force-legacy=false
else ifeq ($(ARCH), loongarch64)
    CROSS_PREFIX = loongarch64-linux-gnu-
    ARCH_CFLAGS = -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-loongarch64
    QEMU_FLAGS = -machine virt -m 512M -nographic -smp 1
else ifeq ($(ARCH), aarch64)
    CROSS_PREFIX = aarch64-linux-gnu-
    ARCH_CFLAGS = -march=armv8-a -mgeneral-regs-only -fno-pic -mcmodel=large -mno-outline-atomics
    ARCH_LDFLAGS = -static -no-pie
    QEMU = qemu-system-aarch64
    QEMU_FLAGS = -machine virt -cpu cortex-a57 -m 512M -nographic -smp 1 -global virtio-mmio.force-legacy=false
endif

# In bringup mode, boot kernel only (no fs image dependency).
ifneq ($(BRINGUP),1)
ifeq ($(ARCH), riscv64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += -drive file=$(EXT4_IMG),if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x2 -device virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2
endif

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x3 -device virtio-blk-device,drive=x3,bus=virtio-mmio-bus.3
endif

else ifeq ($(ARCH), loongarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0
QEMU_FLAGS += -drive file=$(EXT4_IMG),if=none,format=raw,id=x1 -device virtio-blk-pci,drive=x1
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-pci,netdev=net

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x2 -device virtio-blk-pci,drive=x2
endif

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x3 -device virtio-blk-pci,drive=x3
endif

else ifeq ($(ARCH), aarch64)
QEMU_FLAGS += -drive file=$(FAT32_IMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += -drive file=$(EXT4_IMG),if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
QEMU_FLAGS += $(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4

ifneq ($(wildcard sdcard-rv.img),)
QEMU_FLAGS += -drive file=sdcard-rv.img,if=none,format=raw,id=x2 -device virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2
endif

ifneq ($(wildcard sdcard-la.img),)
QEMU_FLAGS += -drive file=sdcard-la.img,if=none,format=raw,id=x3 -device virtio-blk-device,drive=x3,bus=virtio-mmio-bus.3
endif

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

# Bringup / contest mode markers for conditional compilation.
ifeq ($(BRINGUP),1)
CFLAGS += -DBRINGUP
endif
ifeq ($(CONTEST),1)
CFLAGS += -DCONTEST
endif

LDFLAGS = -nostdlib -nostartfiles -Wl,--build-id=none -T $(KERNEL_DIR)/arch/$(ARCH)/boot/ldscript.ld $(ARCH_LDFLAGS)

# Source files
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
             $(wildcard $(KERNEL_DIR)/abi/$(ABI)/*.c) \
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
        user_apps fs_img kernel-only dev-build contest-rv contest-la

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
	$(MAKE) -C user ARCH=riscv64 CONTEST=$(CONTEST) OPT="$(OPT)"

check-loongarch64-user:
	$(MAKE) -C user ARCH=loongarch64 CONTEST=$(CONTEST) OPT="$(OPT)"

check-aarch64-user:
	$(MAKE) -C user ARCH=aarch64 CONTEST=$(CONTEST) OPT="$(OPT)"

check-dev-build:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=0 CONTEST=0 dev-build

check-contest-build:
	$(MAKE) all

smoke-riscv64:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/riscv64-bringup.log"; \
	status=0; \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 512M -nographic -smp 1 -bios default \
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
		-machine virt -m 512M -nographic -smp 1 \
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
		-machine virt -cpu cortex-a57 -m 512M -nographic -smp 1 \
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
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 CONTEST=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/abi-linux-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'syscall_smoke\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 512M -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-drive file=.kernel-build/riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1 \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
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
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 CONTEST=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/proc-a20-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'cat /proc/a20/bcache\ncat /proc/a20/page_cache\npoweroff\n'; } | \
	timeout $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 512M -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-drive file=.kernel-build/riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1 \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
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
	$(MAKE) ARCH=riscv64 CONTEST=1 _contest_build KERNEL_OUT=kernel-rv DISK_OUT=disk.img

contest-la:
	@echo "--- Building LoongArch 64 (contest) ---"
	$(MAKE) ARCH=loongarch64 CONTEST=1 _contest_build KERNEL_OUT=kernel-la DISK_OUT=disk-la.img

_reset_obj:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	$(MAKE) -C user clean

_contest_build: $(KERNEL_ELF) $(USER_BUILD_STAMP) _contest_disk
	cp $(KERNEL_ELF) $(KERNEL_OUT)
	@echo "  -> $(KERNEL_OUT) + $(DISK_OUT)"

_contest_disk: $(USER_BUILD_STAMP)
	dd if=/dev/zero of=$(DISK_OUT) bs=1M count=$(CONTEST_DISK_MB) 2>/dev/null
	mkfs.fat -F 32 $(DISK_OUT)
	@set -e; \
	for f in user/build/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(DISK_OUT) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(DISK_OUT) user/build/mksh ::/sh
	mcopy -o -i $(DISK_OUT) user/build/mksh ::/bash
	-mmd -i $(DISK_OUT) ::/etc >/dev/null 2>&1
	@printf '%s\n' \
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
		'ipv6-opts 60 IPv6-Opts' | mcopy -o -i $(DISK_OUT) - ::/etc/protocols

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
		$(MAKE) -C user ARCH=$(ARCH) CONTEST=$(CONTEST) OPT="$(OPT)"; \
		printf '%s\n' '$(USER_BUILD_ID)' > "$@"; \
	else \
		echo "[USER] $(USER_BUILD_ID) up to date"; \
	fi

fs_img: $(FS_TEST_IMG)

$(FAT32_IMG): $(USER_BUILD_STAMP)
	@echo "Building FAT32 image..."
	@mkdir -p $(BUILD_DIR)
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
	@printf '%s\n' \
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
		'ipv6-opts 60 IPv6-Opts' | mcopy -o -i $(FAT32_IMG) - ::/etc/protocols
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i $(FAT32_IMG) - ::/test.txt

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

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"

# ----------------------------------------------------------------
# Run targets (development mode)
# ----------------------------------------------------------------

run-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _run_impl

run-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _run_impl

run-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _run_impl

_run_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 CONTEST=$(CONTEST) kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) dev-build
endif
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# --- Debug Targets ---

DEBUG_CFLAGS = $(filter-out -O0 -O1 -O2 -O3 -Os -Oz,$(CFLAGS)) -O0 -g -DDEBUG

debug-riscv64:
	$(MAKE) ARCH=riscv64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _debug_impl

debug-loongarch64:
	$(MAKE) ARCH=loongarch64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _debug_impl

debug-arm64:
	$(MAKE) ARCH=aarch64 BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) _debug_impl

_debug_impl:
ifeq ($(BRINGUP),1)
	$(MAKE) ARCH=$(ARCH) BRINGUP=1 CONTEST=$(CONTEST) CFLAGS="$(DEBUG_CFLAGS)" kernel-only
else
	$(MAKE) ARCH=$(ARCH) BRINGUP=$(BRINGUP) CONTEST=$(CONTEST) CFLAGS="$(DEBUG_CFLAGS)" dev-build
endif
	@echo "Waiting for GDB connection on port 1234..."
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -S -s

ARCH ?= riscv64
include ../tools/mk/llvm.mk
OPT ?= -O3
PROFILE ?= full
ENVELOPE_ACQUIRE_RACE_TEST ?= 0
SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST ?= 0
NPROC ?= $(or $(shell getconf _NPROCESSORS_ONLN 2>/dev/null),$(shell sysctl -n hw.logicalcpu 2>/dev/null),4)
NOMMU ?= 0
PYTHON ?= python3

.DEFAULT_GOAL := all

# This makefile only recurses into musl.  Keep the parent's jobserver flags,
# but do not leak command-line overrides such as ARCH and BUILD_DIR into musl.
override MAKEOVERRIDES =
MUSL_MAKE_JOBS := $(if $(filter -j%,$(MAKEFLAGS)),,-j$(NPROC))

SUPPORTED_ARCHES := riscv64 loongarch64 aarch64 x86_64 arm32 riscv32 ppc64le
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH '$(ARCH)'; expected one of: $(SUPPORTED_ARCHES))
endif

MUSL_VARIANT := $(if $(filter llvm,$(TOOLCHAIN)),llvm-rt-,)$(ARCH)$(if $(filter 1,$(NOMMU)),-nommu,)
BUILD_DIR ?= build/$(MUSL_VARIANT)
OBJ_DIR := $(BUILD_DIR)/obj
MUSL_BASE := $(CURDIR)/external/musl
MUSL_BUILD_WAS_OVERRIDDEN := $(if $(filter undefined,$(origin MUSL_BUILD)),0,1)
MUSL_CRT := $(MUSL_BUILD)/lib

ARCH_STAMP := $(BUILD_DIR)/.build-id
ARCH_BUILD_ID := upstream-busybox-1.37:$(ARCH):$(NOMMU):$(OPT):$(PROFILE):ENVELOPE_ACQUIRE_RACE_TEST=$(ENVELOPE_ACQUIRE_RACE_TEST)

.PHONY: arch-stamp-refresh
arch-stamp-refresh:
	@:

$(ARCH_STAMP): arch-stamp-refresh Makefile
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' '$(ARCH_BUILD_ID)' > $@.tmp
	@if [ -f $@ ] && diff -q $@ $@.tmp >/dev/null 2>&1; then \
		rm -f $@.tmp; \
	else \
		mv $@.tmp $@; \
		echo "[USER] initialized build variant $(ARCH_BUILD_ID) in $(BUILD_DIR)"; \
	fi

RISCV_ELF_PREFIX ?= $(if $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null),riscv64-unknown-elf-,$(if $(shell command -v riscv64-elf-gcc 2>/dev/null),riscv64-elf-,riscv64-unknown-elf-))
CROSS_COMPILE_riscv64 := $(RISCV_ELF_PREFIX)
CROSS_COMPILE_loongarch64 := loongarch64-linux-gnu-
CROSS_COMPILE_aarch64 := aarch64-linux-gnu-
CROSS_COMPILE_x86_64 := x86_64-linux-gnu-
CROSS_COMPILE_arm32 := $(if $(shell command -v arm-linux-gnueabihf-gcc 2>/dev/null),arm-linux-gnueabihf-,$(if $(shell command -v arm-none-eabi-gcc 2>/dev/null),arm-none-eabi-,arm-linux-gnueabihf-))
RISCV_ELF_RV32_MULTIDIR := $(if $(filter gcc,$(TOOLCHAIN)),$(shell if command -v $(RISCV_ELF_PREFIX)gcc >/dev/null 2>&1; then \
	$(RISCV_ELF_PREFIX)gcc -march=rv32imafdc -mabi=ilp32d -print-multi-directory 2>/dev/null; \
fi))
CROSS_COMPILE_riscv32 := $(if $(filter-out .,$(RISCV_ELF_RV32_MULTIDIR)),$(RISCV_ELF_PREFIX),$(if $(shell command -v riscv32-linux-gnu-gcc 2>/dev/null),riscv32-linux-gnu-,$(RISCV_ELF_PREFIX)))
CROSS_COMPILE_ppc64le := powerpc64le-linux-gnu-
CROSS_COMPILE ?= $(CROSS_COMPILE_$(ARCH))

MUSL_ARCH_riscv64 := riscv64
MUSL_ARCH_loongarch64 := loongarch64
MUSL_ARCH_aarch64 := aarch64
MUSL_ARCH_x86_64 := x86_64
MUSL_ARCH_arm32 := arm
MUSL_ARCH_riscv32 := riscv32
MUSL_ARCH_ppc64le := powerpc64
MUSL_ARCH := $(MUSL_ARCH_$(ARCH))

MUSL_TARGET_riscv64 := riscv64
MUSL_TARGET_loongarch64 := loongarch64-linux-gnu
MUSL_TARGET_aarch64 := aarch64-linux-gnu
MUSL_TARGET_x86_64 := x86_64-linux-gnu
MUSL_TARGET_arm32 := arm-linux-gnueabihf
MUSL_TARGET_riscv32 := riscv32-linux-gnu
MUSL_TARGET_ppc64le := powerpc64le-linux-gnu
MUSL_TARGET := $(MUSL_TARGET_$(ARCH))

ARCH_CFLAGS_riscv64 := -mabi=lp64d -march=rv64g -mcmodel=medany
ARCH_CFLAGS_loongarch64 := -mabi=lp64d -march=loongarch64 -mcmodel=normal -fno-pic -mstrict-align
ARCH_CFLAGS_aarch64 := -march=armv8-a -fno-pic -fno-tree-vectorize
ARCH_CFLAGS_x86_64 := -m64 -fno-pic -fno-pie -mno-red-zone
ARCH_CFLAGS_arm32 := -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -mno-unaligned-access
ARCH_CFLAGS_riscv32 := -march=rv32imafdc -mabi=ilp32d -mcmodel=medany
ARCH_CFLAGS_ppc64le := -m64 -mcpu=power8 -mtune=power8 -mlong-double-64 -fno-pic -mno-vsx -mno-altivec -fno-tree-vectorize
ARCH_CFLAGS := $(ARCH_CFLAGS_$(ARCH))

ARCH_LDFLAGS_riscv64 :=
ARCH_LDFLAGS_loongarch64 := -no-pie
ARCH_LDFLAGS_aarch64 := -no-pie
ARCH_LDFLAGS_x86_64 := -no-pie
ARCH_LDFLAGS_arm32 := -no-pie
ARCH_LDFLAGS_riscv32 := -no-pie -Wl,-m,elf32lriscv
ARCH_LDFLAGS_ppc64le := -no-pie
ARCH_LDFLAGS := $(ARCH_LDFLAGS_$(ARCH))

MUSL_CONFIG_CFLAGS_riscv64 := $(OPT) -mcmodel=medany -march=rv64g -mabi=lp64d
MUSL_CONFIG_CFLAGS_loongarch64 := $(OPT) -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static -mstrict-align
MUSL_CONFIG_CFLAGS_aarch64 := $(OPT) -fno-pic -static -fno-tree-vectorize
MUSL_CONFIG_CFLAGS_x86_64 := $(OPT) -m64 -fno-pic -fno-pie -mno-red-zone -static
MUSL_CONFIG_CFLAGS_arm32 := $(OPT) -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -mno-unaligned-access -static
MUSL_CONFIG_CFLAGS_riscv32 := $(OPT) -march=rv32imafdc -mabi=ilp32d -mcmodel=medany -static
MUSL_CONFIG_CFLAGS_ppc64le := $(OPT) -m64 -mcpu=power8 -mtune=power8 -mlong-double-64 -fno-pic -static -mno-vsx -mno-altivec -fno-tree-vectorize
MUSL_CONFIG_CFLAGS := $(MUSL_CONFIG_CFLAGS_$(ARCH)) -D__A20OS__

CCACHE ?= $(shell command -v ccache 2>/dev/null)
CCACHE_PREFIX := $(if $(CCACHE),$(CCACHE) ,)
CC := $(CCACHE_PREFIX)$(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib

# LLVM uses LLD and a separately built compiler-rt archive; never host runtimes.
ifeq ($(TOOLCHAIN),llvm)
CC := $(LLVM_CC) $(LLVM_LINK) -Wno-unused-command-line-argument
LD := $(LLVM_LD)
AR := $(LLVM_PREFIX)llvm-ar$(LLVM_SUFFIX)
RANLIB := $(LLVM_PREFIX)llvm-ranlib$(LLVM_SUFFIX)
OBJCOPY := $(LLVM_PREFIX)llvm-objcopy$(LLVM_SUFFIX)
ARCH_CFLAGS := $(filter-out -mstrict-align,$(ARCH_CFLAGS))
endif

MUSL_INC = -isystem $(MUSL_BUILD)/obj/include \
            -isystem $(MUSL_BASE)/include \
            -isystem $(MUSL_BASE)/arch/$(MUSL_ARCH) \
            -isystem $(MUSL_BASE)/arch/generic

# -MMD -MP makes the compiler emit a .d next to every object it builds.  Make
# otherwise sees only the .c file, so editing a header such as
# init_common/runtime_setup.h rebuilds nothing and silently ships a stale
# binary; -MP also emits empty rules for headers so a deleted one cannot wedge
# the build.  The .d files are read back by DEP_FILES at the end of this file,
# the same way the kernel side wires CFLAGS -MMD -MP into DEP_FILES.
CFLAGS = -Wall -Wextra $(OPT) -ffreestanding -nostdinc -MMD -MP $(MUSL_INC) \
          -Iinit_common -Icmds $(ARCH_CFLAGS)
ifeq ($(ENVELOPE_ACQUIRE_RACE_TEST),1)
CFLAGS += -DENVELOPE_ACQUIRE_RACE_TEST
endif
# Test-profile only: makes syscall_smoke arm the private
# prctl(0xA20FU) one-shot clone failure so the kernel's fork rollback runs
# against a real partial publication.
ifeq ($(SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST),1)
CFLAGS += -DSYSCALL_SMOKE_TASK_CONSTRUCTION_TEST
endif
# Test-profile only: tells trap_storm_test which address the kernel's injected
# "repaired but re-trapping" fault is keyed on.
ifeq ($(TRAP_STORM_TEST),1)
CFLAGS += -DCONFIG_TRAP_STORM_TEST
endif
ifeq ($(FUTEX_COW_WINDOW_TEST),1)
CFLAGS += -DCONFIG_FUTEX_COW_WINDOW_TEST
endif
LDFLAGS := -static -nostdlib -Wl,--build-id=none -Wl,-Ttext-segment=0x10000 $(ARCH_LDFLAGS)

ifeq ($(NOMMU),1)
ifeq ($(ARCH),riscv64)
CROSS_COMPILE := riscv64-linux-gnu-
CC := $(CCACHE_PREFIX)$(CROSS_COMPILE)gcc
endif
ifeq ($(ARCH),riscv32)
CROSS_COMPILE := riscv64-linux-gnu-
CC := $(CCACHE_PREFIX)$(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib
endif
# Strip conflicting flags for NOMMU
ARCH_LDFLAGS := $(filter-out -no-pie,$(ARCH_LDFLAGS))
ARCH_CFLAGS := $(filter-out -fno-pic -fno-pie,$(ARCH_CFLAGS))
MUSL_CONFIG_CFLAGS := $(filter-out -fno-pic -fno-pie,$(MUSL_CONFIG_CFLAGS))

LDFLAGS := -static-pie -nostdlib -Wl,--build-id=none -Wl,-z,notext -Wl,--no-dynamic-linker $(ARCH_LDFLAGS) $(ARCH_CFLAGS)
CFLAGS += -fPIE -Dfork=vfork
MUSL_CONFIG_CFLAGS += -fPIE
endif

ifeq ($(TOOLCHAIN),llvm)
CC := $(LLVM_CC) $(LLVM_LINK) -Wno-unused-command-line-argument
LD := $(LLVM_LD)
AR := $(LLVM_PREFIX)llvm-ar$(LLVM_SUFFIX)
RANLIB := $(LLVM_PREFIX)llvm-ranlib$(LLVM_SUFFIX)
OBJCOPY := $(LLVM_PREFIX)llvm-objcopy$(LLVM_SUFFIX)
endif

ifeq ($(TOOLCHAIN),llvm)
# Kept as an internal variable name for existing link recipes only.
LIBGCC := $(LLVM_USER_BUILTINS)
MUSL_CONFIG_LDFLAGS := $(LLVM_LINK)
llvm_comma := ,
LDFLAGS := $(subst -Wl$(llvm_comma)-Ttext-segment=0x10000,-Wl$(llvm_comma)--image-base=0x10000,$(LDFLAGS))
else
LIBGCC_DEFAULT := $(shell $(CROSS_COMPILE)gcc $(ARCH_CFLAGS) -print-libgcc-file-name 2>/dev/null)
ifeq ($(ARCH),riscv64)
# Keep libgcc on the same ABI as ARCH_CFLAGS.  In particular, the current
# riscv64 userspace is lp64d; forcing an lp64 archive makes the final link mix
# double-float objects with a soft-float runtime.
LIBGCC := $(LIBGCC_DEFAULT)
MUSL_CONFIG_LDFLAGS := -L$(dir $(LIBGCC))
else ifeq ($(ARCH),riscv32)
# NOMMU builds use the Linux cross compiler for its sysroot headers and crt,
# but this host's riscv64-linux-gnu GCC has no RV32 multilib. Select libgcc
# from the matching bare-metal RV32IMAFDC/ILP32D multilib instead. Keep an
# explicit command-line LIBGCC override for toolchain setups with their own
# compatible runtime, and validate the actual archive members before linking.
RISCV32_LIBGCC_CC ?= $(RISCV_ELF_PREFIX)gcc
RISCV32_LIBGCC_AR ?= $(RISCV_ELF_PREFIX)ar
ifeq ($(origin LIBGCC),undefined)
LIBGCC := $(shell $(RISCV32_LIBGCC_CC) $(ARCH_CFLAGS_riscv32) -print-libgcc-file-name 2>/dev/null)
endif
RISCV32_LIBGCC_VALID := $(shell $(PYTHON) ../tools/check-riscv-libgcc.py --ar $(RISCV32_LIBGCC_AR) "$(LIBGCC)" >/dev/null 2>&1; echo $$?)
ifneq ($(RISCV32_LIBGCC_VALID),0)
$(error RV32 requires an ELF32 RISC-V ILP32D LIBGCC archive; selected '$(LIBGCC)'. Set LIBGCC to a matching multilib archive)
endif
MUSL_CONFIG_LDFLAGS := -L$(dir $(LIBGCC))
else
LIBGCC := $(LIBGCC_DEFAULT)
MUSL_CONFIG_LDFLAGS :=
endif

endif

# Each user build gets an immutable musl output directory keyed by the source
# identity and the complete target/compiler configuration. An explicit
# MUSL_BUILD override is allowed for a pre-provisioned matching tree, but is
# never cleaned or repurposed by this makefile.
USERLAND_REQUIRED_INPUTS := external/musl/configure
USERLAND_MISSING_INPUTS := $(filter-out $(wildcard $(USERLAND_REQUIRED_INPUTS)),$(USERLAND_REQUIRED_INPUTS))
ifneq ($(strip $(USERLAND_MISSING_INPUTS)),)
$(error Missing userland submodules: $(USERLAND_MISSING_INPUTS). Run git submodule update --init --recursive from the repository root)
endif

MUSL_BUILD_KEY := $(shell $(PYTHON) ../tools/packaging/musl_build_identity.py \
	--source "$(MUSL_BASE)" --target "$(MUSL_TARGET)" --compiler "$(if $(filter llvm,$(TOOLCHAIN)),$(CLANG) --target=$(LLVM_TARGET_$(ARCH)),$(CROSS_COMPILE)gcc)" \
	--cflags="$(MUSL_CONFIG_CFLAGS)" --ldflags="$(MUSL_CONFIG_LDFLAGS)" 2>/dev/null)
ifeq ($(strip $(MUSL_BUILD_KEY)),)
$(error Could not compute musl source/build identity)
endif
ifeq ($(MUSL_BUILD_WAS_OVERRIDDEN),0)
MUSL_BUILD := $(abspath $(BUILD_DIR)/.musl/$(MUSL_BUILD_KEY))
endif
MUSL_BUILD := $(abspath $(MUSL_BUILD))
MUSL_CRT := $(MUSL_BUILD)/lib

# musl's makefiles do not treat config.mak as an input to every object.  Keep
# our own configuration signature so ABI, compiler, or flag changes cannot
# silently reuse incompatible CRT and libc objects after a rebase/toolchain
# upgrade.
MUSL_BUILD_STAMP := $(MUSL_BUILD)/.a20os-build-id
MUSL_CC_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
MUSL_CC_VERSION := $(shell $(CC) -dumpfullversion -dumpversion 2>/dev/null)
MUSL_BUILD_ID := key=$(MUSL_BUILD_KEY);target=$(MUSL_TARGET);cc=$(CC);cc-target=$(MUSL_CC_TARGET);cc-version=$(MUSL_CC_VERSION);cflags=$(MUSL_CONFIG_CFLAGS);ldflags=$(MUSL_CONFIG_LDFLAGS)

ifeq ($(NOMMU),1)
CRT_START := $(MUSL_CRT)/rcrt1.o $(MUSL_CRT)/crti.o
else
CRT_START := $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o
endif
CRT_END := $(MUSL_CRT)/crtn.o
LIBC := $(MUSL_CRT)/libc.a $(LIBGCC)

# ARM libgcc references libc symbols (e.g. raise); AArch64 libgcc's
# lse-init.o likewise pulls in __getauxval.  Allow circular resolution for
# both ARM ABIs.
ifneq ($(filter arm32 aarch64,$(ARCH)),)
ifeq ($(TOOLCHAIN),gcc)
LIBC := -Wl,--start-group $(MUSL_CRT)/libc.a $(LIBGCC) -Wl,--end-group
endif
endif

MUSL_CHECK_FILES := $(MUSL_CRT)/crt1.o \
                    $(MUSL_CRT)/crti.o \
                    $(MUSL_CRT)/crtn.o \
                    $(MUSL_CRT)/libc.a \
                    $(MUSL_BUILD)/obj/include/bits/alltypes.h \
                    $(MUSL_BUILD)/obj/include/bits/syscall.h \
                    $(MUSL_BUILD_STAMP)

# Common UNIX tools come from upstream BusyBox, including the download applet.
UPSTREAM_APPLETS := basename cat chgrp chmod chown chroot cksum cmp comm cp cut date dd \
    dirname du echo env expand expr false find fold grep head hostname id install kill \
    link ln logger logname ls mkdir mkfifo mknod mktemp mv nice nl nohup od paste \
    printenv printf pwd readlink rev rm rmdir sed seq setsid sleep sort split strings \
    sync tail tar tee test time touch tr true tty uname unexpand uniq unlink \
    uudecode uuencode wc which whoami xargs yes mount umount ps free dmesg \
    reboot poweroff wget
LOCAL_CMD_SRCS := $(filter-out $(addprefix cmds/core/,$(addsuffix .c,$(UPSTREAM_APPLETS))) cmds/net/wget.c,$(wildcard cmds/*/*.c))
ARCH_CMD_SRCS := $(wildcard arch/$(ARCH)/cmds/*.c)
LOCAL_CMD_NAMES := $(basename $(notdir $(LOCAL_CMD_SRCS)))
BUSYBOX_APPLETS := $(filter-out $(LOCAL_CMD_NAMES),$(UPSTREAM_APPLETS))
BUSYBOX_BIN := $(BUILD_DIR)/busybox
BUSYBOX_TOOL_TARGETS := $(addprefix $(BUILD_DIR)/,$(BUSYBOX_APPLETS))
BUSYBOX_STAGE_STAMP := $(BUILD_DIR)/.busybox-applets
BUSYBOX_JOBS ?= 4

LOCAL_CMD_OBJS := $(patsubst cmds/%.c,$(OBJ_DIR)/cmds/%.o,$(LOCAL_CMD_SRCS))
LOCAL_CMDS := $(addprefix $(BUILD_DIR)/,$(LOCAL_CMD_NAMES) $(basename $(notdir $(ARCH_CMD_SRCS))))
ARCH_CMD_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(ARCH_CMD_SRCS))

# The hypervisor guest loader, shared by the two programs that boot a guest:
# /hyp_boot (the fixed-parameter smoke gate) and /hypvm (the same boot with its
# parameters on the command line).  It lives one level deeper than
# cmds/<dir>/<file>.c on purpose -- LOCAL_CMD_SRCS above is cmds/*/*.c, so the
# same code at cmds/core/hyp_guest.c would be built into a third binary called
# hyp_guest; a subdirectory is what keeps it a module.  The generic object rule
# ($(OBJ_DIR)/cmds/%.o: cmds/%.c) still compiles it, and the two commands get
# it as an extra link prerequisite at their rules below.  Listed in
# USER_INTERMEDIATE_TARGETS so its .d file joins DEP_FILES and a header edit
# under cmds/core/hyp/ rebuilds both commands.
HYP_GUEST_OBJ := $(OBJ_DIR)/cmds/core/hyp/hyp_guest.o

FF_DIR := external/apps/fastfetch
FF_GEN := $(OBJ_DIR)/fastfetch/gen
FF_MAIN := $(FF_DIR)/src/fastfetch.c
FF_MAIN_OBJ := $(OBJ_DIR)/fastfetch/main.o

FF_COMMON_SRCS := \
	src/common/impl/commandoption.c \
	src/common/impl/duration.c \
	src/common/impl/font.c \
	src/common/impl/format.c \
	src/common/impl/frequency.c \
	src/common/impl/genconfig.c \
	src/common/impl/init.c \
	src/common/impl/jsonconfig.c \
	src/common/impl/library.c \
	src/common/impl/lua.c \
	src/common/impl/netif.c \
	src/common/impl/networking_common.c \
	src/common/impl/option.c \
	src/common/impl/parsing.c \
	src/common/impl/percent.c \
	src/common/impl/printing.c \
	src/common/impl/properties.c \
	src/common/impl/settings.c \
	src/common/impl/size.c \
	src/common/impl/temps.c \
	src/common/impl/time.c \
	src/common/impl/edidHelper.c \
	src/common/impl/base64.c \
	src/common/impl/FFlist.c \
	src/common/impl/FFstrbuf.c \
	src/common/impl/path.c \
	src/common/impl/FFPlatform.c \
	src/common/impl/smbios.c \
	src/common/impl/strutil.c \
	src/common/impl/dbus.c \
	src/common/impl/io_unix.c \
	src/common/impl/netif_linux.c \
	src/common/impl/networking_linux.c \
	src/common/impl/processing_linux.c \
	src/common/impl/FFPlatform_unix.c \
	src/common/impl/binary_linux.c \
	src/common/impl/kmod_linux.c

FF_DETECT_SRCS := \
	src/detection/bluetoothradio/bluetoothradio.c \
	src/detection/bootmgr/bootmgr.c \
	src/detection/chassis/chassis.c \
	src/detection/codec/codec.c \
	src/detection/codec/codec_vulkan.c \
	src/detection/cpu/cpu.c \
	src/detection/cpu/cpu_x86.c \
	src/detection/cpuusage/cpuusage.c \
	src/detection/command/command.c \
	src/detection/disk/disk.c \
	src/detection/diskio/diskio.c \
	src/detection/displayserver/displayserver.c \
	src/detection/editor/editor.c \
	src/detection/font/font.c \
	src/detection/gpu/gpu.c \
	src/detection/host/host_mac.c \
	src/detection/media/media.c \
	src/detection/netio/netio.c \
	src/detection/opencl/opencl.c \
	src/detection/opengl/opengl_shared.c \
	src/detection/os/os.c \
	src/detection/packages/packages.c \
	src/detection/physicalmemory/physicalmemory.c \
	src/detection/publicip/publicip.c \
	src/detection/terminaltheme/terminaltheme.c \
	src/detection/terminalfont/terminalfont.c \
	src/detection/terminalshell/terminalshell.c \
	src/detection/version/version.c \
	src/detection/vulkan/vulkan.c \
	src/detection/weather/weather.c \
	src/detection/zpool/zpool.c

FF_LINUX_DETECT_SRCS := \
	src/detection/battery/battery_linux.c \
	src/detection/bios/bios_linux.c \
	src/detection/board/board_linux.c \
	src/detection/bootmgr/bootmgr_linux.c \
	src/detection/brightness/brightness_linux.c \
	src/detection/btrfs/btrfs_linux.c \
	src/detection/chassis/chassis_linux.c \
	src/detection/codec/codec_linux.c \
	src/detection/cpu/cpu_linux.c \
	src/detection/cpucache/cpucache_linux.c \
	src/detection/cpuusage/cpuusage_linux.c \
	src/detection/cursor/cursor_linux.c \
	src/detection/bluetooth/bluetooth_linux.c \
	src/detection/bluetoothradio/bluetoothradio_linux.c \
	src/detection/disk/disk_linux.c \
	src/detection/dns/dns_linux.c \
	src/detection/physicaldisk/physicaldisk_linux.c \
	src/detection/physicalmemory/physicalmemory_linux.c \
	src/detection/diskio/diskio_linux.c \
	src/detection/displayserver/linux/displayserver_linux.c \
	src/detection/displayserver/linux/common.c \
	src/detection/displayserver/linux/drm.c \
	src/detection/displayserver/linux/wayland/wayland.c \
	src/detection/displayserver/linux/wayland/global-output.c \
	src/detection/displayserver/linux/wayland/wl-output-protocol.c \
	src/detection/displayserver/linux/wayland/wp-color-management-v1-protocol.c \
	src/detection/displayserver/linux/wayland/kde-output.c \
	src/detection/displayserver/linux/wayland/kde-output-device-v2-protocol.c \
	src/detection/displayserver/linux/wayland/xdg-output-unstable-v1-protocol.c \
	src/detection/displayserver/linux/wmde.c \
	src/detection/displayserver/linux/xcb.c \
	src/detection/displayserver/linux/xlib.c \
	src/detection/font/font_linux.c \
	src/detection/gpu/gpu_linux.c \
	src/detection/gpu/gpu_drm.c \
	src/detection/gpu/gpu_pci.c \
	$(if $(filter x86_64 aarch64,$(ARCH)),src/detection/gpu/gpu_windows.c) \
	src/detection/gpu/gpu_nvidia.c \
	src/detection/gpu/gpu_mthreads.c \
	src/detection/gtk_qt/gtk.c \
	src/detection/host/host_linux.c \
	src/detection/icons/icons_linux.c \
	src/detection/initsystem/initsystem_linux.c \
	src/detection/keyboard/keyboard_linux.c \
	src/detection/libc/libc_linux.c \
	src/detection/lm/lm_linux.c \
	src/detection/loadavg/loadavg_linux.c \
	src/detection/locale/locale_linux.c \
	src/detection/localip/localip_linux.c \
	src/detection/gamepad/gamepad_linux.c \
	src/detection/media/media_linux.c \
	src/detection/memory/memory_linux.c \
	src/detection/mouse/mouse_linux.c \
	src/detection/netio/netio_linux.c \
	src/detection/opengl/opengl_linux.c \
	src/detection/os/os_linux.c \
	src/detection/packages/packages_linux.c \
	src/detection/packages/packages_nix.c \
	src/detection/poweradapter/poweradapter_linux.c \
	src/detection/processes/processes_linux.c \
	src/detection/gtk_qt/qt.c \
	src/detection/sound/sound_linux.c \
	src/detection/swap/swap_linux.c \
	src/detection/terminalfont/terminalfont_linux.c \
	src/detection/terminalshell/terminalshell_linux.c \
	src/detection/terminalsize/terminalsize_linux.c \
	src/detection/theme/theme_linux.c \
	src/detection/top/top.c \
	src/detection/top/top_linux.c \
	src/detection/tpm/tpm_linux.c \
	src/detection/uptime/uptime_linux.c \
	src/detection/users/users_linux.c \
	src/detection/wallpaper/wallpaper_linux.c \
	src/detection/wifi/wifi_linux.c \
	src/detection/wm/wm_linux.c \
	src/detection/de/de_linux.c \
	src/detection/wmtheme/wmtheme_linux.c \
	src/detection/camera/camera_linux.c

FF_LOGO_SRCS := \
	src/logo/builtin.c \
	src/logo/image/im6.c \
	src/logo/image/im7.c \
	src/logo/image/image.c \
	src/logo/logo.c

FF_MODULES_SRCS := \
	src/modules/battery/battery.c \
	src/modules/bios/bios.c \
	src/modules/bluetooth/bluetooth.c \
	src/modules/bluetoothradio/bluetoothradio.c \
	src/modules/board/board.c \
	src/modules/bootmgr/bootmgr.c \
	src/modules/brightness/brightness.c \
	src/modules/break/break.c \
	src/modules/btrfs/btrfs.c \
	src/modules/camera/camera.c \
	src/modules/chassis/chassis.c \
	src/modules/codec/codec.c \
	src/modules/colors/colors.c \
	src/modules/cpu/cpu.c \
	src/modules/cpucache/cpucache.c \
	src/modules/cpuusage/cpuusage.c \
	src/modules/cursor/cursor.c \
	src/modules/custom/custom.c \
	src/modules/command/command.c \
	src/modules/datetime/datetime.c \
	src/modules/de/de.c \
	src/modules/disk/disk.c \
	src/modules/diskio/diskio.c \
	src/modules/dns/dns.c \
	src/modules/editor/editor.c \
	src/modules/font/font.c \
	src/modules/gpu/gpu.c \
	src/modules/host/host.c \
	src/modules/icons/icons.c \
	src/modules/initsystem/initsystem.c \
	src/modules/gamepad/gamepad.c \
	src/modules/kernel/kernel.c \
	src/modules/keyboard/keyboard.c \
	src/modules/lm/lm.c \
	src/modules/loadavg/loadavg.c \
	src/modules/locale/locale.c \
	src/modules/localip/localip.c \
	src/modules/logo/logo.c \
	src/modules/memory/memory.c \
	src/modules/monitor/monitor.c \
	src/modules/netio/netio.c \
	src/modules/opencl/opencl.c \
	src/modules/opengl/opengl.c \
	src/modules/os/os.c \
	src/modules/packages/packages.c \
	src/modules/physicaldisk/physicaldisk.c \
	src/modules/physicalmemory/physicalmemory.c \
	src/modules/processes/processes.c \
	src/modules/player/player.c \
	src/modules/poweradapter/poweradapter.c \
	src/modules/publicip/publicip.c \
	src/modules/display/display.c \
	src/modules/separator/separator.c \
	src/modules/shell/shell.c \
	src/modules/sound/sound.c \
	src/modules/swap/swap.c \
	src/modules/media/media.c \
	src/modules/mouse/mouse.c \
	src/modules/terminal/terminal.c \
	src/modules/terminaltheme/terminaltheme.c \
	src/modules/terminalfont/terminalfont.c \
	src/modules/terminalsize/terminalsize.c \
	src/modules/theme/theme.c \
	src/modules/title/title.c \
	src/modules/top/top.c \
	src/modules/tpm/tpm.c \
	src/modules/uptime/uptime.c \
	src/modules/users/users.c \
	src/modules/version/version.c \
	src/modules/vulkan/vulkan.c \
	src/modules/wallpaper/wallpaper.c \
	src/modules/weather/weather.c \
	src/modules/wifi/wifi.c \
	src/modules/wm/wm.c \
	src/modules/wmtheme/wmtheme.c \
	src/modules/zpool/zpool.c \
	src/modules/modules.c

FF_OPTS_SRCS := \
	src/options/display.c \
	src/options/logo.c \
	src/options/general.c

FF_3RDPARTY_SRCS := \
	src/3rdparty/yyjson/yyjson.c

FF_OPTIONAL_SRCS := \
	src/common/impl/wcwidth.c \
	src/common/impl/memrchr.c

FF_ALL_LIB_SRCS := $(FF_COMMON_SRCS) $(FF_DETECT_SRCS) $(FF_LINUX_DETECT_SRCS) \
                   $(FF_LOGO_SRCS) $(FF_MODULES_SRCS) $(FF_OPTS_SRCS) $(FF_3RDPARTY_SRCS) \
                   $(FF_OPTIONAL_SRCS)
FF_LIB_OBJS := $(patsubst %.c,$(OBJ_DIR)/fastfetch/%.o,$(FF_ALL_LIB_SRCS))
FF_LIB := $(OBJ_DIR)/fastfetch/libfastfetch.a

# glibc/kernel uapi headers live under /usr/<triple>/include for Debian/Ubuntu
# cross toolchains, but under /usr/include/<triple> for native multiarch.  Pick
# whichever exists on this host so the build also works when the host arch
# differs from the target (e.g. building riscv64 on an aarch64 machine).
FF_LINUX_UAPI_ROOT ?=
FF_LINUX_INC_DIR = $(or $(firstword $(wildcard \
                    $(if $(FF_LINUX_UAPI_ROOT),$(FF_LINUX_UAPI_ROOT)/$(1)/include) \
                    /usr/include/$(1) /usr/$(1)/include /usr/$(1)/sysroot/usr/include)),\
                    /usr/include/$(1))
FF_LINUX_INC_riscv64 := -isystem $(call FF_LINUX_INC_DIR,riscv64-linux-gnu)
# Linux UAPI headers are architecture-generic for the fastfetch uses here, so
# riscv32 shares the installed riscv64 header set; the target compiler still
# supplies the correct 32-bit ABI types and predefined macros.
FF_LINUX_INC_riscv32 := -isystem $(call FF_LINUX_INC_DIR,riscv64-linux-gnu)
FF_LINUX_INC_loongarch64 := -isystem $(call FF_LINUX_INC_DIR,loongarch64-linux-gnu)
FF_LINUX_INC_aarch64 := -isystem $(call FF_LINUX_INC_DIR,aarch64-linux-gnu)
FF_LINUX_INC_arm32 := -isystem $(call FF_LINUX_INC_DIR,arm-linux-gnueabihf)
FF_LINUX_INC_x86_64 := -isystem $(call FF_LINUX_INC_DIR,x86_64-linux-gnu)
FF_LINUX_INC_ppc64le := -isystem $(call FF_LINUX_INC_DIR,powerpc64le-linux-gnu)
FF_LINUX_INC := $(FF_LINUX_INC_$(ARCH)) \
                -isystem $(shell $(CC) -print-file-name=include)

FF_CFLAGS := $(filter-out -Wconversion,$(CFLAGS)) \
             -std=gnu23 \
             -I$(FF_DIR)/src -I$(FF_GEN) \
             -isystem /usr/include $(FF_LINUX_INC) \
             -include wchar.h \
             -D_GNU_SOURCE -D_XOPEN_SOURCE -D_ATFILE_SOURCE -D__linux__ \
             -D__STDC_WANT_LIB_EXT1__ -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
             -DFF_DISABLE_DLOPEN=1 \
             -DFF_HAVE_NO_GL=1 \
             -DFF_ENABLE_WCWIDTH=1 \
             -DFASTFETCH_TARGET_BINARY_NAME=fastfetch \
             -DFF_PACKAGES_DISABLE_LIST=FF_PACKAGES_FLAG_NONE \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
             -Wno-unused-variable -Wno-unused-but-set-variable -Wno-missing-field-initializers \
             -Wno-maybe-uninitialized -Wno-implicit-fallthrough -Wno-stringop-truncation \
             -Wno-type-limits -Wno-override-init -Wno-int-conversion -Wno-compat-pointer-types \
             -Wno-incompatible-pointer-types -fno-strict-aliasing -Wno-error

FF_CMD := $(BUILD_DIR)/fastfetch

INIT_COMMON_SRCS := $(wildcard init_common/*.c)
INIT_COMMON_OBJS := $(patsubst init_common/%.c,$(OBJ_DIR)/init_common/%.o,$(INIT_COMMON_SRCS))

INIT_MAIN_SRC := init.c
INIT_EXTRA_OBJS := $(INIT_COMMON_OBJS)
INIT_OBJ := $(OBJ_DIR)/init.o

USER_INTERMEDIATE_TARGETS := $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(LOCAL_CMD_OBJS) $(ARCH_CMD_OBJS) \
    $(FF_MAIN_OBJ) $(FF_LIB_OBJS) $(FF_LIB) $(HYP_GUEST_OBJ) $(OBJ_DIR)/crypto/md5.o

ifneq ($(wildcard $(FF_DIR)/src/fastfetch.c),)
FF_AVAILABLE := 1
FASTFETCH_TARGET := $(FF_CMD)
else
FF_AVAILABLE :=
FASTFETCH_TARGET :=
endif

all: build_dir $(BUILD_DIR)/init $(BUILD_DIR)/sh $(BUSYBOX_TOOL_TARGETS) $(LOCAL_CMDS) $(FASTFETCH_TARGET)

fastfetch: build_dir $(FF_CMD)

build_dir: $(ARCH_STAMP)
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR)

# GNU make executes the whole recursive recipe under -n. Check only compact
# flags (not long options like --no-print-directory) before any lock or write.
musl_prepare:
	@case '$(filter-out --%,$(firstword $(MAKEFLAGS)))' in *n*) exit 0 ;; esac; \
	set -e; \
	lock_file="$(MUSL_BUILD).lock"; \
	if command -v flock >/dev/null 2>&1; then \
		mkdir -p "$(dir $(MUSL_BUILD))"; \
		exec 9>"$$lock_file"; \
		flock 9; \
	fi; \
	missing=0; \
	stale=0; \
	for f in $(MUSL_CHECK_FILES); do \
		if [ ! -f "$$f" ]; then missing=1; break; fi; \
	done; \
	if [ "$$missing" -eq 0 ] && \
	   [ "$$(cat "$(MUSL_BUILD_STAMP)")" != '$(MUSL_BUILD_ID)' ]; then \
		stale=1; \
	fi; \
	if [ "$$missing" -eq 0 ] && find "$(MUSL_BASE)" \
		\( -path "$(MUSL_BASE)/build-*" -o -path '*/.git' \) -prune -o \
		-type f -newer "$(MUSL_CRT)/libc.a" -print -quit | grep -q .; then \
		stale=1; \
	fi; \
	if [ "$$missing" -ne 0 ] || [ "$$stale" -ne 0 ]; then \
		if [ "$(MUSL_BUILD_WAS_OVERRIDDEN)" = 1 ] && \
		   [ -d "$(MUSL_BUILD)" ] && \
		   [ -n "$$(find "$(MUSL_BUILD)" -mindepth 1 -maxdepth 1 -print -quit)" ]; then \
			echo "[MUSL] refusing to rebuild explicit MUSL_BUILD with mismatched or incomplete identity: $(MUSL_BUILD)" >&2; \
			exit 1; \
		fi; \
		echo "[MUSL] $(MUSL_VARIANT) identity stale or missing, rebuilding private output..."; \
		if [ "$(MUSL_BUILD_WAS_OVERRIDDEN)" = 0 ]; then rm -rf "$(MUSL_BUILD)"; fi; \
		mkdir -p "$(MUSL_BUILD)"; \
		(cd "$(MUSL_BUILD)" && env -u ARCH -u MAKEFLAGS sh "$(MUSL_BASE)/configure" \
			--target=$(MUSL_TARGET) \
			--srcdir="$(MUSL_BASE)" \
			--prefix=/opt/musl-$(ARCH) \
			--disable-shared \
			--disable-wrapper \
			'CC=$(CC)' 'CROSS_COMPILE=$(CROSS_COMPILE)' 'AR=$(AR)' 'RANLIB=$(RANLIB)' $(if $(filter llvm,$(TOOLCHAIN)),'LIBCC=$(LIBGCC)',) \
			CFLAGS='$(MUSL_CONFIG_CFLAGS)' \
			LDFLAGS='$(MUSL_CONFIG_LDFLAGS)'); \
		env -u ARCH $(MAKE) -C "$(MUSL_BUILD)" $(MUSL_MAKE_JOBS); \
		mkdir -p "$(BUILD_DIR)"; \
		printf '%s\n' '$(MUSL_BUILD_ID)' > "$(MUSL_BUILD_STAMP).tmp"; \
		mv "$(MUSL_BUILD_STAMP).tmp" "$(MUSL_BUILD_STAMP)"; \
	else \
		echo "[MUSL] build-$(MUSL_VARIANT) ready"; \
	fi

# BusyBox remains upstream source, with the same musl CRT as A20 test programs.
# The RISC-V compiler can target bare metal while these binaries implement
# the Linux ABI. Upstream uses __linux__ to select headers and OS behavior.
BUSYBOX_CFLAGS = $(CFLAGS) -D__linux__ $(FF_LINUX_INC) -idirafter /usr/include
.PHONY: busybox busybox-stage
busybox: $(BUSYBOX_BIN)
$(BUSYBOX_BIN): Makefile $(ARCH_STAMP) ../tools/packaging/build_busybox.py ../tools/packaging/prepare_busybox.py | build_dir musl_prepare
	$(PYTHON) ../tools/packaging/build_busybox.py --arch $(ARCH) --nommu $(NOMMU) \
	    --output "$(abspath $@)" --jobs $(BUSYBOX_JOBS) --cc='$(CC)' \
	    --cflags='$(BUSYBOX_CFLAGS)' --ldflags='$(LDFLAGS)' \
	    --crt-start='$(CRT_START)' --crt-end='$(CRT_END)' --libc='$(LIBC)'
$(BUSYBOX_STAGE_STAMP): $(BUSYBOX_BIN) Makefile ../tools/packaging/stage_busybox.py
	$(PYTHON) ../tools/packaging/stage_busybox.py --binary "$(abspath $(BUSYBOX_BIN))" \
	    --output "$(abspath $(BUILD_DIR))" --applets "$(BUSYBOX_APPLETS)"
	@touch $@
$(BUILD_DIR)/sh $(BUSYBOX_TOOL_TARGETS): $(BUSYBOX_STAGE_STAMP)
	@test -x "$@" || { rm -f "$(BUSYBOX_STAGE_STAMP)"; $(MAKE) "$(BUSYBOX_STAGE_STAMP)"; }

$(BUILD_DIR)/rtw89d: $(wildcard wifi/*.c wifi/*.h wifi/compat/*/*.h wifi/compat/*.h) \
                    ../tools/drivers/build_rtw89.py external/rtw89/Makefile | build_dir musl_prepare
	@test "$(ARCH)" = x86_64
	$(PYTHON) ../tools/drivers/build_rtw89.py --cc "$(CC)" \
		--musl-build "$(MUSL_BUILD)" --out "$(abspath $(BUILD_DIR))/rtw89-build" --jobs 8
	cp $(BUILD_DIR)/rtw89-build/rtw89d $@
	$(if $(filter llvm,$(TOOLCHAIN)),$(LLVM_PREFIX)llvm-strip$(LLVM_SUFFIX),$(CROSS_COMPILE)strip) --strip-debug $@

$(BUILD_DIR)/wifi: wifi/wifi.c wifi/control.h | build_dir musl_prepare
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CRT_START) wifi/wifi.c $(CRT_END) $(LIBC)

$(BUILD_DIR)/wifi-dhcp: wifi/wifi-dhcp | build_dir
	cp $< $@
	chmod 755 $@

$(addprefix $(BUILD_DIR)/,wifi-gui wifi_backend.py wifi_portal.py a20-wifi.desktop): $(BUILD_DIR)/%: wifi/% | build_dir
	cp $< $@
	chmod 755 $@

$(BUILD_DIR)/input-probe.py: diagnostics/input-probe.py | build_dir
	cp $< $@
	chmod 755 $@

$(BUILD_DIR)/90-libinput-fuzz-override.rules: ../packages/overlay/xfce/etc/udev/rules.d/90-libinput-fuzz-override.rules | build_dir
	cp $< $@
	chmod 644 $@

$(BUILD_DIR)/90-a20-ibus.gschema.override: ../packages/overlay/xfce/usr/share/glib-2.0/schemas/90-a20-ibus.gschema.override | build_dir
	cp $< $@
	chmod 644 $@

$(BUILD_DIR)/session-log.py: ../packages/overlay/xfce/usr/lib/a20/session-log.py | build_dir
	cp $< $@
	chmod 755 $@

$(BUILD_DIR)/session-power.py: ../packages/overlay/xfce/usr/lib/a20/session-power.py | build_dir
	cp $< $@
	chmod 755 $@

$(BUILD_DIR)/rtw89-notices: wifi/LICENSE external/rtw89/LICENSE external/rtw89/CREDITS.md \
                          external/rtw89/firmware/LICENCE.rtlwifi_firmware.txt \
                          external/rtw89/third_party/hostap/COPYING \
                          external/rtw89/third_party/hostap/README \
                          external/rtw89/third_party/mbedtls/LICENSE | build_dir
	cat $^ > $@

# Optional libc/kernel ABI regression executable; not installed by `all`.
.PHONY: stock-musl-abi
stock-musl-abi: tests/test_stock_musl_abi.c cmds/linux_sched.h Makefile | build_dir musl_prepare
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/stock_musl_abi $(CRT_START) tests/test_stock_musl_abi.c $(CRT_END) $(LIBC)

$(BUILD_DIR)/init: $(INIT_OBJ) $(INIT_EXTRA_OBJS) | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(CRT_END) $(LIBC)

# One explicit rule per local command: flat binary -> subdir object, so make
# tracks the source dependency (the object rule below builds it from cmds/%/%.c).
# The link line takes $^ rather than $< so that a command which names extra
# prerequisites (hyp_boot/hypvm with the shared guest loader) links them too;
# for every other command $^ is just its own object, the same single file $< was.
define LOCAL_CMD_RULE
$(BUILD_DIR)/$(basename $(notdir $(1))): $(patsubst cmds/%.c,$(OBJ_DIR)/cmds/%.o,$(1)) | build_dir musl_prepare
	@mkdir -p $$(dir $$@)
	$(CC) $(LDFLAGS) -o $$@ $(CRT_START) $$^ $(CRT_END) $(LIBC)
endef
$(foreach src,$(LOCAL_CMD_SRCS),$(eval $(call LOCAL_CMD_RULE,$(src))))
$(BUILD_DIR)/pppoe: $(OBJ_DIR)/crypto/md5.o
$(OBJ_DIR)/cmds/net/pppoe.o: crypto/md5.h
$(OBJ_DIR)/crypto/%.o: crypto/%.c crypto/md5.h | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

define ARCH_CMD_RULE
$(BUILD_DIR)/$(basename $(notdir $(1))): $(patsubst %.c,$(OBJ_DIR)/%.o,$(1)) | build_dir musl_prepare
	@mkdir -p $$(dir $$@)
	$(CC) $(LDFLAGS) -o $$@ $(CRT_START) $$^ $(CRT_END) $(LIBC)
endef
$(foreach src,$(ARCH_CMD_SRCS),$(eval $(call ARCH_CMD_RULE,$(src))))
$(OBJ_DIR)/arch/%.o: arch/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


# The two guest-boot commands additionally link the shared loader.  Prerequisite
# only, no recipe: the rule above already gave both targets one.
$(BUILD_DIR)/hyp_boot $(BUILD_DIR)/hypvm $(BUILD_DIR)/a20-vmd: $(HYP_GUEST_OBJ)

$(INIT_OBJ): $(INIT_MAIN_SRC) Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/cmds/%.o: cmds/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/cmds/stress/fbdev_map_test.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/stress/fbdev_map_test.o: ../kernel/include/uapi/a20/framebuffer.h

$(OBJ_DIR)/cmds/core/videoplay.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/videoplay.o: ../kernel/include/uapi/a20/framebuffer.h

$(OBJ_DIR)/cmds/core/audioplay.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/audioplay.o: ../kernel/include/uapi/a20/audio.h

# The injected fault address is a kernel/userspace contract, so both sides
# include the same uapi header rather than repeating the constant.
$(OBJ_DIR)/cmds/core/trap_storm_test.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/trap_storm_test.o: ../kernel/include/uapi/a20/trap_storm.h


$(OBJ_DIR)/cmds/stress/cluster_threads.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/stress/cluster_threads.o: ../kernel/include/uapi/a20/block.h

$(OBJ_DIR)/cmds/core/virtio_scsi_test.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/virtio_scsi_test.o: ../kernel/include/uapi/a20/block.h

$(OBJ_DIR)/cmds/core/gpu3d_test.o: CFLAGS += -I../kernel/include

$(OBJ_DIR)/cmds/core/vport_test.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/vport_test.o: ../kernel/include/drivers/char/virtio_console.h

$(OBJ_DIR)/init_common/%.o: init_common/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/lib/%.o: lib/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(FF_GEN)/.headers-stamp: extra/fastfetch_gen_headers.sh \
                          $(shell find $(FF_DIR)/src/logo/ascii -name '*.txt' 2>/dev/null) \
                          $(FF_DIR)/doc/help.json $(FF_DIR)/CMakeLists.txt
	@mkdir -p $(FF_GEN)
	$(SHELL) extra/fastfetch_gen_headers.sh $(abspath $(FF_DIR)) $(FF_GEN)
	@touch $@

$(FF_GEN)/fastfetch_config.h $(FF_GEN)/fastfetch_datatext.h $(FF_GEN)/logo_builtin.h: $(FF_GEN)/.headers-stamp

$(FF_MAIN_OBJ): $(FF_MAIN) Makefile $(FF_GEN)/fastfetch_config.h $(FF_GEN)/fastfetch_datatext.h $(FF_GEN)/logo_builtin.h | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(FF_CFLAGS) -c $< -o $@

$(OBJ_DIR)/fastfetch/src/%.c: $(FF_DIR)/src/%.c Makefile | build_dir musl_prepare $(FF_GEN)/.headers-stamp
	@mkdir -p $(dir $@)

# gpu.c auto-detects optional host EGL headers with __has_include.  Those
# headers are not valid cross-target feature probes, so disable that probe for
# this translation unit; the A20OS build already disables GL backends.
$(OBJ_DIR)/fastfetch/src/detection/gpu/gpu.o: FF_CFLAGS += -D'__has_include(x)=0'

$(OBJ_DIR)/fastfetch/src/%.o: $(FF_DIR)/src/%.c Makefile $(FF_GEN)/fastfetch_config.h $(FF_GEN)/fastfetch_datatext.h $(FF_GEN)/logo_builtin.h | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(FF_CFLAGS) -c $< -o $@

$(FF_LIB): $(FF_LIB_OBJS) | build_dir
	@mkdir -p $(dir $@)
	$(RM) $@
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(FF_CMD): $(FF_MAIN_OBJ) $(FF_LIB) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(FF_MAIN_OBJ) $(CRT_END) $(FF_LIB) $(LIBC)

clean:
	@for i in 1 2 3; do \
		rm -rf $(BUILD_DIR) && break; \
		sleep 0.1; \
	done
	@test ! -e $(BUILD_DIR)

distclean: clean
	@:

# Header deps written by CFLAGS above, read back so make rebuilds the objects
# whose translation unit included a changed header.  USER_INTERMEDIATE_TARGETS
# already enumerates every object this makefile compiles, so one substitution
# keeps the dep set in step with the compile rules instead of duplicating it;
# the lib/ objects are appended because that rule has no source list of its own.
# -include rather than include: a variant that has never been built has no .d
# files yet and must not fail on them (same guard as targets-images.mk).
DEP_FILES := $(filter %.d,$(USER_INTERMEDIATE_TARGETS:%.o=%.d)) \
             $(patsubst lib/%.c,$(OBJ_DIR)/lib/%.d,$(wildcard lib/*.c))

-include $(DEP_FILES)

.PHONY: all clean distclean musl_prepare
.PRECIOUS: $(USER_INTERMEDIATE_TARGETS)
.SECONDARY: $(USER_INTERMEDIATE_TARGETS)

# VM supervisor wire ABI and vport ioctl are shared source headers.
$(OBJ_DIR)/cmds/core/a20-vmd.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/a20-vmd.o: cmds/core/hyp/vmd_protocol.h ../kernel/include/drivers/char/virtio_console.h

ifeq ($(TOOLCHAIN),llvm)
include ../tools/mk/llvm-runtime.mk
musl_prepare: $(LLVM_USER_BUILTINS)
endif

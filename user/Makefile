ARCH ?= riscv64
OPT ?= -O3
PROFILE ?= full
NPROC ?= $(or $(shell getconf _NPROCESSORS_ONLN 2>/dev/null),$(shell sysctl -n hw.logicalcpu 2>/dev/null),4)
NOMMU ?= 0

.DEFAULT_GOAL := all

# This makefile only recurses into musl.  Keep the parent's jobserver flags,
# but do not leak command-line overrides such as ARCH and BUILD_DIR into musl.
override MAKEOVERRIDES =
MUSL_MAKE_JOBS := $(if $(filter -j%,$(MAKEFLAGS)),,-j$(NPROC))

SUPPORTED_ARCHES := riscv64 loongarch64 aarch64 x86_64 arm32 riscv32 ppc64le
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH '$(ARCH)'; expected one of: $(SUPPORTED_ARCHES))
endif

MUSL_VARIANT := $(ARCH)$(if $(filter 1,$(NOMMU)),-nommu,)
BUILD_DIR ?= build/$(MUSL_VARIANT)
OBJ_DIR := $(BUILD_DIR)/obj
MUSL_BASE := $(CURDIR)/external/musl
MUSL_BUILD := $(MUSL_BASE)/build-$(MUSL_VARIANT)
MUSL_CRT := $(MUSL_BUILD)/lib

ARCH_STAMP := $(BUILD_DIR)/.build-id
ARCH_BUILD_ID := $(ARCH):$(NOMMU):$(OPT):$(PROFILE)

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
RISCV_ELF_RV32_MULTIDIR := $(shell if command -v $(RISCV_ELF_PREFIX)gcc >/dev/null 2>&1; then \
	$(RISCV_ELF_PREFIX)gcc -march=rv32imafdc -mabi=ilp32d -print-multi-directory 2>/dev/null; \
fi)
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
ARCH_CFLAGS_loongarch64 := -mabi=lp64d -march=loongarch64 -mcmodel=normal -fno-pic
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
MUSL_CONFIG_CFLAGS_loongarch64 := $(OPT) -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic -static
MUSL_CONFIG_CFLAGS_aarch64 := $(OPT) -fno-pic -static -fno-tree-vectorize
MUSL_CONFIG_CFLAGS_x86_64 := $(OPT) -m64 -fno-pic -fno-pie -mno-red-zone -static
MUSL_CONFIG_CFLAGS_arm32 := $(OPT) -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -mno-unaligned-access -static
MUSL_CONFIG_CFLAGS_riscv32 := $(OPT) -march=rv32imafdc -mabi=ilp32d -mcmodel=medany -static
MUSL_CONFIG_CFLAGS_ppc64le := $(OPT) -m64 -mcpu=power8 -mtune=power8 -mlong-double-64 -fno-pic -static -mno-vsx -mno-altivec -fno-tree-vectorize
MUSL_CONFIG_CFLAGS := $(MUSL_CONFIG_CFLAGS_$(ARCH)) -D__A20OS__

ifeq ($(NOMMU),1)
ifeq ($(ARCH),aarch64)
TLSE_NOMMU_CFLAGS := -mgeneral-regs-only
endif
endif

CCACHE ?= $(shell command -v ccache 2>/dev/null)
CCACHE_PREFIX := $(if $(CCACHE),$(CCACHE) ,)
CC := $(CCACHE_PREFIX)$(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib

MUSL_INC := -isystem $(MUSL_BUILD)/obj/include \
            -isystem $(MUSL_BASE)/include \
            -isystem $(MUSL_BASE)/arch/$(MUSL_ARCH) \
            -isystem $(MUSL_BASE)/arch/generic

CFLAGS := -Wall -Wextra $(OPT) -ffreestanding -nostdinc $(MUSL_INC) \
          -Iinit_common -Icmds $(ARCH_CFLAGS)
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

LIBGCC_DEFAULT := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)
ifeq ($(ARCH),riscv64)
# Keep libgcc on the same ABI as ARCH_CFLAGS.  In particular, the current
# riscv64 userspace is lp64d; forcing an lp64 archive makes the final link mix
# double-float objects with a soft-float runtime.
LIBGCC := $(LIBGCC_DEFAULT)
MUSL_CONFIG_LDFLAGS := -L$(dir $(LIBGCC))
else ifeq ($(ARCH),riscv32)
# The selected compiler already encodes the compatible RV32 multilib choice.
LIBGCC := $(LIBGCC_DEFAULT)
MUSL_CONFIG_LDFLAGS := -L$(dir $(LIBGCC))
else
LIBGCC := $(LIBGCC_DEFAULT)
MUSL_CONFIG_LDFLAGS :=
endif

# musl's makefiles do not treat config.mak as an input to every object.  Keep
# our own configuration signature so ABI, compiler, or flag changes cannot
# silently reuse incompatible CRT and libc objects after a rebase/toolchain
# upgrade.
MUSL_BUILD_STAMP := $(BUILD_DIR)/.musl-build-id
MUSL_CC_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
MUSL_CC_VERSION := $(shell $(CC) -dumpfullversion -dumpversion 2>/dev/null)
MUSL_BUILD_ID := target=$(MUSL_TARGET);cc=$(CC);cc-target=$(MUSL_CC_TARGET);cc-version=$(MUSL_CC_VERSION);cflags=$(MUSL_CONFIG_CFLAGS);ldflags=$(MUSL_CONFIG_LDFLAGS)

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
LIBC := -Wl,--start-group $(MUSL_CRT)/libc.a $(LIBGCC) -Wl,--end-group
endif

MUSL_CHECK_FILES := $(MUSL_CRT)/crt1.o \
                    $(MUSL_CRT)/crti.o \
                    $(MUSL_CRT)/crtn.o \
                    $(MUSL_CRT)/libc.a \
                    $(MUSL_BUILD)/obj/include/bits/alltypes.h \
                    $(MUSL_BUILD)/obj/include/bits/syscall.h \
                    $(MUSL_BUILD_STAMP)

SBASE_DIR := external/sbase
WGET_CMD_SRC := cmds/net/wget.c
WGET_OBJ := $(OBJ_DIR)/cmds/net/wget.o
WGET_CMD := $(BUILD_DIR)/wget
LOCAL_CMD_SRCS := $(filter-out $(WGET_CMD_SRC),$(wildcard cmds/*/*.c))
LOCAL_CMD_NAMES := $(basename $(notdir $(LOCAL_CMD_SRCS)))

SBASE_ALL_BINS := basename cat chgrp chmod chown chroot cksum cmp comm cp cut date dd \
                  dirname du echo env expand expr false find fold grep head hostname \
                  id join kill link ln logger logname ls mkdir mkfifo mknod mktemp mv \
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
TLSE_CFLAGS := $(TLSE_BASE_CFLAGS) $(TLSE_OPT) $(TLSE_ARCH_CFLAGS) $(TLSE_NOMMU_CFLAGS) -fno-strict-aliasing -fno-strict-overflow -fwrapv \
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
LOCAL_CMDS := $(addprefix $(BUILD_DIR)/,$(LOCAL_CMD_NAMES))

FF_DIR := external/fastfetch
FF_GEN := $(OBJ_DIR)/fastfetch/gen
FF_MAIN := $(FF_DIR)/src/fastfetch.c
FF_MAIN_OBJ := $(OBJ_DIR)/fastfetch/main.o

FF_COMMON_SRCS := \
	src/common/impl/commandoption.c \
	src/common/impl/duration.c \
	src/common/impl/font.c \
	src/common/impl/format.c \
	src/common/impl/frequency.c \
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

FF_LINUX_INC_riscv64 := -isystem /usr/include/riscv64-linux-gnu
FF_LINUX_INC_loongarch64 := -isystem /usr/include/loongarch64-linux-gnu
FF_LINUX_INC_aarch64 := -isystem /usr/include/aarch64-linux-gnu
FF_LINUX_INC_x86_64 := -isystem /usr/include/x86_64-linux-gnu
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

USER_INTERMEDIATE_TARGETS := $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(MKSH_OBJS) \
                             $(LOCAL_CMD_OBJS) $(SBASE_OBJS) $(SBASE_LIBUTF_OBJS) \
                             $(SBASE_LIBUTIL_OBJS) $(SBASE_LIBUTF) $(SBASE_LIBUTIL) \
                             $(WGET_OBJ) $(TLSE_OBJ) $(FF_MAIN_OBJ) $(FF_LIB_OBJS) $(FF_LIB) \
                             $(DESKTOP_OBJS) $(LVGL_OBJS)

BUILD_DESKTOP ?= 1
ifeq ($(BUILD_DESKTOP),1)
DESKTOP_TARGET := $(BUILD_DIR)/desktop
else
DESKTOP_TARGET :=
endif

ifneq ($(wildcard $(FF_DIR)/src/fastfetch.c),)
FF_AVAILABLE := 1
FASTFETCH_TARGET := $(FF_CMD)
else
FF_AVAILABLE :=
FASTFETCH_TARGET :=
endif

all: build_dir $(BUILD_DIR)/init $(BUILD_DIR)/mksh $(BUILD_DIR)/sh $(SBASE_CMDS) $(LOCAL_CMDS) $(WGET_CMD) $(DESKTOP_TARGET) $(FASTFETCH_TARGET)

fastfetch: build_dir $(FF_CMD)

build_dir: $(ARCH_STAMP)
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR)

musl_prepare:
	@set -e; \
	lock_file="$(BUILD_DIR).musl.lock"; \
	if command -v flock >/dev/null 2>&1; then \
		mkdir -p "$(dir $(BUILD_DIR))"; \
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
		echo "[MUSL] build-$(MUSL_VARIANT) stale or missing, rebuilding..."; \
		rm -rf "$(MUSL_BUILD)"; \
		mkdir -p "$(MUSL_BUILD)"; \
		(cd "$(MUSL_BUILD)" && env -u ARCH -u MAKEFLAGS sh ../configure \
			--target=$(MUSL_TARGET) \
			--prefix=/opt/musl-$(ARCH) \
			--disable-shared \
			--disable-wrapper \
			CC=$(CC) CROSS_COMPILE=$(CROSS_COMPILE) \
			CFLAGS='$(MUSL_CONFIG_CFLAGS)' \
			LDFLAGS='$(MUSL_CONFIG_LDFLAGS)'); \
		env -u ARCH $(MAKE) -C "$(MUSL_BUILD)" $(MUSL_MAKE_JOBS); \
		mkdir -p "$(BUILD_DIR)"; \
		printf '%s\n' '$(MUSL_BUILD_ID)' > "$(MUSL_BUILD_STAMP).tmp"; \
		mv "$(MUSL_BUILD_STAMP).tmp" "$(MUSL_BUILD_STAMP)"; \
	else \
		echo "[MUSL] build-$(MUSL_VARIANT) ready"; \
	fi

$(BUILD_DIR)/mksh: $(MKSH_OBJS) | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(MKSH_OBJS) $(CRT_END) $(LIBC)

$(BUILD_DIR)/sh: $(BUILD_DIR)/mksh | build_dir
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/init: $(INIT_OBJ) $(INIT_EXTRA_OBJS) | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(CRT_END) $(LIBC)

# One explicit rule per local command: flat binary -> subdir object, so make
# tracks the source dependency (the object rule below builds it from cmds/%/%.c).
define LOCAL_CMD_RULE
$(BUILD_DIR)/$(basename $(notdir $(1))): $(patsubst cmds/%.c,$(OBJ_DIR)/cmds/%.o,$(1)) | build_dir musl_prepare
	@mkdir -p $$(dir $$@)
	$(CC) $(LDFLAGS) -o $$@ $(CRT_START) $$< $(CRT_END) $(LIBC)
endef
$(foreach src,$(LOCAL_CMD_SRCS),$(eval $(call LOCAL_CMD_RULE,$(src))))

$(SBASE_CMDS): $(BUILD_DIR)/%: $(OBJ_DIR)/sbase/%.o $(SBASE_LIBUTIL) $(SBASE_LIBUTF) | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $< $(CRT_END) -Wl,--start-group $(SBASE_LIBUTIL) $(SBASE_LIBUTF) $(LIBC) -Wl,--end-group

$(WGET_CMD): $(WGET_OBJ) $(TLSE_OBJ) | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(WGET_OBJ) $(TLSE_OBJ) $(CRT_END) $(LIBC)

LVGL_DIR := external/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -type f -name '*.c')
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))
LVGL_CFLAGS := $(CFLAGS) -I$(LVGL_DIR) -Iexternal -Idesktop -DLV_CONF_INCLUDE_SIMPLE
ifeq ($(NOMMU),1)
LVGL_CFLAGS += -DA20OS_NOMMU
endif

DESKTOP_SRCS := desktop/desktop.c desktop/desktop_terminal.c desktop/lv_port_disp.c desktop/lv_port_indev.c \
desktop/desktop_utils.c desktop/desktop_cursor_breeze.c \
desktop/desktop_dashboard.c desktop/desktop_monitor.c desktop/desktop_processes.c \
desktop/desktop_files.c desktop/desktop_network.c desktop/desktop_system.c
DESKTOP_OBJS := $(patsubst desktop/%.c,$(OBJ_DIR)/desktop/%.o,$(DESKTOP_SRCS))

desktop/desktop_cursor_breeze.c: desktop/gen_breeze_cursor.py
	python3 $< $@ external/breeze

$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c desktop/lv_conf.h | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LVGL_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/desktop/%.o: desktop/%.c | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LVGL_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/desktop: $(DESKTOP_OBJS) $(LVGL_OBJS) | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $^ $(CRT_END) $(LIBC)

$(INIT_OBJ): $(INIT_MAIN_SRC) Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/cmds/%.o: cmds/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/cmds/core/audioplay.o: CFLAGS += -I../kernel/include
$(OBJ_DIR)/cmds/core/audioplay.o: ../kernel/include/uapi/a20/audio.h

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

$(TLSE_OBJ): $(TLSE_DIR)/tlse.c Makefile | build_dir musl_prepare
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

$(OBJ_DIR)/init_common/%.o: init_common/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/lib/%.o: lib/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/mksh/%.o: $(MKSH_DIR)/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(MKSH_CFLAGS) -c $< -o $@

$(FF_GEN)/.headers-stamp: $(shell find $(FF_DIR)/src/logo/ascii -name '*.txt' 2>/dev/null) \
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
	rm -rf $(MUSL_BASE)/build-*

.PHONY: all clean distclean musl_prepare
.PRECIOUS: $(USER_INTERMEDIATE_TARGETS)
.SECONDARY: $(USER_INTERMEDIATE_TARGETS)

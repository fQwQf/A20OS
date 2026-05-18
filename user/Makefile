ARCH ?= riscv64
OPT ?= -O3

.DEFAULT_GOAL := all

SUPPORTED_ARCHES := riscv64 loongarch64 aarch64
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH '$(ARCH)'; expected one of: $(SUPPORTED_ARCHES))
endif

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj/$(ARCH)
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
	src/detection/cpu/cpu.c \
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
	src/detection/displayserver/linux/wayland/zwlr-output.c \
	src/detection/displayserver/linux/wayland/kde-output.c \
	src/detection/displayserver/linux/wayland/wlr-output-management-unstable-v1-protocol.c \
	src/detection/displayserver/linux/wayland/kde-output-device-v2-protocol.c \
	src/detection/displayserver/linux/wayland/kde-output-order-v1-protocol.c \
	src/detection/displayserver/linux/wayland/xdg-output-unstable-v1-protocol.c \
	src/detection/displayserver/linux/wmde.c \
	src/detection/displayserver/linux/xcb.c \
	src/detection/displayserver/linux/xlib.c \
	src/detection/font/font_linux.c \
	src/detection/gpu/gpu_linux.c \
	src/detection/gpu/gpu_drm.c \
	src/detection/gpu/gpu_pci.c \
	src/detection/gpu/gpu_windows.c \
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
FF_LINUX_INC := $(FF_LINUX_INC_$(ARCH))

FF_CFLAGS := $(filter-out -Wconversion,$(CFLAGS)) \
             -I$(FF_DIR)/src -I$(FF_GEN) \
             -isystem /usr/include $(FF_LINUX_INC) \
             -include wchar.h \
             -D_GNU_SOURCE -D_XOPEN_SOURCE -D_ATFILE_SOURCE -D__linux__ \
             -D__STDC_WANT_LIB_EXT1__ -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
             -DFF_DISABLE_DLOPEN=1 \
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

USER_INTERMEDIATE_TARGETS := $(INIT_OBJ) $(INIT_EXTRA_OBJS) $(LIB_A20OS) $(MKSH_OBJS) \
                             $(LOCAL_CMD_OBJS) $(SBASE_OBJS) $(SBASE_LIBUTF_OBJS) \
                             $(SBASE_LIBUTIL_OBJS) $(SBASE_LIBUTF) $(SBASE_LIBUTIL) \
                             $(WGET_OBJ) $(TLSE_OBJ) $(FF_MAIN_OBJ) $(FF_LIB_OBJS) $(FF_LIB)

ifneq ($(wildcard $(FF_DIR)/src/fastfetch.c),)
FF_AVAILABLE := 1
else
FF_AVAILABLE :=
endif

all: build_dir $(BUILD_DIR)/init $(BUILD_DIR)/mksh $(BUILD_DIR)/sh $(SBASE_CMDS) $(LOCAL_CMDS) $(WGET_CMD)

fastfetch: build_dir $(FF_CMD)

build_dir:
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR)

musl_prepare:
	@set -e; \
	missing=0; \
	stale=0; \
	for f in $(MUSL_CHECK_FILES); do \
		if [ ! -f "$$f" ]; then missing=1; break; fi; \
	done; \
	if [ "$$missing" -eq 0 ] && find "$(MUSL_BASE)" \
		\( -path "$(MUSL_BASE)/build-*" -o -path '*/.git' \) -prune -o \
		-type f -newer "$(MUSL_CRT)/libc.a" -print -quit | grep -q .; then \
		stale=1; \
	fi; \
	if [ "$$missing" -ne 0 ] || [ "$$stale" -ne 0 ]; then \
		echo "[MUSL] build-$(ARCH) stale or missing, rebuilding..."; \
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

$(OBJ_DIR)/init_common/%.o: init_common/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/lib/%.o: lib/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/mksh/%.o: $(MKSH_DIR)/%.c Makefile | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(MKSH_CFLAGS) -c $< -o $@

$(FF_GEN)/.headers-stamp: $(wildcard $(FF_DIR)/src/logo/ascii/*.txt) \
                          $(FF_DIR)/src/data/help.json $(FF_DIR)/src/data/structure.txt
	@mkdir -p $(FF_GEN)
	$(SHELL) extra/fastfetch_gen_headers.sh $(abspath $(FF_DIR)) $(FF_GEN)
	@touch $@

$(FF_GEN)/fastfetch_config.h $(FF_GEN)/fastfetch_datatext.h $(FF_GEN)/logo_builtin.h: $(FF_GEN)/.headers-stamp

$(FF_MAIN_OBJ): $(FF_MAIN) Makefile $(FF_GEN)/fastfetch_config.h $(FF_GEN)/fastfetch_datatext.h $(FF_GEN)/logo_builtin.h | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(FF_CFLAGS) -c $< -o $@

$(OBJ_DIR)/fastfetch/src/%.c: $(FF_DIR)/src/%.c Makefile | build_dir musl_prepare $(FF_GEN)/.headers-stamp
	@mkdir -p $(dir $@)

$(OBJ_DIR)/fastfetch/src/%.o: $(FF_DIR)/src/%.c Makefile $(FF_GEN)/fastfetch_config.h $(FF_GEN)/fastfetch_datatext.h | build_dir musl_prepare
	@mkdir -p $(dir $@)
	$(CC) $(FF_CFLAGS) -c $< -o $@

$(FF_LIB): $(FF_LIB_OBJS) | build_dir
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(FF_CMD): $(FF_MAIN_OBJ) $(FF_LIB) $(LIB_A20OS) | build_dir musl_prepare
	$(CC) $(LDFLAGS) -o $@ $(CRT_START) $(FF_MAIN_OBJ) $(CRT_END) $(FF_LIB) $(LIBC)

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

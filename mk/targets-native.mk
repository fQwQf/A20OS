NATIVE_TEST_DIR  := user/tests
NATIVE_LD        := user/liba20rt/a20-generic.ld
NATIVE_CRT0_RV   := user/liba20rt/crt0_rv64.S
NATIVE_CRT0_LA   := user/liba20rt/crt0_la64.S
NATIVE_CRT0_AARCH64 := user/liba20rt/crt0_aarch64.S
NATIVE_CRT0_X86_64  := user/liba20rt/crt0_x86_64.S
NATIVE_CRT0_ARM32   := user/liba20rt/crt0_arm32.S
NATIVE_CRT0_RV32    := user/liba20rt/crt0_rv64.S
NATIVE_CRT0_PPC64LE := user/liba20rt/crt0_ppc64le.S
NATIVE_CC_riscv64     := $(RISCV_ELF_PREFIX)gcc
NATIVE_CC_loongarch64 := loongarch64-linux-gnu-gcc
NATIVE_CC_aarch64     := aarch64-linux-gnu-gcc
NATIVE_CC_x86_64      := x86_64-linux-gnu-gcc
NATIVE_CC_arm32       := arm-linux-gnueabihf-gcc
NATIVE_CC_riscv32     := $(RISCV_ELF_PREFIX)gcc
NATIVE_CC_ppc64le     := powerpc64le-linux-gnu-gcc
NATIVE_CC := $(CCACHE_PREFIX)$(NATIVE_CC_$(ARCH))
NATIVE_CFLAGS_riscv64     := -march=rv64gc -mabi=lp64d -mcmodel=medany
NATIVE_CFLAGS_loongarch64 := -march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic
NATIVE_CFLAGS_aarch64     := -march=armv8-a -fno-pic
NATIVE_CFLAGS_x86_64      := -m64 -mno-red-zone -fno-pic -fno-pie
NATIVE_CFLAGS_arm32       := -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-pic -fno-builtin
NATIVE_CFLAGS_riscv32     := -march=rv32imafdc -mabi=ilp32d -mcmodel=medany -fno-builtin
NATIVE_CFLAGS_ppc64le     := -m64 -mcpu=power8 -mabi=elfv2 -fno-pic -mno-vsx -mno-altivec
NATIVE_CFLAGS := $(NATIVE_CFLAGS_$(ARCH))
NATIVE_LIBS_arm32 := -Wl,--start-group -lgcc -Wl,--end-group
NATIVE_LIBS := $(if $(NATIVE_LIBS_$(ARCH)),$(NATIVE_LIBS_$(ARCH)),-lgcc)
NATIVE_ARCH_SRC_arm32 := user/liba20rt/a20_arm_rt.c
NATIVE_ARCH_SRC := $(NATIVE_ARCH_SRC_$(ARCH))
NATIVE_CRT0_riscv64     := $(NATIVE_CRT0_RV)
NATIVE_CRT0_loongarch64 := $(NATIVE_CRT0_LA)
NATIVE_CRT0_aarch64     := $(NATIVE_CRT0_AARCH64)
NATIVE_CRT0_x86_64      := $(NATIVE_CRT0_X86_64)
NATIVE_CRT0_arm32       := $(NATIVE_CRT0_ARM32)
NATIVE_CRT0_riscv32     := $(NATIVE_CRT0_RV32)
NATIVE_CRT0_ppc64le     := $(NATIVE_CRT0_PPC64LE)
NATIVE_CRT0 := $(NATIVE_CRT0_$(ARCH))
NATIVE_SDK_SRC   := user/liba20rt/a20_malloc.c
NATIVE_COMPILER_RT_SRC := user/liba20rt/a20_compiler_rt.c

NATIVE_LIBC_SRC  := \
    user/liba20c/malloc.c \
    user/liba20c/bare_alloc.c \
    user/liba20c/unistd.c \
    user/liba20c/dirent.c \
    user/liba20c/fcntl.c \
    user/liba20c/stdlib.c \
    user/liba20c/stdio.c \
    user/liba20c/printf.c \
    user/liba20c/string.c \
    user/liba20c/time.c \
    user/liba20c/fdtable.c \
    user/liba20c/exit.c \
    user/liba20c/errno.c \
    user/liba20c/environ.c \
    user/liba20c/a20_errno.c

define NATIVE_TEST_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
	    $(3) \
	    $(NATIVE_SDK_SRC) \
	    $(NATIVE_COMPILER_RT_SRC) \
	    $(NATIVE_ARCH_SRC) \
	    user/tests/test_native_hello.c \
	    $(NATIVE_LIBS) \
	    -o $(4)
endef

define NATIVE_MINIMAL_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    user/tests/test_native_minimal.c \
    -o $(4)
endef

$(NATIVE_HELLO_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_hello.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_TEST_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-test-arch: $(NATIVE_HELLO_BIN)

native-test-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-test-arch
native-test-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-test-arch
native-test-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) native-test-arch
native-test-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) native-test-arch
native-test-arm32:
	$(MAKE) ARCH=arm32 NOMMU=$(NOMMU) native-test-arch
native-test-rv32:
	$(MAKE) ARCH=riscv32 NOMMU=$(NOMMU) native-test-arch
native-test-ppc64le:
	$(MAKE) ARCH=ppc64le NOMMU=$(NOMMU) native-test-arch

native-test: $(DEFAULT_NATIVE_TEST_TARGETS)

native-test-all: native-test-rv native-test-la native-test-aarch64 native-test-x86_64 native-test-arm32 native-test-rv32 native-test-ppc64le

native-minimal-rv:
	$(call NATIVE_MINIMAL_RECIPE,$(RISCV_ELF_PREFIX)gcc,-march=rv64gc -mabi=lp64d -mcmodel=medany,$(NATIVE_CRT0_RV),user/build/riscv64/native-minimal-rv)
	@file user/build/riscv64/native-minimal-rv

native-minimal-la:
	$(call NATIVE_MINIMAL_RECIPE,loongarch64-linux-gnu-gcc,-march=loongarch64 -mabi=lp64d -mcmodel=normal -fno-pic,$(NATIVE_CRT0_LA),user/build/loongarch64/native-minimal-la)
	@file user/build/loongarch64/native-minimal-la

native-minimal: native-minimal-rv native-minimal-la

define NATIVE_HANDLE_TEST_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
	    $(3) \
	    $(NATIVE_SDK_SRC) \
	    $(NATIVE_COMPILER_RT_SRC) \
	    $(NATIVE_ARCH_SRC) \
	    user/tests/test_native_handle.c \
	    $(NATIVE_LIBS) \
	    -o $(4)
endef

$(NATIVE_HANDLE_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_handle.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_HANDLE_TEST_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-handle-test-arch: $(NATIVE_HANDLE_BIN)

native-handle-test-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-arm32:
	$(MAKE) ARCH=arm32 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-rv32:
	$(MAKE) ARCH=riscv32 NOMMU=$(NOMMU) native-handle-test-arch
native-handle-test-ppc64le:
	$(MAKE) ARCH=ppc64le NOMMU=$(NOMMU) native-handle-test-arch

native-handle-test: $(DEFAULT_NATIVE_HANDLE_TARGETS)

native-handle-test-all: native-handle-test-rv native-handle-test-la native-handle-test-aarch64 native-handle-test-x86_64 native-handle-test-arm32 native-handle-test-rv32 native-handle-test-ppc64le

define NATIVE_LIBC_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt -Iuser/liba20c/include \
    -T$(NATIVE_LD) \
    $(3) \
	    $(NATIVE_SDK_SRC) \
	    $(NATIVE_LIBC_SRC) \
	    $(NATIVE_ARCH_SRC) \
	    user/tests/test_liba20c.c \
	    $(NATIVE_LIBS) \
	    -o $(4)
endef

$(NATIVE_LIBC_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_LIBC_SRC) $(NATIVE_ARCH_SRC) \
		user/tests/test_liba20c.c user/liba20rt/a20-generic.ld \
		user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_LIBC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-libc-arch: $(NATIVE_LIBC_BIN)

native-libc-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-libc-arch
native-libc-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-libc-arch
native-libc-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) native-libc-arch
native-libc-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) native-libc-arch
native-libc-arm32:
	$(MAKE) ARCH=arm32 NOMMU=$(NOMMU) native-libc-arch
native-libc-rv32:
	$(MAKE) ARCH=riscv32 NOMMU=$(NOMMU) native-libc-arch
native-libc-ppc64le:
	$(MAKE) ARCH=ppc64le NOMMU=$(NOMMU) native-libc-arch

native-libc: $(DEFAULT_NATIVE_LIBC_TARGETS)

native-libc-all: native-libc-rv native-libc-la native-libc-aarch64 native-libc-x86_64 native-libc-arm32 native-libc-rv32 native-libc-ppc64le

native-programs: $(NATIVE_OUTPUTS)

define NATIVE_FUTEX_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    user/tests/test_native_futex.c \
    $(NATIVE_LIBS) \
    -o $(4)
endef

$(NATIVE_FUTEX_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_futex.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_sync.h
	$(call NATIVE_FUTEX_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-futex-arch: $(NATIVE_FUTEX_BIN)

native-futex-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-futex-arch

define NATIVE_DEBUG_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    user/tests/test_native_debug.c \
    $(NATIVE_LIBS) \
    -o $(4)
endef

$(NATIVE_DEBUG_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_debug.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_debug.h
	$(call NATIVE_DEBUG_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-debug-test-arch: $(NATIVE_DEBUG_BIN)

define NATIVE_EXT_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static     $(2)     -Iuser -Iuser/liba20rt     -T$(NATIVE_LD)     $(3)     $(NATIVE_SDK_SRC)     $(NATIVE_COMPILER_RT_SRC)     $(NATIVE_ARCH_SRC)     user/tests/test_native_ext.c     $(NATIVE_LIBS)     -o $(4)
endef

$(NATIVE_EXT_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_ext.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_ext.h
	$(call NATIVE_EXT_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-ext-test-arch: $(NATIVE_EXT_BIN)

native-ext-test-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-ext-test-arch

native-debug-test-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-debug-test-arch

define NATIVE_MM_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    user/tests/test_native_mm.c \
    $(NATIVE_LIBS) \
    -o $(4)
endef

$(NATIVE_MM_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_mm.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_mem.h
	$(call NATIVE_MM_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-mm-arch: $(NATIVE_MM_BIN)

native-mm-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-mm-arch

define NATIVE_SIGNAL_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    user/tests/test_native_signal.c \
    $(NATIVE_LIBS) \
    -o $(4)
endef

$(NATIVE_SIGNAL_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_signal.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_task.h
	$(call NATIVE_SIGNAL_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-signal-arch: $(NATIVE_SIGNAL_BIN)

native-signal-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-signal-arch

define NATIVE_IPC_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    user/tests/test_native_ipc.c \
    $(NATIVE_LIBS) \
    -o $(4)
endef

$(NATIVE_IPC_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_ipc.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_channel.h
	$(call NATIVE_IPC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-ipc-arch: $(NATIVE_IPC_BIN)

native-ipc-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-ipc-arch

native-ipc-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-ipc-arch

define NATIVE_CONTRACT_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    user/tests/test_native_contract.c \
    $(NATIVE_LIBS) \
    -o $(4)
endef

$(NATIVE_CONTRACT_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_contract.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_channel.h user/liba20rt/a20_event.h user/liba20rt/a20_mem.h user/liba20rt/a20_handle.h
	$(call NATIVE_CONTRACT_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-contract-arch: $(NATIVE_CONTRACT_BIN)

native-contract-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-contract-arch

native-contract-la:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) native-contract-arch

define NATIVE_SVC_RECIPE
@mkdir -p $(dir $(5))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    $(4) \
    $(NATIVE_LIBS) \
    -o $(5)
endef

$(NATIVE_SVCMAN_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/svcman.c user/svc/svc_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/svcman.c,$@)

$(NATIVE_ECHOD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/echod.c user/svc/svc_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/echod.c,$@)

native-svc-arch: $(NATIVE_SVCMAN_BIN) $(NATIVE_ECHOD_BIN)

native-svc-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-svc-arch

$(NATIVE_SHMRING_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_shmring.c user/svc/shmring_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_shmring.h user/liba20rt/a20_mem.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_shmring.c,$@)

$(NATIVE_SHMRINGD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/shmringd.c user/svc/shmring_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_shmring.h user/liba20rt/a20_mem.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/shmringd.c,$@)

$(NATIVE_CHAND_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/chand.c user/svc/shmring_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/chand.c,$@)

native-shmring-arch: $(NATIVE_SHMRING_BIN) $(NATIVE_SHMRINGD_BIN) $(NATIVE_CHAND_BIN)

native-shmring-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-shmring-arch

$(NATIVE_RTCD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_rtcd.c user/svc/rtcd_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h user/liba20rt/a20_device.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_rtcd.c,$@)

define NATIVE_RTCD_RECIPE
@mkdir -p $(dir $(5))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt -Ikernel/include \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    $(4) \
    $(NATIVE_LIBS) \
    -o $(5)
endef

$(NATIVE_RTCDD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/rtcd.c user/svc/rtcd_proto.h \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		kernel/include/drivers/driver_descriptor.h kernel/include/drivers/dual/drv_env.h kernel/include/drivers/dual/goldfish_rtc.h
	$(call NATIVE_RTCD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/rtcd.c,$@)

native-rtcd-arch: $(NATIVE_RTCD_BIN) $(NATIVE_RTCDD_BIN)



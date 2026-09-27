# ---- netd: userspace lwIP service (removed).
# The TCP/IP stack lives in the kernel (kernel/external/lwip + kernel/net);
# the user-space netd frame-ring/socket-proxy plane was abandoned.

.PHONY: smoke-native-dynlink smoke-native-mm smoke-native-signal

native-rtcd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-rtcd-arch

$(NATIVE_REGISTRY_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_registry.c user/liba20rt/a20_registry.h $(A20_SERVICES_IDL_HDR) \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_registry.c,$@)

$(NATIVE_SVCMGR_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/svcmgr.c user/liba20rt/a20_registry.h $(A20_SERVICES_IDL_HDR) \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/svcmgr.c,$@)

native-registry-arch: $(NATIVE_REGISTRY_BIN) $(NATIVE_SVCMGR_BIN)

native-registry-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-registry-arch

$(NATIVE_ISOLATION_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/tests/test_native_isolation.c $(A20_SERVICES_IDL_HDR) \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_SVC_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/tests/test_native_isolation.c,$@)

native-isolation-arch: $(NATIVE_ISOLATION_BIN)

native-isolation-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-isolation-arch

$(NATIVE_UBDD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/ubd.c $(A20_SERVICES_IDL_HDR) \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h kernel/include/drivers/driver_descriptor.h
	$(call NATIVE_RTCD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/ubd.c,$@)

# ufsd：用户态 FAT32 文件系统服务；fat32lite 与内核 NOMMU 构建同源编译。
# ufsd：用户态文件系统宿主。fat32lite 同源编译；ext4/isofs/ntfs 内核
# diskfs 源码经 fscompat 兼容环境（-Iuser/svc/fscompat 优先解析）原样编译。
UFSD_CORE_SRCS := user/svc/ufsd.c user/svc/fscompat/compat.c \
	user/svc/fscompat/bcache_user.c user/svc/ufs_fat_backend.c \
	user/svc/ufs_vnfs_backend.c kernel/fs/diskfs/fat32lite.c
UFSD_VNFS_SRCS := kernel/fs/diskfs/ext4.c kernel/fs/diskfs/ext4_file.c \
	kernel/fs/diskfs/ext4_journal.c kernel/fs/diskfs/ext4_namei.c \
	kernel/fs/diskfs/ext4_sync.c kernel/fs/diskfs/isofs.c \
	kernel/fs/diskfs/ntfs.c kernel/fs/diskfs/ntfs_file.c \
	kernel/fs/diskfs/ntfs_index.c kernel/fs/diskfs/ntfs_namei.c

define NATIVE_UFSD_RECIPE
@mkdir -p $(dir $(5))
$(1) -ffreestanding -nostdlib -static \
    $(2) \
    -Iuser -Iuser/liba20rt -Iuser/svc/fscompat -Ikernel/include \
    -T$(NATIVE_LD) \
    $(3) \
    $(NATIVE_SDK_SRC) \
    $(NATIVE_COMPILER_RT_SRC) \
    $(NATIVE_ARCH_SRC) \
    $(UFSD_CORE_SRCS) $(UFSD_VNFS_SRCS) \
    $(NATIVE_LIBS) \
    -o $(5)
endef

$(NATIVE_UFSD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) $(UFSD_CORE_SRCS) $(UFSD_VNFS_SRCS) \
		kernel/include/fs/fat32lite.h kernel/include/fs/ufs_proto.h \
		user/svc/ufs_backends.h user/svc/ufs_fat_backend.c user/svc/ufs_vnfs_backend.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h
	$(call NATIVE_UFSD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/ufsd.c,$@)

native-ufsd-arch: $(NATIVE_UFSD_BIN)

native-ufsd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-ufsd-arch

$(NATIVE_UINPUTD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/uinputd.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		kernel/include/drivers/driver_descriptor.h kernel/include/drivers/dual/drv_env.h kernel/include/drivers/dual/virtio_mmio.h kernel/include/drivers/dual/virtio_input.h kernel/include/drivers/dual/virtq.h
	$(call NATIVE_RTCD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/uinputd.c,$@)

native-uinputd-arch: $(NATIVE_UINPUTD_BIN)

native-uinputd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-uinputd-arch

$(NATIVE_UEDUD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/uedud.c \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h \
		kernel/include/drivers/driver_descriptor.h kernel/include/drivers/dual/drv_env.h
	$(call NATIVE_RTCD_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),user/svc/uedud.c,$@)

native-uedud-arch: $(NATIVE_UEDUD_BIN)

native-uedud-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-uedud-arch

define NATIVE_PERSONALITY_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) -Iuser -Iuser/liba20rt -T$(NATIVE_LD) \
    $(3) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
    user/tests/test_native_personality.c $(NATIVE_LIBS) -o $(4)
endef

$(NATIVE_PERSONALITY_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		user/tests/test_native_personality.c user/liba20rt/a20_personality.h
	$(call NATIVE_PERSONALITY_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-personality-arch: $(NATIVE_PERSONALITY_BIN)

native-personality-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-personality-arch

define NATIVE_LINUX_RECIPE
@mkdir -p $(dir $(4))
$(1) -ffreestanding -nostdlib -static \
    $(2) -Iuser -Iuser/liba20rt -T$(NATIVE_LD) \
    $(3) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
    user/tests/test_native_linux.c $(NATIVE_LIBS) -o $(4)
endef

$(NATIVE_LINUX_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) \
		user/tests/test_native_linux.c user/liba20rt/a20_linux.h user/liba20rt/a20_personality.h
	$(call NATIVE_LINUX_RECIPE,$(NATIVE_CC),$(NATIVE_CFLAGS),$(NATIVE_CRT0),$@)

native-linux-arch: $(NATIVE_LINUX_BIN)

native-linux-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-linux-arch

native-ubd-arch: $(NATIVE_UBDD_BIN)

native-ubd-rv:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) native-ubd-arch

UBD_SCRATCH_IMG := $(BUILD_DIR)/ubd-scratch.img
UBD_SCRATCH_BIG := $(BUILD_DIR)/ubd-big.bin

$(UBD_SCRATCH_BIG): FORCE
	@rm -f $@; \
	dd if=/dev/zero of=$@ bs=1M count=4 2>/dev/null

$(UBD_SCRATCH_IMG): $(UBD_SCRATCH_BIG)
	@rm -f $@; \
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null; \
	$(MKFS_FAT) -F 32 $@; \
	printf 'A20OS-UBD-MARKER' | dd of=$@ bs=1 seek=4096 conv=notrunc 2>/dev/null; \
	mcopy -o -i $@ $(UBD_SCRATCH_BIG) ::/big.bin

UFS_SCRATCH_IMG := $(BUILD_DIR)/ufs-scratch.img

$(UFS_SCRATCH_IMG): FORCE
	@rm -f $@; \
	dd if=/dev/zero of=$@ bs=1M count=32 2>/dev/null; \
	$(MKFS_FAT) -F 32 $@; \
	printf 'hello-uxfs\n' | mcopy -o -i $@ - ::/hello.txt

UFS_EXT4_IMG := $(BUILD_DIR)/ufs-ext4.img
UFS_ISO_IMG  := $(BUILD_DIR)/ufs-iso.img
UFS_NTFS_IMG := $(BUILD_DIR)/ufs-ntfs.img

$(UFS_EXT4_IMG): FORCE
	@rm -rf $(BUILD_DIR)/ufs-ext4-staging; mkdir -p $(BUILD_DIR)/ufs-ext4-staging
	@printf 'hello ext4 user-space\n' > $(BUILD_DIR)/ufs-ext4-staging/hello.txt
	@rm -f $@; dd if=/dev/zero of=$@ bs=1M count=32 2>/dev/null
	mke2fs -q -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index \
		-d $(BUILD_DIR)/ufs-ext4-staging $@
	@rm -rf $(BUILD_DIR)/ufs-ext4-staging

$(UFS_ISO_IMG): tools/mkisofs_test.c
	$(HOST_CC) $(HOST_CFLAGS) $< -o $(BUILD_DIR)/a20-mkisofs-gen
	$(BUILD_DIR)/a20-mkisofs-gen $@

$(UFS_NTFS_IMG): FORCE
	@rm -f $@; dd if=/dev/zero of=$@ bs=1M count=32 2>/dev/null
	mkfs.ntfs -F -Q -L A20NTFS $@ >/dev/null 2>&1

# smoke-native-fs-all：ufsd 全部四种文件系统后端的端到端门禁。
# 盘位（避开 bus.3/bus.5 的用户驱动预留）：bus.2=fat(1) bus.4=ext4(2)
# bus.6=iso9660(3) bus.7=ntfs(4)；主存储在 bus.0。
smoke-native-fs-all:
	$(PYTHON) tools/smoke.py smoke-native-fs-all

# smoke-native-ufs：文件系统实现运行在用户态服务（ufsd）的端到端门禁。
# 第二块盘挂在空闲槽位 bus.2（bus.3/bus.5 已预留给 ubd/uinputd），
# DEV_CLASS_BLOCK 序号 1 由 ufsd 经 fs_block_io 访问；FAT32 解析全部发生
# 在 ufsd 进程内，内核仅保留 VFS 代理。
smoke-native-ufs:
	$(PYTHON) tools/smoke.py smoke-native-ufs

smoke-dual-input:
	$(PYTHON) tools/smoke.py smoke-dual-input

smoke-native-ubd:
	$(PYTHON) tools/smoke.py smoke-native-ubd

smoke-native-isolation:
	$(PYTHON) tools/smoke.py smoke-native-isolation

smoke-native-registry:
	$(PYTHON) tools/smoke.py smoke-native-registry

smoke-native-rtcd:
	$(PYTHON) tools/smoke.py smoke-native-rtcd

smoke-native-shmring:
	$(PYTHON) tools/smoke.py smoke-native-shmring

smoke-clock-vdso:
	$(PYTHON) tools/smoke.py smoke-clock-vdso

smoke-native-svc:
	$(PYTHON) tools/smoke.py smoke-native-svc

smoke-native-contract:
	$(PYTHON) tools/smoke.py smoke-native-contract

smoke-native-personality:
	$(PYTHON) tools/smoke.py smoke-native-personality

smoke-native-linux:
	$(PYTHON) tools/smoke.py smoke-native-linux

smoke-native-ipc:
	$(PYTHON) tools/smoke.py smoke-native-ipc

smoke-native-signal:
	$(PYTHON) tools/smoke.py smoke-native-signal

smoke-native-dynlink:
	$(PYTHON) tools/smoke.py smoke-native-dynlink

smoke-native-mm:
	$(PYTHON) tools/smoke.py smoke-native-mm

smoke-native-futex:
	$(PYTHON) tools/smoke.py smoke-native-futex

smoke-native-deepen:
	$(PYTHON) tools/smoke.py smoke-native-deepen

smoke-native-ext:
	$(PYTHON) tools/smoke.py smoke-native-ext

smoke-native-debug:
	$(PYTHON) tools/smoke.py smoke-native-debug

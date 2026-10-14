# ---- netd: userspace lwIP service (removed).
# The TCP/IP stack lives in the kernel (kernel/external/lwip + kernel/net);
# the user-space netd frame-ring/socket-proxy plane was abandoned.

.PHONY: smoke-native-dynlink smoke-native-mm smoke-native-signal \
	smoke-native-ipc-wake-race smoke-native-ipc-queued-reply

.PHONY: smoke-native-suite
smoke-native-suite:
	$(PYTHON) tools/smoke.py native-suite --arch $(ARCH)

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

NATIVE_UBD_RUST_OBJ := $(USER_BUILD_DIR)/rust/virtio_blk_core.o
ifeq ($(VIRTIO_BLK_CORE_IMPL),rust)
ifneq ($(ARCH),riscv64)
$(error VIRTIO_BLK_CORE_IMPL=rust user placement currently requires ARCH=riscv64)
endif
$(NATIVE_UBD_RUST_OBJ): kernel/drivers/block/virtio_blk_core.rs
	@mkdir -p $(dir $@)
	$(RUSTC) --edition=2021 --crate-type lib --emit=obj --target riscv64gc-unknown-none-elf \
		-C panic=abort -C opt-level=3 -C codegen-units=1 \
		-C relocation-model=static -C code-model=medium $< -o $@
NATIVE_UBD_CORE_DEFS := -DCONFIG_VIRTIO_BLK_CORE_RUST
NATIVE_UBD_CORE_OBJ := $(NATIVE_UBD_RUST_OBJ)
endif

$(NATIVE_UBDD_BIN): $(NATIVE_CRT0) $(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/ubd.c $(A20_SERVICES_IDL_HDR) \
		user/liba20rt/a20-generic.ld user/liba20rt/crt0_a20.h user/liba20rt/a20_syscall.h kernel/include/drivers/driver_descriptor.h \
		kernel/include/drivers/block/virtio_blk_core.h $(NATIVE_UBD_CORE_OBJ)
	$(NATIVE_CC) -ffreestanding -nostdlib -static $(NATIVE_CFLAGS) $(NATIVE_UBD_CORE_DEFS) \
		-Iuser -Iuser/liba20rt -Ikernel/include -T$(NATIVE_LD) $(NATIVE_CRT0) \
		$(NATIVE_SDK_SRC) $(NATIVE_COMPILER_RT_SRC) $(NATIVE_ARCH_SRC) user/svc/ubd.c \
		$(NATIVE_UBD_CORE_OBJ) $(NATIVE_LIBS) -o $@

# ufsd：用户态 FAT32 文件系统服务；fat32lite 与内核 NOMMU 构建同源编译。
# ufsd：用户态文件系统宿主。fat32lite 同源编译；ext4/isofs/ntfs 内核
# diskfs 源码经 fscompat 兼容环境（-Iuser/svc/fscompat 优先解析）原样编译。
UFSD_CORE_SRCS := user/svc/ufsd.c user/svc/fscompat/compat.c \
	user/svc/fscompat/bcache_user.c user/svc/ufs_fat_backend.c \
	user/svc/ufs_vnfs_backend.c kernel/fs/diskfs/fat32lite.c
UFSD_VNFS_SRCS := kernel/fs/diskfs/ext4.c kernel/fs/diskfs/ext4_file.c \
	kernel/fs/diskfs/ext4_journal.c kernel/fs/diskfs/ext4_csum.c \
	kernel/fs/diskfs/ext4_namei.c \
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
		kernel/include/fs/fat32lite.h kernel/include/fs/ufs_proto.h kernel/fs/diskfs/ext4_inode_free_txn.h \
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
	@$(PYTHON) tools/img.py scratch --scratch-kind zeros \
		--scratch-out "$@" --scratch-mb 4 \
		--mkfs-fat "$(MKFS_FAT)"
$(UBD_SCRATCH_IMG): $(UBD_SCRATCH_BIG)
	@$(PYTHON) tools/img.py scratch --scratch-kind fat-marker \
		--scratch-out "$@" --scratch-mb 64 \
		--mkfs-fat "$(MKFS_FAT)" --payload "$(UBD_SCRATCH_BIG)"
UFS_SCRATCH_IMG := $(BUILD_DIR)/ufs-scratch.img

$(UFS_SCRATCH_IMG): FORCE
	@$(PYTHON) tools/img.py scratch --scratch-kind fat-hello \
		--scratch-out "$@" --scratch-mb 32 \
		--mkfs-fat "$(MKFS_FAT)"
UFS_EXT4_IMG := $(BUILD_DIR)/ufs-ext4.img
UFS_ISO_IMG  := $(BUILD_DIR)/ufs-iso.img
UFS_NTFS_IMG := $(BUILD_DIR)/ufs-ntfs.img

.PHONY: native-ubd-image native-ufs-image native-ufs-images
native-ubd-image: $(UBD_SCRATCH_IMG)
native-ufs-image: $(UFS_SCRATCH_IMG)
native-ufs-images: $(UFS_SCRATCH_IMG) $(UFS_EXT4_IMG) $(UFS_ISO_IMG) $(UFS_NTFS_IMG)

$(UFS_EXT4_IMG): FORCE
	@$(PYTHON) tools/img.py scratch --scratch-kind ext4 \
		--scratch-out "$@" --scratch-mb 32 \
		--mkfs-fat "$(MKFS_FAT)"
$(UFS_ISO_IMG): tools/mkisofs_test.c
	$(HOST_CC) $(HOST_CFLAGS) $< -o $(BUILD_DIR)/a20-mkisofs-gen
	$(BUILD_DIR)/a20-mkisofs-gen $@

$(UFS_NTFS_IMG): FORCE
	@$(PYTHON) tools/img.py scratch --scratch-kind ntfs \
		--scratch-out "$@" --scratch-mb 32 \
		--mkfs-fat "$(MKFS_FAT)"
# smoke-native-fs-all：ufsd 全部四种文件系统后端的端到端门禁。
# 盘位（避开 bus.3/bus.5 的用户驱动预留）：bus.2=fat(1) bus.4=ext4(2)
# bus.6=iso9660(3) bus.7=ntfs(4)；主存储在 bus.0。
smoke-native-fs-all:
	$(PYTHON) tools/smoke.py smoke-native-fs-all --arch $(ARCH)

# smoke-native-ufs：文件系统实现运行在用户态服务（ufsd）的端到端门禁。
# 第二块盘挂在空闲槽位 bus.2（bus.3/bus.5 已预留给 ubd/uinputd），
# DEV_CLASS_BLOCK 序号 1 由 ufsd 经 fs_block_io 访问；FAT32 解析全部发生
# 在 ufsd 进程内，内核仅保留 VFS 代理。
smoke-native-ufs:
	$(PYTHON) tools/smoke.py smoke-native-ufs --arch $(ARCH)

smoke-dual-input:
	$(PYTHON) tools/smoke.py smoke-dual-input

smoke-native-ubd:
	$(PYTHON) tools/smoke.py smoke-native-ubd --arch $(ARCH)

smoke-native-isolation:
	$(PYTHON) tools/smoke.py smoke-native-isolation --arch $(ARCH)

smoke-native-registry:
	$(PYTHON) tools/smoke.py smoke-native-registry --arch $(ARCH)

smoke-native-rtcd:
	$(PYTHON) tools/smoke.py smoke-native-rtcd --arch $(ARCH)

smoke-native-shmring:
	$(PYTHON) tools/smoke.py smoke-native-shmring --arch $(ARCH)

smoke-clock-vdso:
	$(PYTHON) tools/smoke.py smoke-clock-vdso

smoke-native-svc:
	$(PYTHON) tools/smoke.py smoke-native-svc --arch $(ARCH)

smoke-native-rust-peer:
	$(PYTHON) tools/smoke.py smoke-native-rust-peer --arch $(ARCH)

smoke-native-rust-idl-service:
	$(PYTHON) tools/smoke.py smoke-native-rust-idl-service --arch $(ARCH)

smoke-native-contract:
	$(PYTHON) tools/smoke.py smoke-native-contract --arch $(ARCH)

smoke-native-cluster:
	$(PYTHON) tools/smoke.py smoke-native-cluster --arch $(ARCH)

smoke-native-personality:
	$(PYTHON) tools/smoke.py smoke-native-personality --arch $(ARCH)

smoke-native-linux:
	$(PYTHON) tools/smoke.py smoke-native-linux --arch $(ARCH)

smoke-native-ipc:
	$(PYTHON) tools/smoke.py smoke-native-ipc --arch $(ARCH)

smoke-native-ipc-wake-race:
	$(PYTHON) tools/smoke.py smoke-native-ipc-wake-race

smoke-native-ipc-queued-reply:
	$(PYTHON) tools/smoke.py smoke-native-ipc-queued-reply

smoke-native-signal:
	$(PYTHON) tools/smoke.py smoke-native-signal --arch $(ARCH)

smoke-native-dynlink:
	$(PYTHON) tools/smoke.py smoke-native-dynlink --arch $(ARCH)

smoke-native-mm:
	$(PYTHON) tools/smoke.py smoke-native-mm --arch $(ARCH)

smoke-native-futex:
	$(PYTHON) tools/smoke.py smoke-native-futex --arch $(ARCH)

smoke-native-deepen:
	$(PYTHON) tools/smoke.py smoke-native-deepen --arch $(ARCH)

smoke-native-pager-rust:
	$(PYTHON) tools/smoke.py smoke-native-pager-rust --arch $(ARCH)

smoke-native-ext:
	$(PYTHON) tools/smoke.py smoke-native-ext --arch $(ARCH)

smoke-native-debug:
	$(PYTHON) tools/smoke.py smoke-native-debug --arch $(ARCH)

.PHONY: smoke-native-cluster-supervisor
smoke-native-cluster-supervisor:
	$(PYTHON) tools/smoke.py smoke-native-cluster-supervisor

# One Native application contract across server ISAs, real kernel boots.
.PHONY: check-native-sync
check-native-sync:
	$(PYTHON) tools/check-native-sync.py --task-objects-impl $(TASK_OBJECTS_IMPL)

.PHONY: check-native-thread-admission
check-native-thread-admission:
	$(PYTHON) tools/check-native-thread-admission.py

.PHONY: check-native-task-object
check-native-task-object:
	$(PYTHON) tools/check-native-task-object.py

.PHONY: check-native-runtime
check-native-runtime:
	$(PYTHON) tools/check-native-runtime.py

.PHONY: check-native-spawn-admission
check-native-spawn-admission:
	$(PYTHON) tools/check-native-spawn-admission.py

.PHONY: check-native-exec-features
check-native-exec-features:
	$(PYTHON) tools/check-native-exec-features.py

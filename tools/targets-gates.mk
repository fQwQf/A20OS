# Host-side unit tests for pure kernel format/helper logic.  These compile
# shared headers with the host compiler and run on the build machine, so
# format regressions are caught without booting the kernel.
HOST_CC ?= gcc
HOST_TESTS_SRC := $(wildcard tools/tests/*.c)
HOST_TESTS_BIN := $(patsubst tools/tests/%.c,/tmp/a20-host-%,$(HOST_TESTS_SRC))

host-tests: $(HOST_TESTS_BIN)
	@for t in $(HOST_TESTS_BIN); do \
		echo "== $$t =="; \
		"$$t" || exit 1; \
	done
	@echo "host-tests: PASS"

/tmp/a20-host-%: tools/tests/%.c
	$(HOST_CC) -Ikernel/include -O2 -Wall -Wextra $< -o $@

# Minimal ISO9660 test image for the isofs driver (no mkisofs/xorriso needed).
$(ISOFS_IMG): tools/mkisofs_test.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -O2 -Wall -Wextra $< -o /tmp/a20-mkisofs
	/tmp/a20-mkisofs $@

check-vfs-abstraction: smoke-vfs-stress
	@rg -q "VFS_OPEN_DISPATCH_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "VFS_REFCOUNT_HELPER_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "VFS_DCACHE_MOUNT_VNODE_INVARIANT" kernel/include/fs/vfs.h
	@rg -q "vfile_ref_init" kernel/fs/file.c kernel/include/fs/file.h kernel/include/fs/vfs.h
	@rg -q "vfile_get" kernel/fs/file.c kernel/include/fs/file.h kernel/include/fs/vfs.h
	@rg -q "vfile_put_ref_only" kernel/fs/file.c kernel/include/fs/file.h kernel/include/fs/vfs.h
	@rg -q "\.open[[:space:]]*=" kernel/fs/diskfs/ramfs.c kernel/fs/diskfs/fat32.c kernel/fs/diskfs/ext4.c kernel/fs/procfs/procfs.c kernel/fs/devfs/devfs.c kernel/fs/cgroupfs.c kernel/fs/sysfs.c
	@rg -q "CGROUPFS_DOTDOT_PARENT_LOOKUP" kernel/fs/cgroupfs.c
	@rg -q "VFS_CONCURRENCY_SMOKE_MATRIX" kernel/fs/vfs.c
	@rg -q "VFS_STRESS: PASS" user/cmds/stress/vfs_stress.c
	@! rg -q "FS_TYPE_(FAT32|EXT4|RAMFS|PROCFS|DEVFS|CGROUP|SYSFS).*open|fat32_open_vnode|ext4_open_vnode|ramfs_open_vnode|procfs_open_vnode|devfs_open_vnode|cgroupfs_open_vnode|sysfs_open_vnode" kernel/fs/vfs.c
	@! rg -q "refcount_(set|inc|dec_and_test)\(&[A-Za-z0-9_>\.-]*->ref_count" kernel/fs/diskfs/ramfs.c kernel/fs/diskfs/fat32.c kernel/fs/diskfs/ext4.c kernel/fs/procfs/procfs.c kernel/fs/devfs/devfs.c kernel/fs/cgroupfs.c kernel/fs/sysfs.c kernel/fs/inotify.c kernel/fs/memfd.c kernel/fs/pipe.c
	@! rg -q "for simplicity" kernel/fs/cgroupfs.c
	@echo "check-vfs-abstraction: PASS"

check-abi-boundary:
	@$(PYTHON) tools/gen_linux_syscall_coverage.py
	@rg -q "LINUX_ABI_BOUNDARY_CONTRACT" kernel/abi/linux/syscall_impl.h
	@rg -q "LINUX_ABI_EXPLICIT_STUB_CONTRACT" kernel/abi/linux/syscall_table.def
	@rg -q "LINUX_ABI_SCHED_STUB_BOUNDARY" kernel/abi/linux/sys_sched.c
	@rg -q "smoke-sched-stress" tools/targets-smoke.mk
	@rg -q "SCHED_STRESS: PASS" user/cmds/stress/sched_stress.c
	@rg -q "smoke-futex-stress" tools/targets-smoke.mk
	@rg -q "FUTEX_STRESS: PASS" user/cmds/stress/futex_stress.c
	@rg -q "LINUX_ABI_BPF_STUB_BOUNDARY" kernel/abi/linux/sys_bpf.c
	@rg -q "LINUX_ABI_NAMESPACE_STUB_BOUNDARY" kernel/abi/linux/sys_namespace.c
	@rg -q "LINUX_ABI_CAPABILITY_STUB_BOUNDARY" kernel/abi/linux/sys_capability.c
	@rg -q "ABI_CORE_API_CONTRACT" kernel/include/abi/core_api.h
	@rg -q "abi_core_proc_exec" kernel/abi/linux/sys_namespace.c kernel/include/abi/core_api.h
	@rg -q "abi_core_proc_mmap" kernel/abi/native/sys_phase2.c kernel/include/abi/core_api.h
	@rg -q "NATIVE_DEBUG_LIMITED_CONTRACT" kernel/abi/native/sys_phase2.c
	@rg -q "NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX" kernel/include/ipc/handle_table.h
	@rg -q "NATIVE_HANDLE_CAPABILITY_TEST_CONTRACT" kernel/abi/native/handle_table.c
	@rg -q "Debug 分区受限" docs/native-abi/00-overview.md
	@! rg -q "uint64_t args\[[0-9]+\]" user/liba20c/*.c
	@! rg -q "无 stub 残留|all Phase 2\+ syscalls|Debug \(0x0900\) — stubs" docs/native-abi/00-overview.md kernel/abi/native/sys_core.c kernel/abi/native/sys_phase2.c
	@echo "check-abi-boundary: PASS"

check-driver-core-model: smoke-driver-lifecycle
	@rg -q "DRIVER_CORE_CONCURRENCY_MODEL" kernel/drivers/core/driver_core.c
	@rg -q "spin_lock_irqsave\(&g_driver_core_lock\)" kernel/drivers/core/driver_core.c
	@rg -q "mutex_lock\(&g_driver_core_ops\)" kernel/drivers/core/driver_core.c
	@rg -q -- "return -EEXIST" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_CORE_DYNAMIC_LIMITS" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_IRQ_TABLE_FIXED_LIMIT" kernel/drivers/core/driver_hwapi.c
	@rg -q "irq_table_lock" kernel/drivers/core/driver_hwapi.c
	@rg -q "irq_active" kernel/drivers/core/driver_hwapi.c
	@rg -q "DRIVER_PROBE_FAILURE_CLEANUP" kernel/drivers/core/driver_core.c
	@rg -q "dev->drv = NULL" kernel/drivers/core/driver_core.c
	@rg -q "dev->state = DEV_STATE_UNINIT" kernel/drivers/core/driver_core.c
	@rg -q "DRIVER_ENUMERATION_FAILURE_MODEL" kernel/drivers/bus/virtio_mmio_bus.c kernel/drivers/bus/pci_bus.c
	@rg -q "DRIVER_IRQ_DMA_SEMANTICS" kernel/drivers/core/driver_hwapi.h
	@rg -q "DRIVER_SMOKE_MATRIX" kernel/drivers/core/driver_core.h
	@rg -q "class_device_publish" kernel/drivers/core/driver_core.c kernel/drivers/core/driver_class.c
	@rg -q "class_device_unpublish" kernel/drivers/core/driver_core.c kernel/drivers/core/driver_class.c
	@rg -q "DEV_STATE_REMOVING" kernel/drivers/core/driver_core.h kernel/drivers/core/driver_core.c
	@rg -q "platform_device_register" kernel/drivers/bus/platform_bus.c kernel/platform/qemu-virt-x86_64/board.c
	@rg -q "DEV_CLASS_AUDIO" kernel/drivers/core/driver_core.h kernel/drvmod/examples/pc_spkr.c
	@rg -Fq "pci_class_code(dev) != 0x040300" kernel/drvmod/examples/hda.c
	@rg -Fq "pci_class_code(dev) != 0x010802" kernel/drvmod/examples/nvme.c
	@rg -Fq "if (!size && bar_lo == 0)" kernel/drivers/bus/pci_bus.c
	@rg -q "\.match = hda_match" kernel/drvmod/examples/hda.c
	@rg -q "\.match = nvme_match" kernel/drvmod/examples/nvme.c
	@rg -q "NVME_IO_SMOKE: PASS" kernel/drvmod/examples/nvme.c tools/targets-steps.mk
	@! rg -q "CONFIG_X86_64" kernel/drvmod/examples/hda.c kernel/drvmod/examples/nvme.c
	@rg -q "drv_driver_register" kernel/drvmod/examples/pc_spkr.c
	@rg -q "virtio_blk_driver_probe" kernel/drivers/block/virtio_blk.c
	@rg -q "virtio_net_driver_probe" kernel/drivers/net/virtio_net.c
	@rg -q "uart_driver_probe" kernel/drivers/char/uart.c
	@rg -q "pty_init" kernel/drivers/char/pty.c
	@rg -q "loop_init" kernel/drivers/block/loop.c
	@rg -q "pci_enumerate" kernel/drivers/bus/pci_bus.c
	@rg -q "virtio_mmio_enumerate" kernel/drivers/bus/virtio_mmio_bus.c
	@rg -q "kernel/drivers/" docs/drivers/README.md
	@rg -q "kernel/platform/" docs/drivers/README.md
	@rg -q "DRIVER_LIFECYCLE_TEST" kernel/drivers/core/driver_lifecycle_test.c kernel/drivers/core/driver_lifecycle_test.h kernel/main.c
	@rg -q "driver_lifecycle_test_run" kernel/main.c kernel/fs/procfs/procfs.c kernel/drivers/core/driver_lifecycle_test.c
	@rg -q "duplicate driver registration" kernel/drivers/core/driver_lifecycle_test.c
	@! rg -q "virtio_gpu_init\(\)|virtio_input_init\(\)" kernel/main.c
	@rg -q "\[VINPUT\] driver registered in core" kernel/drvmod/examples/vinput.c
	@! rg -q "virtio_(blk|net)_init\(\)" kernel/main.c kernel/net/socket.c
	@! rg -q "arch_virtio_(gpu|input)_probe" kernel
	@rg -q "唯一枚举所有权" docs/drivers/guide/core-model.md
	@! rg -q "kernel/driver/|kernel/drv/|kernel/board/|#include \"driver/" docs/drivers -g "*.md"
	@echo "check-driver-core-model: PASS"

check-external-dependency-boundary:
	@rg -qF 'include $$(KERNEL_DIR)/external/lwip/sources.mk' Makefile
	@rg -q "EXTERNAL_LWIP_SOURCE_MANIFEST" docs/project/external-dependencies.md
	@rg -q "LWIP_SRC" kernel/external/lwip/sources.mk
	@rg -q "core/timeouts.c" kernel/external/lwip/sources.mk
	@rg -q "EXTERNAL_LWIP_CONFIG_CONTRACT" docs/project/external-dependencies.md
	@rg -q "NO_SYS=1" docs/project/external-dependencies.md
	@rg -q "g_lwip_lock" docs/project/external-dependencies.md kernel/net/lwip_stack.c
	@rg -q "a20_lwip_poll\(\)" docs/project/external-dependencies.md
	@rg -q "kernel_progress_poll\(\)" docs/project/external-dependencies.md
	@rg -q "EXTERNAL_QEMU_NET_DEFAULTS" docs/project/external-dependencies.md
	@rg -q "10\.0\.2\.15" docs/project/external-dependencies.md kernel/net/lwip_stack.c
	@rg -q "EXTERNAL_USERLAND_UPGRADE_CHECKLIST" docs/project/external-dependencies.md
	@rg -q "syscall smoke, shell smoke, and coreutils smoke" docs/project/external-dependencies.md
	@rg -q "EXTERNAL_STATIC_LINK_REBUILD_CONTRACT" docs/project/external-dependencies.md
	@rg -q "user/build/<arch>\\[-nommu\\]/\\.build-id" docs/project/external-dependencies.md
	@rg -q "EXTERNAL_TLSE_WGET_LIMITS" docs/project/external-dependencies.md
	@rg -q "TLS 1\.3" docs/project/external-dependencies.md
	@! rg -n "^LWIP_SRC[[:space:]]*=" Makefile
	@echo "check-external-dependency-boundary: PASS"

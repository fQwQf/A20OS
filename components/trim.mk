# GENERATED from components/trim.toml by `make regen-trim-fragment`.
# Do not edit by hand: edit the TOML and regenerate.  `make
# check-trim-registry` fails if this file is stale.

TRIM_PROFILE_MCU_ARCHES := armv7m
TRIM_PROFILE_MCU_OPT := -Os
TRIM_PROFILE_MCU_CPPFLAGS := -DCONFIG_MCU -DCONFIG_KLOG_BUF_SIZE=256
TRIM_PROFILE_MCU_SOURCES := kernel/mcu/main.c kernel/mcu/uart.c kernel/mcu/heap.c kernel/mcu/mcu_stubs.c kernel/mcu/leaf.c kernel/core/printf.c kernel/core/string.c kernel/core/panic.c kernel/core/sync.c kernel/core/smp.c kernel/core/shared_fabric.c kernel/core/fabric_atomics.c kernel/abi/native/sys_thread_basic.c kernel/abi/native/sys_task_signal.c kernel/abi/native/sys_file.c kernel/abi/native/sys_spawn_flat.c kernel/fs/file.c kernel/fs/flat_files.c kernel/mm/objcache.c kernel/abi/native/sys_thread_create.c kernel/abi/native/sys_thread_sched.c kernel/abi/native/handle_table.c kernel/ipc/a20_object.c kernel/proc/native_thread.c kernel/proc/task_object.c kernel/core/runtime.c kernel/abi/native/sys_runtime.c kernel/proc/wait.c kernel/abi/native/sys_mem_basic.c kernel/mm/flat_user.c kernel/abi/native/sys_execution.c kernel/abi/native/sys_native_sync.c kernel/ipc/futex.c kernel/core/klog.c kernel/core/timekeeping.c kernel/core/stack_protector.c kernel/proc/sched.c kernel/proc/park.c kernel/proc/timer_heap.c kernel/proc/current.c kernel/proc/pid.c kernel/proc/pidns.c kernel/proc/proc.c kernel/proc/userns.c kernel/proc/task.c kernel/proc/exit.c kernel/proc/signal.c kernel/proc/cg_cpu.c kernel/mm/nommu.c kernel/fs/diskfs/fat32lite.c
TRIM_PROFILE_MCU_FORCE_NOMMU := 1
TRIM_PROFILE_MCU_FORCE_BRINGUP := 1

TRIM_CAP_NOMMU_ARCHES := riscv64 riscv32 aarch64 arm32 armv7m
TRIM_CAP_NOMMU_BOARD_EXCLUSIONS := 
TRIM_CAP_PCIE_MMIO_ALLOC_ARCHES := x86_64 loongarch64 riscv64
TRIM_CAP_PCIE_MMIO_ALLOC_BOARD_EXCLUSIONS := licheerv-nano milk-v-duo ox64
TRIM_CAP_PCIE_MMIO_ECAM_ARCHES := loongarch64 riscv64
TRIM_CAP_PCIE_MMIO_ECAM_BOARD_EXCLUSIONS := licheerv-nano milk-v-duo ox64
TRIM_CAP_RAMFS_USER_ARCHES := loongarch64 riscv64
TRIM_CAP_RAMFS_USER_BOARD_EXCLUSIONS := 
TRIM_CAP_SMP_VERIFIED_QEMU_ARCHES := riscv64 riscv32 arm32 aarch64 loongarch64 x86_64
TRIM_CAP_SMP_VERIFIED_QEMU_BOARD_EXCLUSIONS := 
TRIM_CAP_SWAP_ARCHES := riscv64 loongarch64 aarch64 x86_64 arm32
TRIM_CAP_SWAP_BOARD_EXCLUSIONS := 
TRIM_CAP_XLATOR_ARCHES := riscv64 loongarch64 aarch64 x86_64
TRIM_CAP_XLATOR_BOARD_EXCLUSIONS := 

TRIM_ARCH_CPPFLAGS_aarch64 := -DCONFIG_TRAP_ESR_DIAG
TRIM_ARCH_CPPFLAGS_x86_64 := -DCONFIG_IOPORT -DCONFIG_AHCI -DCONFIG_PCI_MMIO_BASE_LEGACY

# 内核自身 W^X（KXAN：内核映像段权限分离）

用户态 W^X（`kernel/mm/wx.c`）解决的是进程地址空间；本文档描述内核**自身**映像
与直映射的权限收口。目标是引导完成后：

- 内核 `.text` 只读可执行（ROX），`.rodata` 只读不可执行（RO+NX），
  `.data`/`.bss` 读写不可执行（RW+NX）；
- 物理内存直映射（direct map）整体 NX；
- drvmod 内核模块的 text 段 RX、data 段 RW+NX。

## 实现方式

各架构在 `kernel/arch/<arch>/mm/kwx.c` 提供强符号 `arch_kernel_wx_finalize()`，
由 `kernel_main()` 在 `mm_init()` 之后立即调用（此时仍是单核、还没有任何进程
页表复制内核半区）。弱默认空实现在 `kernel/mm/kwx.c`，未完成的架构行为不变。

共同手法：

1. 链接脚本导出段边界符号 `__text_start/__rodata_start/__data_start`
   （`_bss_end` 原有），按页归类每个 4 KiB 页的权限。
2. 把覆盖 RAM 的引导大页降级为中间级表（下级为 2 MiB NX 块），内核映像
   占用的 2 MiB 块再拆为 4 KiB 页并按段打权限；非 RAM 叶项（MMIO）只去 X。
   中间级表经 `pt_map_kernel()` 复制的根项被所有进程页表**共享**，之后的
   修改（如模块打标）对所有地址空间同时生效。
3. **旁路构建 + 原子切换**：新页表先全部建好，再用一次写入替换活动页表
   中的大页项，最后全局 TLB flush。禁止"先装 NX 大页再拆块"——那会让
   CPU 只能靠 TLB 残存项续命，任何 TLB miss 都在 NX 的 .text 上取指故障，
   而 trap vector 同样 NX，直接三连环卡死（riscv64 SMP=2 实测）。
4. 自检：`mm_query_leaf()` 直接查询 text/rodata/data/映像外 RAM 四处
   权限，不符即 panic；成功打印
   `[KXAN] <arch>: text=ROX rodata=RO data=RW+NX dmap=NX`。

### riscv64（Sv39）

- 引导时 `boot_pgdir` 用 1 GiB megapage 同时恒等映射与高半映射 16 GiB
  RAM 窗口（RWX），该表即最终内核页表（`pt_map_kernel` 复制根项 256+）。
- finalize：与 RAM 相交的 gigapage 降级为共享 level-1 表（2 MiB NX 块），
  映像块再拆 4 KiB；MMIO 槽（含高半 MMIO 窗口）去 X。
- 恒等映射（根项 [0,256)）保留到 `arch_unmap_boot_identity()`：从核在
  `.enable_mmu` 写 satp 后的下一条取指仍走恒等地址，提前 NX/拆除会让从核
  在启用分页瞬间取指故障。`smp_boot_secondaries()` 之后整段清除并
  sfence（`kernel/arch/riscv64/mm/kwx.c` 中实现，`platform.h` 声明）。

### x86_64（4 级）

- 引导时 `boot_pdpt_hh`（PML4[256]）用四个 1 GiB 大页映射物理 0..4 GiB
  （RWX；EFER.NXE 由 `trap_init` 打开，AP 蹦床 `ap_trampoline.S` 也已补上
  NXE，否则从核启用分页即保留位 #PF）。
- finalize：与 RAM 相交的槽位（槽 0 必含内核映像）降级为共享 PD
  （2 MiB NX），映像块再拆 PT 4 KiB；MMIO 槽（PCD）只补 NX 位。
- 恒等映射（PML4[0]）由既有 `arch_unmap_boot_identity()` 在 SMP 启动后
  清除，未改动。

### aarch64（4 级，4K granule）

- 引导时 `boot_pgdir[0]/[1]` 共享 `boot_l1`；`boot_l1[1]` 是把整个 DRAM
  （PA 0x40000000 起 1 GiB，含内核映像 PA 0x40080000+）以 EL1 RW+可执行
  映射的 1 GiB block，entry.S 因此暂时清除 SCTLR_EL1.WXN。
- finalize：`boot_l1[1]` 降级为共享 L2 表（2 MiB NX block，EL1 RW、
  PXN|UXN），映像块再拆 L3 4 KiB；恒等别名走同一张 L1 自动一致。
- 拆分完成后在 C 中置回 WXN；`entry.S` 从核路径在 qemu-virt 非 NOMMU
  构建同步改为置位（从核启用 MMU 时映射已是 W^X 干净状态）。

### drvmod 模块

模块经 pfa 取页后在直映射执行。`kernel/drvmod/loader.c` 的布局改为
`[.text][veneers] 页对齐 | [data/bss][GOT]`，拷贝完成后调用
`arch_kwx_module_protect(pa, text_region, total)` 把 text 区置 RX、其余
RW+NX；`drvmod_unload()` 先 `arch_kwx_module_unprotect()` 恢复 RW+NX 再
还页。veneers（riscv64/aarch64 远调用跳板）是可执行代码，因此移入 text 区；
GOT 留在 data 区。打标后做本核 TLB flush，并在板级支持时做远程 shootdown
（riscv64/x86_64 有，aarch64 见下）。

## 关联修复：x86_64 内核态 TLS 风格栈金丝雀

`-fstack-protector-strong` 下 x86_64 编译器发出 TLS 风格 canary 读取
（`mov %fs:0x28,...`），而内核态 FS base 为 0，此前只是因为引导恒等映射
覆盖了线性地址 0x28 才"碰巧能用"；切到任务页表后即在 klog_write 等函数
上 #PF 递归。本次顺带收口（独立于段权限拆分，但同属本次验证范围）：

- `trap_init()` 每 CPU 将 IA32_FS_BASE 指向 `&__stack_chk_guard - 0x28`
  构成的内核 stub；
- `trap.S` 的 isr_common 与 syscall_entry_saved 在保存用户 FS base 后
  切换 FS base 到内核 stub，`__return_to_user` 在 iretq 前恢复原值
  （内核态代码永远读内核 canary，不再触碰可能尚未 demand-page 的用户
  TLS 页——否则页错误处理程序自身递归）。

残余边界：新 exec 任务首帧 fs_base=0，用户态 musl 用全局 canary 不读
%fs:0x28，无影响；aarch64 若编译器同样发 TLS canary（tpidr_el0+0x28），
同类隐患需在 aarch64 入口路径另行评估（不在本次范围）。

## 已知边界

- **aarch64 无远程 TLB shootdown**：qemu-virt-aarch64 板未接
  `remote_tlb_flush` op。模块打标只做本核 `tlbi vmalle1`；理论上从核可能
  持有覆盖模块页的 2 MiB NX block 旧翻译，在该核上首次执行模块代码时会
  误报指令 abort。实际触发需要模块在 SMP 运行后加载且在非加载核上执行，
  目前未发现触发；要收口需为 aarch64 接 IPI shootdown（参照
  `rv64_smp_remote_tlb_flush`）。
- **VirtualBox aarch64 板**：引导映射布局不同（RAM 在 PA 0x08000000，
  `boot_l1[1]` 已是 L2 表），本机制未覆盖；`entry.S` 从核路径在
  `CONFIG_BOARD_VIRTUALBOX_AARCH64` 下保持清除 WXN。可行方案是按同一
  模式拆 `boot_l1[0]` 的 1 GiB normal block，但无法在本环境验证，未交付。
- **aarch64 NOMMU**：身份映射即全部，无高半区可分；保持现状。
- **loongarch64**：内核空间经 DMW 直译窗口，完全绕过多级页表（见
  `pt_map_kernel` 注释），没有可按段打权限的页表层级；要做需要改用页表
  映射内核映像窗口（大改启动路径），本次未动。
- **ppc64le**：radix MMU 平台代码在
  `kernel/arch/ppc64le/platform/radix_mmu.c`，smoke 无覆盖、无 QEMU 验证
  路径，未动；机制上可仿照 x86_64 拆 radix 大页。
- **arm32 / riscv32 / loongarch32 / armv7m**：32 位或 MCU 平台，内核映像
  映射机制各异（arm32 有 `mm/pgtbl.c`），无 smoke 验证路径，未动。

## 验证

- 各架构 `kernel-only` 零警告构建（`-Werror`）；
- `make smoke-riscv64`、`make smoke-abi-linux`、`make smoke-x86_64`、
  `make smoke-aarch64`、`make smoke-smp-bringup`（riscv64 NR_CPUS=2）PASS；
- 引导日志可见 `[KXAN] <arch>: text=ROX rodata=RO data=RW+NX dmap=NX`
  自检行；drvmod 模块（rtc/virtio-blk/virtio-scsi/pc-spkr/ahci 等）加载
  并执行 DriverEntry 正常。

# PPC64LE 平台状态

PPC64LE 当前由 QEMU pSeries（`qemu-virt-ppc64le`）提供受支持的 MMU
运行目标。启动入口切换到 64 位 little-endian 模式，使用 PowerPC Radix
页表；用户地址空间保留低半根页表，内核高半映射通过每个进程共享的根项
继承。异常入口同时覆盖用户态系统调用、用户缺页和内核态 timer/device
中断，地址空间切换使用 pSeries process table 的 PID 1。

## 构建与运行

```sh
make ARCH=ppc64le BOARD=qemu-virt-ppc64le ABI=linux BRINGUP=1 kernel-only
make ARCH=ppc64le BOARD=qemu-virt-ppc64le run
make check-ppc64le-bringup
make check-ppc64le-user
```

QEMU 目标使用 pSeries 固件提供的虚拟终端、RTAS、PCI 配置访问和 TCE
DMA 窗口。`poweroff` 和 `reboot` 通过 RTAS token 请求固件动作；virtio
块设备和网络设备通过 pSeries PCI transport 探测。

## 当前进展（2026-08-02）

已完成并可在干净提交上复现：

- 启动与 Radix MMU 正常：`PAGE_OFFSET` 调整为根索引 256 的高半映射，
  `arch_current_cpu_id()` 在 pSeries guest 中不再读 PIR（会触发 Program
  Exception），`BRINGUP` smoke 通过且 RTAS poweroff 干净退出。
- 完整用户态（musl + 125 个命令 + init + mksh）已构建进 dev 镜像。
- 板级 `enumerate_devices` 直接调用 RTAS PCI/virtio 探测；virtio-blk 挂载
  fat32 到 `/bin`，`/bin/init` ELF 可加载，用户任务（pid 2）可创建。
- 修复了 ISI 取指页错误不保存故障地址的问题：指令页故障的 tval 应为
  SRR0 而不是 DAR，否则 demand paging 永远映射不了入口页。

## 已知边界与阻塞

- **用户任务可执行（打印 mksh 横幅），但 shell 尚未跑通**。上一版“外部
  中断（0x500）风暴”已经消除，关键修复包括：
  1. 根因：QEMU pSeries（默认 POWER10 CPU）在 `ibm,arch-vec-5-platform-
     support` 中把 byte 23 宣告为 `SPAPR_OV5_XIVE_EXPLOIT(0x80)`，即实际
     使用 **XIVE** 中断控制器（`H_XIRR`/`H_EOI` 未注册、FDT 无 ICP reg、
     `ic-mode=xics` 与 `-cpu power8` 均不可用）。
  2. `kernel/arch/ppc64le/boot/entry.S` + `ldscript.ld`：把 XIVE TIMA
     （real `0x6030203000000`，OS 页偏移 `0x190000`）以 2 MiB 叶映射到
     VA `0xC000800040000000`。
  3. `firmware.c ppc64_xics_ack()`：通过 TIMA OS/pool/HV 三环 CPPR 写
     0xff 屏蔽所有 XIVE 外部中断（本移植全部设备轮询，定时器走独立
     0x900 异常）。
  4. `trap_bridge.c`：`-d int` 实测显示存储页故障实际投递到 **0x400
     (ISI)** 而非 0x500（trace 只出现 1 次 ISI，无 EXTERNAL）；保留 0x500
     重定向仅为防御性兼容。
  - **2026-08-03 修复：`__switch` 任务上下文与 C 帧碰撞导致内核跳 0**。
    gdb 实测 `context_switch.part.0+508`（`bl __switch` 返回点）恢复时把
    `__switch` 保存区的 r21 槽（`[orig_r1+16]`）当作 C 帧 LR，`blr` 跳到
    0x0 使内核崩溃。修复：`switch.S` 先把 task_context_t（192 字节）分配
    在当前栈下方（同 loongarch64），保存后 `task->kstack` 指向该位置，
    恢复时 `sp` 仍取原帧基址。修复后 gdb 不再命中 0x0，trace 无 ISEG。
  - **2026-08-03 用户态突破**（无 SIGSEGV，用户任务与横幅正常启动）：
    1. **根因：PTE_U 与硬件 EAA_PRIV 位冲突**。ppc64le 的 `PTE_U` 定义在
       bit 59，经 bswap 恰好映射到 radix PTE 的 **EAA_PRIV**（bit 3），导致
       用户页设了 U 位反而变成"特权页"，用户无法访问自己的 .text，陷入
       取指缺页循环。修复：`page_table.h` 把 `PTE_U` 移到软件位（bit 49），
       `PTE_PRIV` 移到 bit 59（真正的 EAA_PRIV）。
    2. **0x500 交付破坏 SRR0**：QEMU 10.0.11 把同步存储故障投递到 0x500
       向量时，`r0-r2` 与 `SRR0` 均被破坏（SRR0 变成 trap 入口地址）。
       `trap_bridge.c` 的 redirect 对取指故障用 `ppc64_rtu_nip`（最近恢复的
       用户 PC）恢复真实故障地址，并把 `ctx->nip` 一并改正（`__return_to_user`
       会从 trap 帧重载用户 PC）。
    3. `__return_to_user` 仅在用户态且 r1 未被破坏时更新
       `ppc64_rtu_r1/r2/nip`，避免内核态嵌套 trap 泄漏内核值。
    4. `handle_present_page_fault` 对 present 页设置 `PTE_A`（硬件 R 位），
       避免 R/C 故障复发；`arch_tlb_flush_page` 回退到完整 `arch_tlb_flush`
       （address-form tlbie 在 TCG 下不可靠）。
  - **2026-08-03 用户缺页修复（RPDB）**：根因是 `ppc64_radix_root_entry`
    对进程表条目做 `__builtin_bswap64`，QEMU 按 LE（`ldq_phys`）读表后，
    `PRTBE_R_RPDB`（走查基址）被字节反转破坏成 `0x0d00903f00000000`（应为
    token `0x3f900000`），导致 QEMU 走查错误物理而报 NOPTE。修复：条目直接
    用 `(root_pa & 0x0FFFFFFFFFFFFF00) | 13`（RTS=52/RPDS=13，无字节交换）。
    验证：用户入口页（0x10434）demand paging 成功，用户执行到 `__libc_start_main`
    附近；boot 的 PID0 仍用原 `stdbrx` 编码（改 `std` 会破坏 boot，保留原样）。
  - **当前剩余阻塞**：用户执行中段后 SIGSEGV，`pc` 落在内核 `__trap_from_user`
    （`sepc=0x931fc4`，`sp=0x3ffffe90` 正确）。这是 QEMU 0x500 投递破坏
    SRR0/nip 的残留问题：`ppc64_rtu_nip` 恢复对某些嵌套/后续故障未生效
    （`ppc64_rtu_nip` 被内核态返回污染或 r1 被破坏时跳过存储）。另一用户
    进程（0x3daf0000）的缺页/syscall 正常，说明进程表/RTS/页表走查已基本
    可用。
- NOMMU 不支持，SMP 平台启动和远程 TLB shootdown 尚未加入已验证矩阵；
  构建系统默认拒绝该架构的 NOMMU 配置，SMP 实验必须显式设置
  `ALLOW_UNVERIFIED_SMP=1`。

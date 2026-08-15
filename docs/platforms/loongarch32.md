# LoongArch32 (LA32R) 移植状态

> **目标平台**：NaiLoong Core —— 一个以 Chisel 编写的 `LoongArch32 Reduced`（LA32R）
> 乱序超标量处理器核，挂接龙芯杯 NSCSCC SoC（32 位 AXI）。README 声称该核可在龙芯
> 实验箱约 80 MHz 运行性能测试并启动 Linux。
>
> **移植性质**：内核 bring-up 已完成并在 LA32R 全系统模拟器（cemu）上验证到
> `init_kthread (pid=1)`。**尚未**在任何真实 LA32R 硬件（NaiLoong Core / 龙芯杯实验箱）
> 上运行，也**尚未**提供用户态（musl loongarch32 尚未移植）。

LoongArch32 是本仓库第五个 32 位架构（在 riscv32 / arm32 / armv7m 之后），也是第三个
LoongArch 目标（与 loongarch64 并列）。架构目录 `kernel/arch/loongarch32/` 完整实现了
指令集机制层，板级 `kernel/platform/nailoong/` 描述 NaiLoong SoC。

## 与 loongarch64 的核心差异（移植要点）

LA32R 与 LA64 共享指令编码，但目标核 NaiLoong Core 的实现有一些 loongarch64 移植所
没有的特殊之处，全部集中在架构层：

1. **软件 TLB refill**。NaiLoong Core 没有硬件页表巡游器（无 `lddir`/`ldpte`，没有
   `PWCL/PWCH/STLBPS`）。TLB miss 触发 ecode `0x3F`，CPU 切到 DA 直址模式并跳到
   `TLBRENTRY (0x88)`；`trap.S` 里的 `tlb_refill_entry_la32` 在物理地址空间里软件巡游
   两级页表（`PGDL` 指向根表，`TLBEHI` 记录故障 VPN），装载 `TLBELO0/1` 后 `tlbfill`，
   `ertn` 时硬件恢复分页。无有效 PTE 时仍填充 V=0 表项，让重执访问产生精确的
   PIL/PIS/PIF 缺页。

2. **页表为两级**：根目录与叶目录各 1024 项 × 4 字节（`va[31:22]` / `va[21:12]`），
   `ARCH_PT_LEVELS=2`、`ARCH_PT_BITS=10`。PTE 位域把 `V/D/PLV/MAT/G` 放在与硬件
   `TLBELO` 相同的位置，`PPN` 在 `[31:12]`，语义位 `R/W/X/COW/LEAF` 是纯软件位
   （该核的 TLBELO 没有 NR/NX）。

3. **DMW 窗口**。该核的 DMW 位域为 `vseg[31:29]` / `pseg[27:25]`（512 MiB 窗口，
   `paddr = {pseg, va[28:0]}`）。内核配置：
   - `DMW0 = 0x88000011`：`0x80000000-0x9FFFFFFF`（DRAM）恒等映射、CC 缓存；
   - `DMW1 = 0x00000001`：`0x00000000-0x1FFFFFFF`（SoC MMIO，含 UART
     `0x1FE001E0`）恒等映射、uncached。
   内核自身（代码/栈/页表）经 DMW0 访问，不进 TLB；用户地址空间经页表 + TLB。

4. **定时器**。该核没有 `rdtime`，稳定计数器经 `rdcntvl.w` / `rdcntvh.w`（`CNTVL/CNTVH`
   两个 32 位 CSR）暴露；`timer.c` 先读高半再读低半避免进位错读。`TCFG/TICLR` 语义与
   loongarch64 相同。

5. **上下文切换与 `$tp`**。`__switch` 通过 `$tp` 寄存器把切出任务的 `sp` 写回
   `task->kstack`。首次切换离开 boot 上下文时 `$tp` 为 0，因此 `arch_set_task_pointer()`
   必须**显式** `move $tp, %0`（同时写 `SAVE1` 供 `__trap_from_user` 恢复），否则首切换
   会写到地址 0。loongarch64 移植在 QEMU 上“恰好”不炸是因为 QEMU 把地址 0 映射成了
   RAM；真实核上这是必须修的点。

6. **异常入口 64 字节对齐**。`EENTRY`/`TLBRENTRY` 会清低 6 位地址。la32 分支的 ld 不
   尊重输入节的 64 字节对齐（`.balign 64` 有 off-by-4 的 bug，`. = ALIGN(64)` 在独立输出
   节内也无效），因此 `trap.S` 用 `ALIGN_TO_64` 宏（显式 `.zero` 计算）保证入口在节内
   对齐，`ldscript.ld` 再用 `.text : { ... . = ALIGN(64); *(.trapcode) } :text` 把
   `trapcode` 放进 `.text` 的 PT_LOAD，既满足对齐又让 `objcopy -O binary` 携带陷阱代码。

7. **无 FPU / 无 LSX / 无 IOCSR**。trap 帧只保存 32 个 GPR + 异常元数据（144 字节），
   没有 FP/LSX 向量区；无 IOCSR 意味着无 IPI，单核目标（`.smp = NULL`）。

## 构建

需要 LA32R 交叉工具链。系统 `loongarch64-linux-gnu-gcc` 只支持 `lp64`（multilib 关闭），
所以从源码构建了带 la32 分支的 binutils + gcc：

- 上游补丁：`github.com/cloudspurs/binutils-gdb`（分支 `la32`）与
  `github.com/cloudspurs/gcc`（分支 `la32`）；
- 配置：`--target=loongarch32-linux-gnu`，默认 `-march=la32v1.0 -mabi=ilp32s`，
  按 `-march=la32v1.0 -mabi=ilp32s` 构建 libgcc（含 `__udivdi3/__divdi3` 等）；
- 该 gcc 的默认 ABI 仍是 `ilp32d`，链接时会有 ABI 警告但产物一致，无碍内核构建。

内核构建：

```sh
export PATH="$HOME/loongarch32-toolchain/install/bin:$PATH"

# bring-up 模式：完整启动后按规范关机
make ARCH=loongarch32 BOARD=nailoong BRINGUP=1 kernel-only

# 开发构建：启动到 init_kthread，尝试加载 /bin/init
make ARCH=loongarch32 BOARD=nailoong BRINGUP=0 kernel-only

# 架构边界门禁
make check-arch-boundary
make check-smp-platform-boundary
```

产出：`.kernel-build/loongarch32-nailoong-*/kernel.elf`（ELF32 LoongArch，入口
`0x80000000`）。

## 运行验证（cemu 模拟器）

上游 QEMU 目前没有 LoongArch32 机器，因此用 `github.com/cyyself/cemu`（支持
LoongArch32(Reduced) + TLB + 16550 UART 的全系统模拟器）做验证。cemu 的 la32r 实现
不是完整的 LA32R，验证过程中给 cemu 补了缺失指令并修了若干解码问题（见下文“cemu
补丁”）。

运行方式（`kernel.bin` 为 `objcopy -O binary` 产物）：

```sh
# cemu 自定义 main：RAM 映射到 0x80000000（512 MiB），UART 在 0x1FE001E0，
# 跳转到 0x80000000，轮询打印 UART 输出
cemu-a20os /tmp/kernel.bin
```

**bring-up 模式**（`BRINGUP=1`）完整启动到：

```
======================================
    A20OS Kernel
======================================
[INIT] Trap initialized
[INIT] Kernel extension points initialized
[FDT] LoongArch32 memory window 0x80000000..0xa0000000 (512 MiB)
[INIT] Board early init done
[INIT] UART initialized
[INIT] Timer initialized
[INIT] Timekeeping initialized
[MM] Buddy+Slab: 131072 frames, ... free (501 MB)
[INIT] Memory initialized
[INIT] Random initialized
[INIT] Driver core initialized
[INIT] Drivers probed
[RAMFS] Initialized, root inode 0
[INIT] VFS initialized
[LWIP] initialized: IPv4 IPv6 TCP UDP RAW ICMP DHCP DNS loopif
[INIT] Network initialized
[INIT] Process manager initialized
[INIT] System ready (bringup, no userspace)
part ok
System is going down for power-off NOW
```

**开发模式**（`BRINGUP=0`）继续走到：

```
[INIT] entering scheduler...
[INIT] init_kthread started (pid=1)
[DRIVERMGR] runtime driver manager init
[INIT] opening /bin/init...
[INIT] Cannot open /bin/init: -2
========== KERNEL PANIC ==========
init: no init program found
```

`/bin/init` 找不到是预期结果：板级没有块设备/根文件系统，也没有 LA32R 用户态。调度器、
`init_kthread`（pid=1）与驱动管理器的运行时路径均已打通。

## 工具链与模拟器的已知坑（移植过程记录）

这些不是内核 bug，但移植时绕开了它们，记录如下：

1. **la32 GCC 会把相邻字节写合并成未对齐的 `st.w`**（如 termios `c_cc[13..15]` 在
   偏移 29 处合成一个字写）。LoongArch 对未对齐访问报 `ALE`，真机上同样会崩。
   解决：`ARCH_CFLAGS_loongarch32` 增加 `-fno-store-merging`。
2. **`arch_set_task_pointer` 必须写 `$tp`**（见上文第 5 点）。
3. **la32 ld 的对齐**：输入节对齐、`.balign`、`. = ALIGN()` 均有偏差；异常入口对齐用
   “显式 `.zero` + 在 `.text` 内 `ALIGN(64)` 放置 trapcode”解决。
4. **cemu 补丁**（验证工具，不进入 A20OS 源码）：
   - 缺 `beqz`/`bnez`（21 位偏移分支），且 `_1ri21` 位域与编码不符 → 重写；
   - 缺 `bstrins.w`/`bstrpick.w`，且 msbw 在 `[20:16]` 与 `_4r.opcode` 的 bit20 重叠，
     用 binutils 的 `0xFFE08000` 掩码精确派发；
   - 缺 `masknez/maskeqz/andn/orn/rotri.w/ctz.w/revb.2h/ext.w.b/ext.w.h/bytepick.w`
     （bytepick 早期用成了 alsl 公式，已修）；
   - `bltz/blez/bgez/bgtz` 实际是 `blt/bge` 与 `$zero` 的别名，cemu 原生 `blt/bge`
     已覆盖，无需新增。

## 当前边界与后续工作

- **真实硬件验证**：当前证据只来自 cemu。下一步应在 NaiLoong Core 的 ChipLab /
  Verilator 流程（含 DiffTest）上跑同一内核镜像，核对 DMW/TLB/定时器语义。
- **用户态**：需要 musl loongarch32（或 glibc）移植与一个 rootfs/initramfs 才能进入
  shell；`exec.c` 的动态链接器回退名、`stat/statfs` 32 位 ABI 布局已就绪。
- **内存布局假设**：`0x80000000` 的 512 MiB DRAM 窗口、`0x1FE001E0` UART 来自龙芯杯
  SoC 惯例；若目标 SoC 不同，改 `kernel/platform/nailoong/board.c` 与 `entry.S` 的
  DMW 常量即可。
- **SMP**：NaiLoong 核无 IOCSR/IPI，板级保持 `.smp = NULL`；多核 SoC 需要新的 IPI
  机制与 secondary 启动路径。
- **构建门禁**：`check-loongarch32-bringup` 已登记，但未加入 `make all` 的默认架构集
  合（决赛提交只构建 RISC-V64 / LoongArch64）。

## 验收记录

- 日期：2026-08-15
- 环境：`loongarch32-linux-gnu-gcc`（cloudspurs la32 分支），cemu（本地补丁版）
- `BRINGUP=1`：完整启动并干净关机 ✓
- `BRINGUP=0`：启动到 `init_kthread (pid=1)`，`/bin/init` 因无 rootfs 缺失 ✓
- `make check-arch-boundary`、`make check-smp-platform-boundary`：PASS
- 已知未验证：真实 NaiLoong Core 上电、用户态 shell、SMP

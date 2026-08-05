# A20OS 混合内核：已建成架构

本文档描述混合内核**当前已实现的形态**（截至 2026-08-05，commit `933026e`）。分阶段的实施记录与验收数据见[01-roadmap.md](01-roadmap.md)；进一步微内核化计划见[02-mainstream-plan.md](02-mainstream-plan.md)。

## 1. 架构形态

```
┌────────────────────────────────────────────────────┐│ 用户态服务层（已实现，可崩溃、可重启）                  ││  svcman(监管)  echod  rtcd(RTC驱动)  shmringd/chand   │├────────────────────────────────────────────────────┤│ 混合内核层（性能关键路径，保持内核态）                  ││  EEVDF 调度 / MM(VMO·VMAR·缺页) / VFS 核心 / 页缓存   ││  Channel·EventQ IPC / 驱动框架·中断分发·MMIO 授权     │├────────────────────────────────────────────────────┤│ 兼容层                                               ││  Linux ABI(223 syscall) + vDSO 快路径                 │└────────────────────────────────────────────────────┘
```

判定规则（不变）：每秒调用 > 10k 次或延迟敏感 < 10µs 的路径留内核；崩溃频繁、协议解析类的迁出。调度器、MM/缺页、VFS 核心、dentry/inode/页缓存、TCP 数据面**确认留内核**。

## 2. 已建成的子系统

### 2.1 对象与能力（Native ABI 底座，先于本项目存在）

统一句柄表 + 14 位 rights + 时态约束 + BLP 标签（`kernel/abi/native/handle_table.c`）；类型化对象生命周期（`kernel/ipc/a20_object.c`）；Channel 含句柄传递（`ρ_recv = ρ_send ∩ ρ_transfer`）；EventQ 统一等待（含`A20_EVENT_EXITED` 任务退出、`A20_EVENT_SIGNALED` 设备信号）。

### 2.2 IPC 快路径（阶段 1）

`channel_call`（syscall `0x0508`，`kernel/abi/native/sys_native_ipc.c`）：一次陷入完成请求发送 + 回复等待，单次句柄查找（READ|WRITE）+ 单次参数校验；服务端唤醒走 `proc_try_wake` 的 priority-preempt 路径（两个 ABI 共享）。SDK：`a20_channel_call[_flags]`。

### 2.3 服务监管（阶段 2）

`user/svc/svcman.c`：`task_spawn` v2（stdio 继承）+ `target_slot`固定槽位传递服务端点（服务以编译期常量命名自己的端点）；EventQ监控 `A20_EVENT_EXITED`（`ev.data0` = 退出码）；指数退避重启 =新建 channel 对 + 重新 spawn。演示：两轮崩溃→自愈。

### 2.4 共享内存数据面（阶段 3）

`user/liba20rt/a20_shmring.h`：SPSC 字节环，全相对偏移（跨进程不同虚拟地址可用），acquire/release 游标，Dekker 门铃（futex 以物理页为 key，跨进程有效）。非满非空路径零 syscall。16 MiB 跨进程完整性验证，TCG 上与 channel 传输持平。

### 2.5 Linux ABI 透明获益（阶段 3B）

- **vDSO**（riscv64）：`kernel/vdso/riscv64/vdso.S` 提供 `__vdso_clock_gettime/gettimeofday/getcpu`；exec 时映射固定 VA （代码 RX `0x3FFC0000` + vvar RO `0x3FFC2000`）+ auxv `AT_SYSINFO_EHDR`；vvar 与内核 timekeeping 读同一个 `time` CSR， realtime 锚点与 syscall 路径位级一致（seqlock 保护）；fork 显式 重映射；musl 程序零改动，实测 4.3×。
- **唤醒捐赠**：pipe/AF_UNIX/futex/channel 的 wake 全部经 `proc_try_wake` priority-preempt，两个 ABI 自动共享。

### 2.6 用户态驱动框架（阶段 4）

`kernel/drivers/core/udriver.c` + syscall `0x0C00–0x0C03`：

- `device_map_mmio`：白名单设备物理窗口 → PFNMAP 用户映射 （qemu-virt-riscv64 注册 goldfish RTC 0x101000/4KiB）。
- `device_irq_listen/ack/unlisten`：IRQ → EventQ，VFIO/UIO 电平协议 （内核 thunk 先在 irqchip 屏蔽，投递 `A20_EVENT_SIGNALED`，用户 确认后重新武装）。
- `udriver_task_cleanup`：任务退出时在 EXITED 事件发出**之前**释放 IRQ 注册，监管者可立即重启设备驱动。

试点 `user/svc/rtcd.c`：goldfish RTC 整体用户态化（纳秒时钟读取、一次性闹钟 IRQ、崩溃演示）；`user/tests/test_native_rtcd.c` 验证时间 RPC、100ms 闹钟往返、崩溃检测与重启恢复。

## 3. 稳定性不变式（已实现）

1. 用户态服务只持有显式授予的 handle；Channel 值语义免疫指针注入。
2. 服务死亡 → 句柄表销毁 → 类型化对象回收（VMO/端点/订阅/IRQ 注册）。
3. 监管者用 EXITED + 退出码检测崩溃，退避后重启并重建服务端点。
4. 热路径（调度/缺页/VFS/页缓存）零额外跳转。

## 4. 性能现状（QEMU TCG 实测）

| 指标 | 数值 | 结论 |
|------|------|------|
| `clock_gettime`（musl） | 2457 ns vs syscall 10637 ns | vDSO 4.3× |
| RPC 往返（32B ping-pong） | ~80 µs | 上下文切换主导，融合收益被淹没 |
| 16 MiB 跨进程传输 | ring ≈ channel（39–42 MiB/s 级） | 持平，ring 陷入少两个数量级 |

**已知性能瓶颈**：RPC 往返成本 = 2 次上下文切换（无直接切换/时间片捐赠）。这是阶段 M1 的目标。

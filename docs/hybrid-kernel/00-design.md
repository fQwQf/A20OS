# A20OS 混合内核：设计参考

本文档描述 A20OS 混合内核的**当前设计形态**：为什么采用混合架构、内核与用户态服务的职责如何划分、以及各核心机制的语义与契约。机制细节与演进方向分别见 [01-mechanisms.md](01-mechanisms.md) 与 [02-mainstream-plan.md](02-mainstream-plan.md)。

## 设计定位

A20OS 同时提供两套用户接口：`abi/linux`（223 个 syscall，运行 musl 程序，是当前主用户态运行时）与 `abi/native`（111 个 syscall，面向能力、句柄与事件的新接口）。混合内核是这两套接口共享的执行底座：**把性能关键路径留在内核态，把可崩溃、可重启的用户态服务作为系统组成部分**。

划分依据是一条判定规则：

> 每秒调用超过约 10k 次或延迟敏感（< 10µs）的路径留在内核；崩溃频繁、协议解析类的工作迁出到用户态服务。

按此规则，调度器、MM/缺页、VFS 核心、dentry/inode/页缓存、TCP 数据面**保留在内核**；服务监管、设备驱动（低速）、注册/命名等作为**用户态服务**运行。

## 架构形态

```
┌────────────────────────────────────────────────────┐│ 用户态服务层（可崩溃、可重启）                        ││  svcman（监管） echod  rtcd（RTC 驱动） shmringd/chand │├────────────────────────────────────────────────────┤│ 混合内核层（性能关键路径）                            ││  EEVDF 调度 / MM(VMO·VMAR·缺页) / VFS 核心 / 页缓存   ││  Channel·EventQ IPC / 驱动框架·中断分发·MMIO 授权     │├────────────────────────────────────────────────────┤│ 兼容层                                               ││  Linux ABI(223 syscall) + vDSO 快路径                 │└────────────────────────────────────────────────────┘
```

- **用户态服务层**：以 Native ABI 编写的服务进程，通过 Channel/EventQ 与内核及其他服务通信。服务崩溃由 svcman 检测并重启，资源由内核按对象模型回收。
- **混合内核层**：自包含的内部实现（`kernel/ipc`、`kernel/mm`、`kernel/proc`、`kernel/drivers`、`kernel/include/core`），对 `abi/` 零依赖；ABI 层只是把用户 syscall 线格式翻译成内部 API 的薄包装。
- **兼容层**：Linux ABI 与 vDSO，让未修改的 musl 程序直接受益于内核机制（唤醒快路径、时间读取等）。

## 核心机制总览

| 机制 | 位置 | 语义 |
|------|------|------|
| `channel_call` 融合 RPC | `syscall 0x0508` | 一次陷入完成请求发送 + 回复等待 |
| 时间片捐赠 | `proc_park_commit_donate` | 同步 IPC 把剩余时间片直接交给被调者（仅 UP） |
| 服务监管 svcman | `user/svc/svcman.c` | 清单拉起、健康探针、崩溃检测、指数退避重启 |
| 服务注册表 | `syscall 0x0A03` | 按名解析服务端点，崩溃后自动重绑 |
| 共享内存 SPSC 环 | `user/liba20rt/a20_shmring.h` | 跨进程数据面，非满非空路径零 syscall |
| 用户态驱动框架 | `syscall 0x0C00–0x0C06` | MMIO 白名单授权 + IRQ→EventQ + DMA 缓冲契约 |
| 对象统计与配额 | `/proc/a20/objects` | 六项对象计数审计 + 句柄硬配额 |
| vDSO | `kernel/vdso/riscv64/vdso.S` | `clock_gettime` 等零陷入读取 |

各机制的详细语义见 [01-mechanisms.md](01-mechanisms.md)。

## 设计原则

### 内核内部实现自包含，ABI 只是薄包装

内部实现（IPC、MM、进程、驱动）自持类型、常量与 API；`abi/linux` 与 `abi/native` 只负责 syscall 线格式翻译。方向永远单向：

```
用户态 ── syscall 线格式 ──> ABI 层（薄包装）── 内部 API ──> 内部实现
```

这保证任何新的 ABI 都可以复用同一套混合内核机制，也保证内部机制不因 ABI 差异而分叉。

### 资源是类型化对象，生命周期显式

所有用户可见资源（文件、通道端点、事件队列、VMO、设备等）由 handle 引用，类型化对象带引用计数与销毁回调。服务崩溃后，内核按对象模型回收其全部资源（句柄表销毁 → 对象引用释放 → 类型化回收），六项对象计数器回归基线。

### 性能与隔离的边界可论证

- **留内核**：调度、MM、VFS、页缓存、TCP 数据面——高频、延迟敏感、崩溃后果严重。
- **迁用户态**：服务监管、设备驱动（低速）、协议/注册类——崩溃频繁、可重启、性能非关键。
- **块/网驱动不外迁**：TCG 数据显示其 I/O 路径由多次上下文切换 + 数据拷贝构成，在现有调度成本下外迁必然劣化（详见 [02-mainstream-plan.md](02-mainstream-plan.md) 的决策记录）。

## 已建成的子系统

### 对象与能力（Native ABI 底座）

统一句柄表 + 14 位 rights + 时态约束 + BLP 安全标签；类型化对象生命周期；Channel 句柄传递（`ρ_recv = ρ_send ∩ ρ_transfer`）；EventQ 统一等待（任务退出、设备信号等事件源）。

### IPC 快路径

`channel_call` 一次陷入完成"发送请求 + 等待回复"，单次句柄查找与参数校验；服务端唤醒走 priority-preempt 快路径，两个 ABI 共享。时间片捐赠在此基础上把同步 IPC 的两次调度排队减少为一次直接切换（UP）。

### 服务化与稳定性

svcman 以声明式清单拉起服务（stdio 继承 + 固定槽位端点传递），用 EventQ 监控服务退出，按指数退避重启；注册表提供按名解析与崩溃重绑；健康探针（ping + 超时强杀）保证服务真实可用。资源隔离由对象配额 + 泄漏审计保障。

### 共享内存数据面

SPSC 字节环（全相对偏移，跨进程不同虚拟地址可用；acquire/release 游标；futex 门铃以物理页为 key）。非满非空路径零 syscall，是用户态服务的大块数据传输通道。

### 用户态驱动框架

内核把白名单内的设备物理窗口以 PFNMAP 映射进驱动进程，把物理 IRQ 绑定为 EventQ 事件源（电平协议：屏蔽 → 投递 → ack 重新武装），DMA 缓冲只能来自内核分配的 VMO。低速设备（如 goldfish RTC）整体迁入用户态服务，崩溃后由 svcman 重启并恢复设备。

### Linux ABI 透明获益

vDSO（`clock_gettime`/`gettimeofday`/`getcpu`，与内核 timekeeping 位级一致）；唤醒快路径（pipe/AF_UNIX/futex/channel 的 wake 统一走 priority-preempt）对两个 ABI 自动生效。

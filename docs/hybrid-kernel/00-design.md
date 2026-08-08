# A20OS 混合内核：设计参考

本文档描述 A20OS 混合内核的**当前设计形态**：为什么采用混合架构、内核与用户态服务的职责如何划分、以及各核心机制的语义与契约。机制细节与演进方向分别见 [01-mechanisms.md](01-mechanisms.md) 与 [02-mainstream-plan.md](02-mainstream-plan.md)。

## 设计定位

A20OS 同时提供两套用户接口：`abi/linux`（`kernel/abi/linux/syscall_table.def` 登记 258 个 syscall，运行 musl 程序，是当前主用户态运行时）与 `abi/native`（`kernel/abi/native/syscall_table.def` 登记 126 个 syscall，面向能力、句柄与事件的新接口）。混合内核是这两套接口共享的执行底座：**把性能关键路径留在内核态，把可崩溃、可重启的用户态服务作为系统组成部分**。

划分依据是一条判定规则：

> 每秒调用超过约 10k 次或延迟敏感（< 10µs）的路径留在内核；崩溃频繁、协议解析类的工作迁出到用户态服务。

按此规则，调度器、MM/缺页、VFS 核心、dentry/inode/页缓存、TCP 数据面**保留在内核**；服务监管、设备驱动（低速）、注册/命名等作为**用户态服务**运行。

## 架构形态

```
┌────────────────────────────────────────────────────┐
│ 用户态服务层（可崩溃、可重启）                        │
│  svcmgr（监管） echod  rtcd（RTC） ubd（virtio-blk） │
│  uinputd（virtio-input） shmringd/chand              │
├────────────────────────────────────────────────────┤
│ 混合内核层（性能关键路径）                            │
│  EEVDF 调度 / MM(VMO·VMAR·缺页) / VFS 核心 / 页缓存   │
│  Channel·EventQ IPC / 驱动框架·中断分发·MMIO 授权     │
│  内核态驱动（lwIP 网络、virtio-blk 数据面）           │
├────────────────────────────────────────────────────┤
│ 兼容层                                               │
│  Linux ABI(258 syscall) + vDSO 快路径                 │
└────────────────────────────────────────────────────┘
```

- **用户态服务层**：以 Native ABI 编写的服务进程，通过 Channel/EventQ 与内核及其他服务通信。服务崩溃由 svcmgr 检测并重启，资源由内核按对象模型回收。
- **混合内核层**：自包含的内部实现（`kernel/ipc`、`kernel/mm`、`kernel/proc`、`kernel/drivers`、`kernel/include/core`），对 `abi/` 零依赖；ABI 层只是把用户 syscall 线格式翻译成内部 API 的薄包装。
- **兼容层**：Linux ABI 与 vDSO，让未修改的 musl 程序直接受益于内核机制（唤醒快路径、时间读取等）。

## 核心机制总览

| 机制 | 位置 | 语义 |
|------|------|------|
| `channel_call` 融合 RPC | `syscall 0x0508` | 一次陷入完成请求发送 + 回复等待 |
| 时间片捐赠 | `proc_park_commit_donate` | 同步 IPC 把剩余时间片直接交给被调者（仅 UP） |
| 服务监管 svcmgr | `user/svc/svcmgr.c` | 清单拉起、健康探针、崩溃检测、指数退避重启、注册表 serve |
| 服务注册表 | `syscall 0x0A03` | 按名解析服务端点，崩溃后自动重绑 |
| 共享内存 SPSC 环 | `user/liba20rt/a20_shmring.h` | 跨进程数据面，非满非空路径零 syscall |
| 用户态驱动框架 | `syscall 0x0C00–0x0C09` | MMIO 白名单授权 + IRQ→EventQ + DMA 缓冲契约 + 所有权 claim/release |
| 内核块代理（udisk） | `syscall 0x0C05/0x0C06` | 页缓存留内核，块请求经共享环转发给用户驱动 |
| 统一驱动框架 | `kernel/drivers/core/driver_core.c` | `driver_t` 模块模型、class 设备、DriverStore 扫描 |
| drvmod 模块加载 | `kernel/drvmod/loader.c` | ELF ET_REL 装载 + `.a20drv` 描述段 + DriverEntry |
| 双态部署环境层 | `kernel/include/drivers/dual/drv_env.h` | 同一驱动源码按 `DRV_ENV_KERNEL/USER/DRVMOD` 编译 |
| IOMMU 硬件隔离 | `kernel/drivers/core/riscv_iommu.c` | RISC-V IOMMU DDT/CQ/FQ 编程，devid 0 SV39 翻译域 |
| 对象统计与配额 | `/proc/a20/objects` | 七项实时对象计数 + 累计计数审计 + 句柄硬配额 |
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

所有用户可见资源（文件、通道端点、事件队列、VMO、设备等）由 handle 引用，类型化对象带引用计数与销毁回调。服务崩溃后，内核按对象模型回收其全部资源（句柄表销毁 → 对象引用释放 → 类型化回收），七项实时对象计数器回归基线。

### 性能与隔离的边界可论证

- **留内核**：调度、MM、VFS、页缓存、TCP 数据面——高频、延迟敏感、崩溃后果严重。
- **迁用户态**：服务监管、设备驱动（低速）、协议/注册类——崩溃频繁、可重启、性能非关键。
- **主存储数据面不外迁**：TCG 数据显示块/网驱动的 I/O 路径由多次上下文切换 + 数据拷贝构成，在现有调度成本下外迁必然劣化，故主存储 virtio-blk 数据面保留内核态；scratch 设备（udisk）仍可外迁演示（详见 [02-mainstream-plan.md](02-mainstream-plan.md) 的决策记录）。

## 已建成的子系统

### 对象与能力（Native ABI 底座）

统一句柄表 + 14 位 rights + 时态约束 + BLP 安全标签；类型化对象生命周期；Channel 句柄传递（`ρ_recv = ρ_send ∩ ρ_transfer`）；EventQ 统一等待（任务退出、设备信号等事件源）。

### IPC 快路径

`channel_call` 一次陷入完成"发送请求 + 等待回复"，单次句柄查找与参数校验；服务端唤醒走 priority-preempt 快路径，两个 ABI 共享。时间片捐赠在此基础上把同步 IPC 的两次调度排队减少为一次直接切换（UP）。

### 服务化与稳定性

svcmgr 以声明式清单拉起服务（stdio 继承 + 固定槽位端点传递），用 EventQ 监控服务退出，按指数退避重启；注册表提供按名解析与崩溃重绑；健康探针（ping + 超时强杀）保证服务真实可用。资源隔离由对象配额 + 泄漏审计保障。

> 命名注意：仓库同时存在 `user/svc/svcmgr.c`（带全局注册表与清单的系统监管者）与 `user/svc/svcman.c`（早期的最小自愈演示，只拉起 echod）。本文档与 [01-mechanisms.md](01-mechanisms.md) 中的"服务监管者"指 `svcmgr`；`svcman` 仅由 `smoke-native-svc` 用作最小回归。

### 共享内存数据面

SPSC 字节环（全相对偏移，跨进程不同虚拟地址可用；acquire/release 游标；futex 门铃以物理页为 key）。非满非空路径零 syscall，是用户态服务的大块数据传输通道。

### 用户态驱动框架

内核把白名单内的设备物理窗口以 PFNMAP 映射进驱动进程，把物理 IRQ 绑定为 EventQ 事件源（电平协议：屏蔽 → 投递 → ack 重新武装），DMA 缓冲只能来自内核分配的 VMO。低速设备（如 goldfish RTC）整体迁入用户态服务，崩溃后由 svcmgr 重启并恢复设备。设备所有权由 `device_claim/release` 仲裁，任务退出时自动清理（`udriver_task_cleanup`）。

### 统一驱动框架与双态部署

`kernel/drivers/core` 提供统一的 `driver_t` 模块模型：DriverStore（`/boot/drivers` 与 `/bin/lib/drivers`）扫描 `.a20drv` 描述段，`drvmod` 把内核态驱动模块装载为 ET_REL，class 设备（char/block/net/input/display/audio）统一注册与 devfs mux。`drv_env.h` 让同一份驱动源码按部署选择编译为内核模块或用户态进程（双态部署），见 [04-dual-placement.md](04-dual-placement.md)。

### IOMMU 硬件 DMA 隔离

`kernel/drivers/core/riscv_iommu.c` 完成 RISC-V IOMMU 的真实硬件初始化：DDT(1LVL)/CQ/FQ 编程与使能，devid 0 配置 SV39 翻译域并经 TR_REQ 验证（已映射 IOVA 精确翻译、未映射 IOVA 被硬件拒绝），devid 1 保持 passthrough。fault 队列消费与 per-device 动态映射为后续工作，见 `smoke-iommu-discovery`。

### Linux ABI 透明获益

vDSO（`clock_gettime`/`gettimeofday`/`getcpu`，与内核 timekeeping 位级一致）；唤醒快路径（pipe/AF_UNIX/futex/channel 的 wake 统一走 priority-preempt）对两个 ABI 自动生效。

### Linux ABI 对 Native 机制的直接消费

- `channel_fd`（`kernel/ipc/channel_fd.c`）：把 channel 端点包装成 fd（`read/write/poll/close`），Linux 程序经 `SYS_a20_channel_pair`（编号 900）与服务注册表 `SYS_a20_registry_client`（编号 901）使用同一 channel 机制；
- `eventfd`/`signalfd`/`timerfd`/`sysv_sem`/`sysv_shm`（`kernel/ipc/`）：ABI 无关的 vfile 后端，Linux 的 `eventfd2/signalfd4/timerfd_*/sem*/shm*` 建立在其上；
- 因此两套 ABI 共享同一套对象/等待/IPC 底座，Linux 侧只是线格式翻译。

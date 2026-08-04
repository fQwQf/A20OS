# A20OS 混合内核改造设计

本文档是 A20OS 从"吸收微内核理念的宏内核"演进为真正混合内核的设计总纲。
配套实施路线见 [01-roadmap.md](01-roadmap.md)。整体架构介绍见
[../OS-Design.md](../OS-Design.md)。

## 1. 目标与原则

把"崩溃代价高、性能不敏感"的子系统迁入受 capability 约束的用户态服务，
同时让性能关键路径保持内核态函数调用。三条硬原则：

1. **不牺牲性能**：任何外迁必须给出迁移前后基准对比；热路径（调度、缺页、
   VFS、页缓存、TCP 数据面）永远留在内核。
2. **不另起炉灶**：复用现有 Native ABI 的 handle/rights、Channel、EventQ、
   VMO/VMAR 机制，只做增量扩展。
3. **故障可恢复**：用户态服务崩溃 = 服务重启，不是整机 panic。

## 2. 现有底座（已具备，无需重建）

代码审计确认以下微内核基础设施已经存在并可用：

| 机制 | 位置 | 说明 |
|------|------|------|
| 统一句柄表 + 14 位 rights | `kernel/abi/native/handle_table.c` | 句柄项内联存储对象指针、类型、rights、时态约束 |
| 类型化对象生命周期 | `kernel/ipc/a20_object.c` | `a20_object_ref/release` 按类型分发 |
| Channel（含句柄传递） | `kernel/ipc/a20_channel.c` | Zircon 风格；`ρ_recv = ρ_send ∩ ρ_transfer` |
| EventQ（统一等待） | `kernel/ipc/a20_event.c` | watch 任意对象；`A20_EVENT_EXITED` 已由 `kernel/proc/exit.c:363` 在任务退出时发出 |
| VMO 跨进程共享 | `kernel/mm/vmo.c` + `vm_share`/channel 传递 | 引用计数的规范页帧持有者 |
| 命名空间 | `kernel/abi/native/sys_native_security.c` | `ns_create/ns_apply` |
| 原生任务派生 | `kernel/abi/native/sys_core.c` `sys_a20_task_spawn` | 一次调用装载 ELF + 传递初始句柄 |
| 用户态 SDK | `user/liba20rt/` | channel/event/mem/handle 的内联封装 |
| SPSC 无锁环（内核内） | `kernel/include/abi/native/ring_spsc.h` | 当前无调用方，可作为共享环参考 |

## 3. 目标架构：三层

```
┌────────────────────────────────────────────────────┐
│ 用户态服务层（可崩溃、可重启、可独立审计）              │
│  netd(协议栈)  blkd(块驱动)  inputd  fsd(具体格式)    │
│  每个服务 = 普通 native 任务 + 显式授予的 handle       │
├────────────────────────────────────────────────────┤
│ 混合内核层（性能关键路径，保持内核态）                  │
│  EEVDF 调度 / MM(VMO·VMAR·缺页) / VFS 核心 / 页缓存   │
│  Channel·EventQ IPC / 驱动框架·中断分发·DMA 授权       │
├────────────────────────────────────────────────────┤
│ 核心可信基（nano-kernel 化方向）                       │
│  陷陷入口 / 上下文切换 / 句柄表 / 对象生命周期           │
└────────────────────────────────────────────────────┘
```

### 3.1 什么留内核，什么迁出去

判定规则：**每秒调用 > 10k 次、或延迟敏感 < 10µs 的路径留内核**；崩溃频繁、
逻辑复杂、协议解析类的迁出。先加 tracepoint 实测，再决定边界。

| 留内核 | 迁用户态 |
|--------|----------|
| 调度器、MM/缺页、VFS 核心、dentry/inode/页缓存 | 具体 FS 格式实现（可选 FUSE-like 服务） |
| 中断分发桩、DMA 缓冲授权 | 驱动业务逻辑（MMIO 映射授权到服务进程） |
| socket 系统调用代理层 | lwIP 协议栈（netd 进程） |
| Channel/EventQ/句柄表 | 服务注册、健康监控、策略（svcman） |

### 3.2 驱动外迁模型

- 内核保留：中断注册/分发（`request_irq` 已有）、`dma_alloc_coherent` 物理页
  分配、MMIO 物理页映射授权（以带 `A20_RIGHT_MAP` 的 handle 授给驱动进程，
  `/dev/fb0` mmap 已是此模式的先例）。
- 数据面：内核与驱动服务共享一块 VMO，里面放请求/完成环（SPSC/MPSC），
  生产消费用 acquire/release 原子序，**正常路径零系统调用**；只有队列
  空/满时才通过 EventQ 或 futex 唤醒。virtio 分割环本来就是跨信任域设计，
  语义直接借用。
- 控制面：走 Channel RPC（见 §4.1 的 `channel_call` 快路径）。
- 可重启：驱动进程只持有 handle 不持有内核指针，崩溃后内核随句柄表销毁
  自动回收全部对象引用；svcman 重新拉起并 replay 注册流程。

## 4. 性能机制（"不牺牲性能"的具体手段）

### 4.1 `channel_call`：融合 RPC 快路径（本项目新增）

微内核 IPC 的经典开销是"两次陷入 + 两次调度"。现有
`a20_channel_send` + `a20_channel_recv` 的 RPC 往返 = 2 次 syscall 陷入 +
2 次句柄查找 + 2 次 park/wake。改造：

- **融合陷入**：一次 `channel_call` 完成 send + 阻塞等回复，RPC 往返陷入
  次数从 4（call 双方各 2 次）降为 2。
- **单次句柄查找**：send 与 recv 阶段共享同一次 `A20_RIGHT_WRITE|READ`
  查找结果。
- **唤醒捐赠**：投递消息时若对端正阻塞在 RECV 等待队列上，走
  `proc_try_wake` 的 priority-preempt 路径（`kernel/proc/park.c` 已有），
  让服务端立即抢占当前 CPU，等效于 L4 的 time-slice donation，避免
  服务端在 runqueue 里排队。

### 4.2 共享 VMO 环形队列（批量数据面）

- 控制消息（< 64B）走 Channel；批量数据一律走共享 VMO + 无锁环。
- 环协议做成 user/kernel 共用的 uapi 头（acquire/release 语义，SPSC 起步），
  参考 `ring_spsc.h` 但改为**相对偏移**而非内核指针，使同一 VMO 在两个
  进程的不同虚拟地址都可用。
- 唤醒合并：生产者只在"消费者可能睡着"时置 doorbell（EventQ 事件），
  消费者睡前复査一次环——类 futex 的"先查后睡"协议。

### 4.3 其他

- 只读高频查询（时钟、cpu 数、uptime）走只读共享页，不进内核。
- 服务间 handle 传递全部走 Channel 的既有 TRANSFER 语义，零拷贝授权。

## 5. 稳定性机制（"微内核优雅"的落地）

1. **svcman 服务监管者**（用户态）：按静态清单拉起服务；用 EventQ watch
   每个服务任务的 `A20_EVENT_EXITED`；崩溃后指数退避重启；重启时通过
   `task_spawn` 的 handles 数组把新的服务端 Channel 端点交回给客户端
   注册表。
2. **崩溃隔离**：服务只能通过显式 handle 访问资源；Channel 值语义天然
   免疫指针注入；BLP 标签与时态句柄（`handle_table.c` 已有）限制横向移动。
3. **资源回收**：服务死亡 → 句柄表销毁 → `a20_object_release` 按类型回收
   VMO/端点/订阅，无内核泄漏。
4. **健康检查**：svcman 周期性 `channel_call` ping；超时未响应判定僵死
   并强杀重启（`task_kill` + `A20_RIGHT_SIGNAL`）。

## 6. 与现有体系的兼容

- Linux ABI 完全不动；全部新机制在 Native ABI 侧增量。
- 现有 93 个 native syscall 不改语义；新增 syscall 走空闲号段。
- 每个阶段提供独立 smoke 目标（见 01-roadmap.md 验收表），任一阶段
  可独立回退。

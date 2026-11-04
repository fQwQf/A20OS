# Native ABI 规范文档索引

A20OS Native ABI 是内核提供的面向对象能力、句柄与异步事件的新一代原生接口规范，旨在为高性能微服务、设备驱动和安全受控环境提供清晰、形式化的系统调用底座。

---

## 规范目录

| 文档 | 说明 |
|---|---|
| [syscalls.md](syscalls.md) | **系统调用速查与调用指南**：全量 175 个系统调用矩阵、全架构陷入约定与 C 代码调用示例 |
| [00-overview.md](00-overview.md) | Native ABI 设计哲学、分层模型与系统调用约定 |
| [01-types.md](01-types.md) | 基础数据类型、可扩充结构体（E-APPEND）与参数传递规则 |
| [02-errors.md](02-errors.md) | 错误码体系与映射规则（`a20_status_t`） |
| [03-handle.md](03-handle.md) | 句柄（Handle）生命周期、权限控制表（Rights）与能力模型 |
| [04-memory.md](04-memory.md) | VMO（虚拟内存对象）与 VMAR（虚拟地址空间区间）内存容器模型 |
| [05-ipc.md](05-ipc.md) | 双向 Channel、共享环（ShmRing）与事件队列（Event Queue）机制 |
| [06-security.md](06-security.md) | 权能最小化模型、预算能力与沙箱隔离 |
| [07-startup.md](07-startup.md) | 原生任务启动流程、CRT0 契约与运行时环境初始化 |
| [09-native-abi-deepening.md](09-native-abi-deepening.md) | ABI 扩展特性：Pager、Monitor、任务控制与事件源扩展 |
| [10-runtime-extensions.md](10-runtime-extensions.md) | 运行时动态扩展与个性化机制 |

> CMSIS-RTOS2 是独立的第三方 ABI，不属于 Native ABI；其人格层、实时模型与实验文档见 [RTOS 文档索引](../rtos/README.md)。Native ABI 侧只定义通用调度原语（`sched_lock`、`task_self`、`task_set_sched` 的捐赠 flags 等）。

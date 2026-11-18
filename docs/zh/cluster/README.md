# 集群子系统文档索引

A20OS 集群子系统提供跨节点与跨架构的统一分布式计算与资源互联机制。它将低功耗 MCU（如 STM32F103、Hi3861）、开发板（如树莓派、RISC-V 开发板）与 x86/ARM 服务器组织在同一套身份、寻址、会话与任务语义之下。

---

## 核心技术规范 (Core Specifications)

| 核心规范 | 涵盖领域 | 说明 |
|---|---|---|
| [**overview.md**](overview.md) | 总体架构与拓扑 | 统一分层模型、节点角色分级与 1.0 语义边界 |
| [**01-abi.md**](01-abi.md) | Native 系统调用 | 集群系统调用契约（0x0520 系列 syscall）与 SDK 接口 |
| [**02-wire-protocol.md**](02-wire-protocol.md) | 线缆帧与协议 | CL 物理帧格式、会话事务、CRC 校验与可靠调用 |
| [**02-udp-demo.md**](02-udp-demo.md) | 联调指南 | 双 QEMU 节点基于 UDP 隧道的联调示例与验证 |
| [**03-kernel-impl.md**](03-kernel-impl.md) | 内核数据面 | 内核集群驱动架构、帧转发、路由表与连接代理 |
| [**04-transports.md**](04-transports.md) | 传输层适配 | 传输层抽象（Loopback、UDP/IPv4、UART SLIP、MCU 叶子） |
| [**05-userspace.md**](05-userspace.md) | 用户态服务 | 用户态服务（clusterd、cluster-agent、jobd）及 SDK |
| [**10-operator-guide.md**](10-operator-guide.md) | 运维与排障 | 部署前提、拓扑规划、监控快照与故障诊断顺序 |
| [**cluster-apps.md**](cluster-apps.md) | 应用与作业 | 原生 Channel 服务开发、分片归约作业与负载实践 |
| [**mcu-shared-kernel.md**](mcu-shared-kernel.md) | MCU 共享内存内核 | 基于共享 RAM 与硬件仲裁的嵌入式对称多处理 (SMP) |
| [**runtime-ssi.md**](runtime-ssi.md) | 单系统映像 (SSI) | 统一执行域、对象目录与租约、跨机透明线程切片 |

---

## 1. 核心架构与总体设计

| 文档 | 说明 |
|---|---|
| [overview.md](overview.md) | 集群总体设计、分层架构与 1.0 语义边界承诺 |
| [runtime-ssi.md](runtime-ssi.md) | 单系统映像（SSI）运行时、分布式命名空间与透明线程切片 |
| [mcu-shared-kernel.md](mcu-shared-kernel.md) | 多 MCU 基于共享 RAM 与硬件仲裁的单内核 SMP 机制 |

## 2. ABI 与用户态编程

| 文档 | 说明 |
|---|---|
| [01-abi.md](01-abi.md) | Native 集群系统调用契约（0x0520 系列 syscall、快照与路由扩展） |
| [05-userspace.md](05-userspace.md) | 用户态服务架构（clusterd、cluster-agent、jobd）及 SDK 绑定 |
| [cluster-apps.md](cluster-apps.md) | 集群分布式应用开发、作业模型（Jobs）与复杂负载集成实践 |

## 3. 协议与传输层实现

| 文档 | 说明 |
|---|---|
| [02-wire-protocol.md](02-wire-protocol.md) | CL 线缆帧协议、包头格式、会话挑战与分片重组 |
| [02-udp-demo.md](02-udp-demo.md) | 双 QEMU 节点基于 UDP 隧道的联调步骤与示例 |
| [03-kernel-impl.md](03-kernel-impl.md) | 内核集群数据面实现、文件职责与事务处理 |
| [04-transports.md](04-transports.md) | 传输适配器抽象（Loopback、UDP/IPv4、UART 主节点、MCU 叶子） |

## 4. 运维与部署

| 文档 | 说明 |
|---|---|
| [10-operator-guide.md](10-operator-guide.md) | 集群部署手册：节点角色配置、拓扑发现、排障顺序与升级策略 |

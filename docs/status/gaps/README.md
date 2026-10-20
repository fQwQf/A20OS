# 1.0 缺口台账

本目录是 A20OS 1.0 **唯一**的缺口清单：所有"还没做到、且属于 1.0 承诺范围"的事情都记录在这里。
参考文档只描述系统现在是什么，不再各自维护待办。

## 用法

- 完成一项，就**删掉对应条目**。本目录清空，表示已承诺范围完成、可以宣布稳定。
- 不设「进行中」状态：一条要么存在（未完成），要么被删除（完成）。中间状态写在
  [current/](../current/) 的现状页里，不属于台账。
- 每条都带**关闭条件**，写的是可验证的事实（哪条门禁、哪个平台、什么行为），不是"继续推进"。
- 条目 ID 为 `<领域>-<序号>`，删除后不回收编号，避免引用错位。

## 什么不算缺口

以下内容**不**进本目录，写在对应参考文档里：

- **有意的设计边界**：例如"这棵树不提供 veth 设备"、"不宣称完整 glibc ABI"。它们是设计选择，
  不是待办；只有被纳入 1.0 承诺的才升级为缺口。
- **架构/硬件前提**：例如"QEMU 没有 H 扩展时内核安全拒绝"。这是行为契约。
- **已完成的里程碑与历史记录**：[history/](../history/) 与 [archive/](../../archive/)。

## 领域划分

| 文件 | 对应 V1 工作包 | 内容 |
| --- | --- | --- |
| [delivery.md](delivery.md) | V1-DELIVERY | 需求台账、候选产物、CI、构建矩阵、门禁基线 |
| [core.md](core.md) | V1-CORE | 调度、同步、任务生命周期、陷阱诊断 |
| [mm.md](mm.md) | V1-MM | 虚拟内存事务、fault、回收、COW |
| [fs.md](fs.md) | V1-FS | ext4/JBD2、NTFS、ISOFS、UXFS、持久化 |
| [abi-ipc.md](abi-ipc.md) | V1-ABI | Linux/Native ABI、IPC、信号、IDL |
| [net.md](net.md) | V1-NET | socket、lwIP、NAT、IPv6、嵌入式档预算 |
| [drivers.md](drivers.md) | V1-DRIVER | 设备生命周期、IRQ/DMA/IOMMU、双态部署 |
| [runtime.md](runtime.md) | V1-RUNTIME | libc、动态链接、exec 继承、Java |
| [cluster.md](cluster.md) | V1-CLUSTER | 成员、对象目录、服务恢复、负载 |
| [hyp.md](hyp.md) | V1-HYP | RISC-V H-extension 生命周期；其余后端另行立项 |
| [desktop.md](desktop.md) | V1-DESKTOP | AArch64 桌面竞态、交互负载、音视频 |
| [security.md](security.md) | V1-SECURITY | 能力、撤销、配额、隔离、负控 |
| [docs.md](docs.md) | — | 文档与门禁自身的已知缺口 |

`requirements-2026-10-14.md` 是 2026-10-14 人工 triage 的二十条需求的原始表，作为已并入本台账的
逐条明细保留；后续以各领域文件为准。

# 虚拟化与 Hypervisor 子系统 (Hypervisor Subsystem)

A20OS Hypervisor 子系统提供轻量级硬件辅助虚拟化能力，基于共享 VM / vCPU 核心抽象支持主流硬件架构后端（RV64 H、RV32 H、AArch64 EL2、ARM32 HYP、AMD SVM 与 Intel VMX）。

---

## 规范与文档导航

| 领域 | 核心文档 | 内容概要 |
| :--- | :--- | :--- |
| **系统架构** | [overview.md](overview.md) | **架构总览**：轻量级地基定位、Stage-2 单级内存复用、页帧出借语义与设备模型 |
| **部署与操作** | [03-usage.md](03-usage.md) | **使用指南**：构建流程、QEMU / Bochs 运行配置、用户态测试与故障排查 |
| **架构后端** | [08-multiarch-backends.md](08-multiarch-backends.md) | **多架构后端**：RV64/RV32/AArch64/ARM32/x86 硬件虚拟化接口与寄存器边界 |
| **虚拟 SBI 规范** | [10-riscv-sbi.md](10-riscv-sbi.md) | **RISC-V SBI**：Base / DBCN / HSM / RFENCE / IPI / TIME 规范实现与测试断言 |
| **宿主与控制平面** | [06-host-runtime.md](06-host-runtime.md) | **宿主运行时**：实例隔离、资源命名空间与生命周期管理 |
| | [07-guest-supervisor.md](07-guest-supervisor.md) | **监管协议**：控制平面协议、动态装载与监控管道 |
| **多核验证** | [09-smp-validation.md](09-smp-validation.md) | **运行验证**：多架构双核 SMP 运行矩阵、正反向门禁判据与环境复现 |
| **代码契约规范** | [00-design.md](00-design.md) | 基础设计契约（C 代码显式引用） |
| | [01-a20os-guest.md](01-a20os-guest.md) | Guest 运行协议契约（C 代码显式引用） |
| | [02-roadmap.md](02-roadmap.md) | 特性依赖与架构演进切片 |
| | [04-platform-design.md](04-platform-design.md) | 板级平台模拟与总线设计参考 |

---

## 运行配置与门禁复现

当前运行配置与门禁复现请参考 [使用指南](03-usage.md) 与 [运行验证](09-smp-validation.md)。

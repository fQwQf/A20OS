# Hypervisor 文档入口

A20OS 提供共享 VM/vCPU 核心与多架构硬件后端。当前可运行 RV64 H、RV32 H、
AArch64 EL2、ARM32 HYP、AMD SVM 和 Intel VMX；LoongArch LVZ 为默认关闭的
实验实现。多架构小载荷双 vCPU SMP 已验证，完整 OS guest 工作流仍面向 RV64。

- [使用指南](03-usage.md)：依赖、构建、QEMU / Bochs 运行、输出判据、用户态
  `hyp_test`、VMM/SMP 调用顺序、RISC-V 管理器、排查与清理。
- [后端能力](08-multiarch-backends.md)：guest 模式、内存与架构接口边界。
- [运行验证](09-smp-validation.md)：本轮实际执行矩阵、SMP 方法、限制与复现。
- [验证证据](validation-2026-10-25.json)、[main 复测](validation-main-2026-10-25.json)：
  镜像/日志 SHA-256 与 PASS/SKIP 标记。
- [实现状态](../status/current/hypervisor-implementation-status.md)：当前能力、
  完整 guest 既有问题与未完成项。
- [命名宿主](06-host-runtime.md)、[guest supervisor](07-guest-supervisor.md)：
  RV64 完整 guest、实例隔离、控制协议与生命周期。

[基础设计](00-design.md)、[A20OS guest 设计](01-a20os-guest.md)、
[路线图](02-roadmap.md) 与 [平台设计](04-platform-design.md) 包含历史描述或
未来目标。使用方式与验收范围以本目录的使用指南、后端说明和验证记录为准。
VMX 本轮使用 Bochs，没有 Intel 实机验证；LoongArch 没有 LVZ 正控执行证据。

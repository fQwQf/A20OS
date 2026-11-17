# A20OS 改进 TODO

1.0 的总范围与完成标准以 [发布计划](../releases/v1.0.md) 为准：本页不是全部工作包，集群、虚拟化、运行时、桌面等已推进子系统还须按各自设计完成。Rust 重写与这些缺口一起收口；既有行为和性能按 [对照规范](../releases/v1.0-validation.md) 验收。 LoongArch32/PPC64LE 已退出 1.0；其活跃实例、构建和 CI 配置清理已完成，历史源码与复现入口保留。

本文记录尚未完成的工程瓶颈与剩余规划。实现细节与验证入口见事实文档（[../testing-gates.md](../testing-gates.md)、[../security/hardening.md](../security/hardening.md)、[../../kernel/abi/linux/syscall_coverage.md](../../../kernel/abi/linux/syscall_coverage.md)），远期方向评估见 [next-horizon.md](next-horizon.md)。

面向服务器部署的当前能力边界与阻塞项排序见[../status/current/server-readiness.md](../status/current/server-readiness.md)。

checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；带日期的验证记录如何引用见文末"验证环境说明"。

当前已完成单核/SMP2 调度进展的有界负载验收，记录见 [本轮进度](../status/current/v1.0-progress.md#本轮完成与验证2026-11-16)；默认 ABI=both 的候选基础 CI 已通过。

N18 关键并发和 N20 本地交付已按[当前候选矩阵](../status/current/v1.0-candidate.md)关闭；六架构用户态/SMP2、共享 VFS/MM 的 SMP8、两个发布档位、NOMMU build-only 和 MCU 预算分别记录，不要求每个架构重复所有负载。当前已收窄的 1.0 交付项已全部关闭。

## 当前状态

候选 `94bbdbb12` 的[基础 CI](https://github.com/fQwQf/A20OS/actions/runs/44072919246)已通过：基础检查、六架构内核编译和一次 QEMU 启动。原 N02 已从待办删除；没有剩余的 1.0 编号交付项。

按项目负责人要求，完整 Native CI、扩展压力和发布逐镜像认证不再是 1.0 必须项。专项在修改相关代码时按需运行；当前本地矩阵和历史云端失败均见[候选记录](../status/current/v1.0-candidate.md)。只运行上述一次精简流程，没有恢复完整专项或发布验收矩阵。

## 验证环境说明

- 工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告，本文不固化某一台 host 的工具缺失状态。
- 各项验证以当前源码构建与运行时门禁结果为准。
- Proc/Sched 的当前聚合门禁是 `make check-doc-test-gates`，包含构建与 QEMU 运行；双架构 debug/release、 1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与 LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一使用 Python 3 标准库；长跑基准入口负责记录 QEMU 命令、镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的历史成功列表代替当前运行结果。

# A20OS 改进 TODO

1.0 的总范围与完成标准以 [发布计划](../releases/v1.0.md) 为准：本页不是全部工作包，集群、虚拟化、运行时、桌面等已推进子系统还须按各自设计完成。Rust 重写与这些缺口一起收口；既有行为和性能按 [对照规范](../releases/v1.0-validation.md) 验收。 LoongArch32/PPC64LE 已退出 1.0；其源码、实例、构建和 CI 清理归入 V1-DELIVERY。

本文记录尚未完成的工程瓶颈与剩余规划。实现细节与验证入口见事实文档（[../testing-gates.md](../testing-gates.md)、[../security/hardening.md](../security/hardening.md)、[../../kernel/abi/linux/syscall_coverage.md](../../../kernel/abi/linux/syscall_coverage.md)），远期方向评估见 [next-horizon.md](next-horizon.md)。

面向服务器部署的当前能力边界与阻塞项排序见[../status/current/server-readiness.md](../status/current/server-readiness.md)。

checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；带日期的验证记录如何引用见文末"验证环境说明"。

当前已完成单核/SMP2 调度进展的有界负载验收，记录见 [本轮进度](../status/current/v1.0-progress.md#本轮完成与验证2026-11-16)；默认 ABI=both 的云端候选复验仍保留。

## P0：默认 ABI=both 的运行验收

已运行的 handle、MM、deepen、contract 用例覆盖权限上限、区域约束、对象生命周期和 `remap-owner-rollback PASS`，通过结果与配置见测试日志。

- [ ] 在云端复验 Native CI 与发布前置门禁
  - `.github/workflows/native.yml` 已接线；当前仅验证本地配方，未触发云端 CI 或发布。
  - RTC、服务注册表 RTC 重启和 UBD 依赖 RISC-V 私有 MMIO/IRQ，AArch64 明确跳过，跳过不计为覆盖。AArch64 PTY 与 timeout 新增入口本地已通过。

## P1：既有驱动的安全与恢复

- [ ] 收口已有支持的常见 PC/QEMU 设备的 DMA 归属、越权拒绝、在途取消及停止失败回收
  - 范围以[常见 PC/QEMU 驱动](../drivers/meta/v1-pc-qemu-scope.md)为准，不逐项完善 Hi3861 等开发板外设，也不要求所有厂商的实体故障认证。
  - 当前实现与已通过的样板见 [驱动现状](../status/current/drivers-implementation-status.md)。
  - 剩余安全关闭条件见 [驱动缺口](../status/gaps/drivers.md)，同源双态、MSI-X 扩展与 DRM/GPU 新功能另见 [后续路线](next-horizon.md#后续版本功能强化)。

## P1：发布配置与工具链

- [ ] 正式支持矩阵内的发布配置与工具链复核
  - 历史构建记录已包含 x86_64、LoongArch64、 RISC-V32 等内核构建；这些是指定工作区的构建证据，不是完整运行或所有板级零警告证明。
  - 在候选工具链上按正式支持矩阵逐行复核，记录配置、警告策略和产物；实验配置单列验证状态，正式支持行缺少证据时仍列待验收。
  - LoongArch32/PPC64LE 不再作为待补支持项，转为 V1-DELIVERY 的活跃配置清理。

## 验证环境说明

- 工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告，本文不固化某一台 host 的工具缺失状态。
- 各项验证以当前源码构建与运行时门禁结果为准。
- Proc/Sched 的当前聚合门禁是 `make check-doc-test-gates`，包含构建与 QEMU 运行；双架构 debug/release、 1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与 LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一使用 Python 3 标准库；长跑基准入口负责记录 QEMU 命令、镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的历史成功列表代替当前运行结果。

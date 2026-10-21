# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-01 长内核路径上的有界进展

- 现状：RT/调度预算与抢占已有实现；长 syscall 期间普通任务的有界进展没有断言。
- 缺口：线程化 IRQ 与长内核路径的有界延迟未完成，也没有风暴/故障用例。
- 关闭条件：普通任务进展断言在单核与 SMP 上通过，长 syscall 与 IRQ 风暴下不误触；延迟分布可复现。
- 来源：requirements N01。

### CORE-04 锁拆分后的竞争未复测

- 现状：`proc_lock` 已删除，调度状态归 per-task `park_lock`，低频残余归 `tasklist_lock`；`g_net_lock` 已分片。拆分后的采样已经做了：[measurements/smp-lock-contention-now.md](../measurements/smp-lock-contention-now.md) 记录 4 vCPU 下 `mm_stress` 的争用主体落在 `park_lock`（2270 次获取）而 `tasklist` 只剩 7 次，以及 `futex_stress` 仍把 `tasklist` 打到 430 次获取 / 550 万次自旋。
- 缺口：B1 基线（只打注册补丁、未拆分）从未采集，所以"拆分带来多少收益"仍不可分辨；`-ENOTCONN` 窗口在 20 次引导里一次未命中。
- 关闭条件：采集 B1 基线，按同一协议复测拆分后的竞争并给出可分辨结论；`-ENOTCONN` 窗口要么命中并修复，要么给出排除依据。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八、[measurements/lock-after.md](../measurements/lock-after.md)。

### CORE-06 futex PI 的窄窗口

- 现状：prepare 后 fork/COW 窗口已补持锁可写复查与解锁重试，`pi-cow-isolation` 在双架构单核/四核通过。
- 缺口：确定性窄窗口故障注入仍缺失，无法证明该窗口被覆盖。
- 关闭条件：注入在 COW 后、解锁重试路径上命中并验证不丢唤醒；负控失败。
- 来源：原 `known-issues.md` 当前工程队列。

### CORE-07 arm32 的 FP 上下文

- 现状：riscv32 与 ppc64le 已补 trap 帧的 FP 保存（ppc64le 仅编译验证）；arm32 未修。
- 缺口：arm32 用户态启用 VFP（`-mfpu=vfpv3-d16 -mfloat-abi=hard`），trap 帧与 `trap.S` 用逐字段断言强绑定，改动未验证；本机无 arm 工具链。
- 关闭条件：arm32 上 FP 上下文切换用例通过；或明确降级为不支持并写入平台契约。
- 诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)。

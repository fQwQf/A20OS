# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-04 锁拆分后的竞争未复测

- 现状：`proc_lock` 已删除，调度状态归 per-task `park_lock`，低频残余归 `tasklist_lock`；`g_net_lock` 已分片。拆分后的采样已经做了：[measurements/smp-lock-contention-now.md](../measurements/smp-lock-contention-now.md) 记录 4 vCPU 下 `mm_stress` 的争用主体落在 `park_lock`（2270 次获取）而 `tasklist` 只剩 7 次，以及 `futex_stress` 仍把 `tasklist` 打到 430 次获取 / 550 万次自旋。
- 缺口：B1 基线（只打注册补丁、未拆分）从未采集，所以"拆分带来多少收益"仍不可分辨；`-ENOTCONN` 窗口在 20 次引导里一次未命中。
- 关闭条件：采集 B1 基线，按同一协议复测拆分后的竞争并给出可分辨结论；`-ENOTCONN` 窗口要么命中并修复，要么给出排除依据。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八、[measurements/lock-after.md](../measurements/lock-after.md)。

### CORE-07 arm32 的 FP 上下文

- 现状：riscv32 与 ppc64le 已补 trap 帧的 FP 保存（ppc64le 仅编译验证）；arm32 未修。
- 缺口：arm32 用户态启用 VFP（`-mfpu=vfpv3-d16 -mfloat-abi=hard`），trap 帧与 `trap.S` 用逐字段断言强绑定，改动未验证；本机无 arm 工具链。
- 关闭条件：arm32 上 FP 上下文切换用例通过；或明确降级为不支持并写入平台契约。
- 诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)。

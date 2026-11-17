# ABI / IPC / SIGNAL 缺口

对应 V1-ABI。现状见 [current/module-status.md](../current/module-status.md)、[current/native-abi-runtime-status.md](../current/native-abi-runtime-status.md)。

本包关闭了 ABI-01..ABI-10，验证入口：

- **ABI=linux / ABI=both 交叉断言与 Linux/Native 逐操作差分**：`make smoke-abi-behavior-linux`、`make smoke-abi-behavior-both`；预期值在`tools/tests/abi_behavior_expected.txt`，差分器为 `tools/abi_behavior_diff.py`。
- **IDL 版本协商与畸形 ACK 矩阵（含 legacy 回退的接受与拒绝双向控制）**：`make smoke-idl-ack-aarch64`（AArch64，10 例）；RISC-V64 SMP2 用`tools/xlator/run-idl-ack-matrix.sh riscv64` 跑同一矩阵。2026-11-16 已修复公开入口路径并完成 AArch64 实际 guest 门禁，见 [本轮记录](../current/v1.0-progress.md#本轮完成与验证2026-11-16)。未知值契约见[05-idl-and-personality.md](../../hybrid-kernel/05-idl-and-personality.md) §1.5。
- **UFFD 剩余路径**：`make smoke-syscall-ext`、`make smoke-syscall-ext-smp2`（unregister liveness、close-while-parked、fd 复用、非匿名与权限拒绝）。
- **keyring 权限与命令**：`make smoke-keyring-rust`、`make smoke-keyring-c`；契约见 [kernel/linux-abi/keyring.md](../../kernel/linux-abi/keyring.md)。
- **子进程退出矩阵**：`make smoke-child-exit-matrix`（SMP2），加`make smoke-sigchld-group`；组合表见[kernel/linux-abi/child-exit-notification.md](../../kernel/linux-abi/child-exit-notification.md)。

设计边界（有意不做，写在参考文档而非缺口）：keyring 的 `nsproxy`/keyring namespace 与未实现的 `keyctl` 命令、Linux/Native handle 空间差异、Native rights 与 Linux uid/gid 权限模型的差异，以及 [Native 检查点式通知](../../native-abi/checkpoint-signals.md)。任意执行点的 Native 异步处理器不列为未完成项，也不转入后续功能队列。

本领域的编号条目已清空。控制终端、后台作业访问与特权挂断/夺取已在 RV64 SMP2 / AArch64 SMP4 重复冷启动验证，契约与入口见[控制终端与作业控制](../../kernel/linux-abi/tty-jobcontrol.md)。目标应用所需的关键 syscall 边界已按[应用契约记录](../../kernel/linux-abi/application-contracts.md#应用关键边界收口2026-11-17)关闭原 N08；本地候选 N18/N20 已按[候选记录](../current/v1.0-candidate.md)关闭，当前候选云端验收仍归 [N02](triage-matrix.md)。

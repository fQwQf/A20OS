# ABI / IPC / SIGNAL 缺口

对应 V1-ABI。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)、
[current/native-abi-runtime-status.md](../current/native-abi-runtime-status.md)。

当前没有未关闭的 1.0 条目。最近一轮（2026-10-22）关闭的验证入口：

- **ABI=linux / ABI=both 交叉断言与 Linux/Native 逐操作差分**：
  `make smoke-abi-behavior-linux`、`make smoke-abi-behavior-both`；预期值在
  `tools/tests/abi_behavior_expected.txt`，差分器为 `tools/abi_behavior_diff.py`。
- **IDL 版本协商与畸形 ACK 矩阵（含 legacy 回退的接受与拒绝双向控制）**：
  `make smoke-idl-ack-aarch64`（AArch64，10 例）；RISC-V64 SMP2 用
  `tools/run-idl-ack-matrix.sh riscv64` 跑同一矩阵。未知值契约见
  [05-idl-and-personality.md](../../hybrid-kernel/05-idl-and-personality.md) §1.5。
- **UFFD 剩余路径**：`make smoke-syscall-ext`、`make smoke-syscall-ext-smp2`
  （unregister liveness、close-while-parked、fd 复用、非匿名与权限拒绝）。
- **keyring 权限与命令**：`make smoke-keyring-rust`、`make smoke-keyring-c`；
  契约见 [native-abi/keyring.md](../../native-abi/keyring.md)。
- **子进程退出矩阵**：`make smoke-child-exit-matrix`（SMP2），加
  `make smoke-sigchld-group`；组合表见
  [kernel/linux-abi/child-exit-notification.md](../../kernel/linux-abi/child-exit-notification.md)。

设计边界（有意不做，写在参考文档而非缺口）：keyring 的 `nsproxy`/keyring namespace
与未实现的 `keyctl` 命令、Linux/Native handle 空间差异、Native rights 与 Linux
uid/gid 权限模型的差异。

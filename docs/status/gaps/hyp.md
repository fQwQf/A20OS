# HYPERVISOR 缺口

对应 V1-HYP 的既有 RISC-V H-extension 范围。现状见
[current/hypervisor-implementation-status.md](../current/hypervisor-implementation-status.md)。

### HYP-01 生命周期重复与资源回收

- 现状：单 guest 启动、vcpu、控制台与 shell 门禁已有。
- 缺口：同宿主第二轮多 guest 启动超时、pause/resume 后退出、双 VM × 双 vCPU SMP 创建/停止/资源清理未闭合；100 次 create/destroy 循环未完成。
- 关闭条件：同宿主完成 100 次循环（可有界分批，但不靠重启宿主掩盖泄漏）；资源计数回到基线。
- 来源：[current/hypervisor-implementation-status.md](../current/hypervisor-implementation-status.md)、
  [current/v1.0-progress.md](../current/v1.0-progress.md) V1-HYP 行。

### HYP-02 故障与低内存回滚

- 现状：正常路径有自检；失败路径覆盖不足。
- 缺口：低内存、stage2 映射失败、设备映射失败后的回滚与资源归还未验证。
- 关闭条件：每个失败阶段都能回滚到无泄漏状态，负控能复现。
- 来源：[current/hypervisor-implementation-status.md](../current/hypervisor-implementation-status.md)。

### HYP-03 其他后端（不属于本包）

- 现状：本包只闭合既有 RISC-V H-extension 生命周期。
- 缺口：AArch64 EL2、Intel VMX、AMD SVM、`/dev/kvm`、用户态设备模型与完整 Linux guest 均未实现。
- 关闭条件：这些需要独立立项；1.0 范围文档必须明确保留或排除，不能静默消失。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md)、PLAN 的明确继续开发事项。

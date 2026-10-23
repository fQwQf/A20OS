# HYPERVISOR 缺口

对应 V1-HYP 的既有 RISC-V H-extension 范围。现状见
[current/hypervisor-implementation-status.md](../current/hypervisor-implementation-status.md)。

### HYP-01 生命周期重复与资源回收

- 现状：单 guest 启动、vcpu、控制台与 shell 门禁已有；正常停止后 VM/vCPU/
  映射/页表计数回到基线。已修复宿主 tick 被每次 RUN 推迟导致的调度停摆，
  managed RUN 现在在 guest trap 边界按宿主时间片主动让出。
- 缺口：同宿主第二轮多 guest 仍会超时——管理进程可能连续数十秒得不到唤醒
  （跨领域阻塞，见 [runtime.md](runtime.md) `RT-01`），RPC 3 秒超时后探针失败；
  pause/resume 后退出仍不稳定；双 VM × 双 vCPU guest 能到 2/2 online，但随后
  在 guest 侧 trap frame 上出现 `sstatus` 损坏（SPP 变成栈指针），根因未定位；
  100 次 create/destroy 循环未完成。
- 关闭条件：同宿主完成 100 次循环（可有界分批，但不靠重启宿主掩盖泄漏）；
  资源计数回到基线；双 vCPU guest 正常进入/退出。
- 来源：[current/hypervisor-implementation-status.md](../current/hypervisor-implementation-status.md)、
  [current/v1.0-progress.md](../current/v1.0-progress.md) V1-HYP 行。

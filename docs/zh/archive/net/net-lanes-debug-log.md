# 网络多 Lane 并发早期调试与 Panic 排查实录 (Archive)

> [!NOTE]
> 本文档归档多 Lane 网络栈在早期研发阶段（阶段 C/D，2026-10）遇到的 LISTEN PCB 竞争崩溃、中断下半部死锁排查与二分调试记录。当前生产网络并发架构请以权威规范 [docs/zh/net/net-lanes.md](../../net/net-lanes.md) 为准。

---

## 1. 引导期 Panic 现场与收窄过程

在多 Lane 与真实 LISTEN PCB 交互测试中，曾观察到间歇性崩溃：

```
make ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 dev-build
# 以 a20.tcpmode=lwip 启动
```

结果在 idle 任务（pid=0）、CPU 0 引导期间发生 Page Fault，栈回溯为 `ethernet_output -> tcp_pcb_remove`，`BADV=0x100000000000038`，链表指针被破坏。

### 排查收窄矩阵

| lane | CPU | 真实 LISTEN pcb | 结果 |
|------|-----|----------------|------|
| 4 | 4 | 有 | panic（门禁中） |
| 4 | 4 | 有 | 正常（单跑） |
| 4 | 1 | 有 | 正常 |

触发条件收窄为：多 lane + 多 CPU + 真实 LISTEN PCB，症状为间歇性并发竞态，而非确定性越界。

---

## 2. 锁契约与断言排查

初期排查曾怀疑 `g_lwip_lock` 遗漏获取。通过在 `LWIP_ASSERT_CORE_LOCKED()` 接入运行时持锁跟踪后，记录到初始化阶段的 23 次调用点，均发生在协议栈就绪前的一致性引导路径（`netif_init` / `netif_add`），收包、accept 与 PCB 增删路径无锁违规。

深入排查后确认：崩溃根因位于 loopback 收包路径上的 pbuf 生命周期与并发修改时序。

---

## 3. IRQ 与 Bottom-Half 调度死锁规避记录

在尝试将网络接收纯粹出中断时，曾发现若 IRQ 仅入队而不调度轮询，会产生死锁：阻塞读取的任务依赖 bottom-half 唤醒，而 bottom-half 需要队列排空，读者未唤醒前 poll 无法触发。

最终解法是在 `kernel/core/progress.c` 的 `kernel_progress_run_bottom_halves()` 显式增加无条件 poll 点，在每次调度决策与时钟 tick 时主动触发 drain。

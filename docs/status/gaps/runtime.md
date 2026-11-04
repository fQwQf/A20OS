# RUNTIME 缺口

对应 V1-RUNTIME。动态链接、exec 继承、kexec 边界与 NOMMU 平台矩阵的现状见[current/native-runtime-1.0.md](../current/native-runtime-1.0.md)。

### RUNTIME-06 多 guest 负载下睡眠唤醒被成秒推迟

- 现状：单 guest 时 `nanosleep(5 ms)` 正常；同一宿主上再启动一台 managed guest 后，`a20-vmd` 的 5 ms 睡眠会规律性地变成约 1 s，偶发数十秒。同一进程的第二个（心跳）线程观察到同样的间隔，说明不是某个 syscall 阻塞；同一时间宿主 shell 的 `echo` 往返仍是 10 ms 量级，所以也不是整机停摆。
- 缺口：等待定时器的到期唤醒与/或 EEVDF 唤醒抢占在 guest 运行期间会丢失或大幅延迟。后果是单线程 VM 管理进程无法在 3 s RPC 超时内响应 START/LIST，表现为「第二轮/多 guest 启动超时」，并会连带 pause/resume 后的退出时序。
- 关闭条件：固定负载下（两台 managed RAMFS guest 启动/退出）`a20-vmd` 的`nanosleep(5 ms)` 最大偏差有上界且可复现，STOP/START/LIST 的 RPC 往返不再超时；负控能区分「定时器唤醒丢失」与「任务不可运行」。
- 来源：HYPERVISOR 收口时实测；证据在[current/hypervisor-implementation-status.md](../current/hypervisor-implementation-status.md)的「重复运行」一节。对应实现位于 `kernel/proc/timer_heap.c` 与`kernel/proc/sched.c`，不属于 hypervisor 包。

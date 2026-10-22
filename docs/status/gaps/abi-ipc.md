# ABI / IPC / SIGNAL 缺口

对应 V1-ABI。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)、
[current/native-abi-runtime-status.md](../current/native-abi-runtime-status.md)。

### ABI-01 ABI=linux 与 ABI=both 的行为交叉断言

- 现状：两种构建都有针对性用例；源码 guard 不比较运行时行为。
- 缺口：没有同一用例在两种档位下逐项比较成功、错误、权限与副作用。
- 关闭条件：同一 fixture 在两种档位运行，归一化结果与独立预期值一致；差异项有明确契约。
- 来源：requirements N03。

### ABI-02 IDL 版本协商的覆盖

- 现状：C/Rust 双绑定、重叠/不相交版本、畸形 ACK 与 legacy 回退在 RV64 与 x86_64 已测。
- 缺口：更广的保留架构/ABI，以及其他消息族的未知值契约未覆盖。
- 关闭条件：同样的精确线格式拒绝与 legacy 回退用例在保留架构/ABI 上通过；其他消息族的未知值有明确行为。
- 来源：requirements N04。

### ABI-03 Linux personality 的语义子集

- 现状：fd table、mmap/VMO、sockets、epoll 的 Native 原语实现存在。
- 缺口：这些子系统在 Linux personality 下的语义等价没有逐操作对照。
- 关闭条件：逐操作语义差异测试 + guest 负载；功能契约通过后才有性能结论。
- 来源：requirements N06。

### ABI-04 syscall 覆盖提升

- 现状：`kernel/abi/linux/syscall_coverage.md` 记录逐项等级；部分区域只覆盖正常路径。
- 缺口：边界情况、保留位、错误码未被执行，不能凭相邻 syscall 推断区域完成。
- 关闭条件：每批选一个部分区域，表内链接的边界/错误测试实际运行后更新覆盖。
- 来源：requirements N08。

### ABI-05 UFFD 与 signal 的剩余路径

- 现状：UFFD 的旧 C 竞态对照失败、Rust 路径通过；signalfd/signal 有针对性用例。
- 缺口：UFFD unregister/fault liveness、fd reuse、权限拒绝与对象生命周期的组合未完整覆盖。
- 关闭条件：unregister 后不再 fault、fd 复用不串对象、拒绝路径无副作用；SMP2 下通过。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### ABI-06 keyring 的语义缺口

- 现状：add/replace/request/search/link/unlink 等已实现，属主与 `CAP_SYS_ADMIN` 检查生效。
- 缺口：存储的权限掩码不参与 read/search/link 约束；`KEYCTL_CLEAR`/`KEYCTL_INVALIDATE` 等未实现；无 `nsproxy`；capabilities 仅 15 个子集。
- 关闭条件：权限位真正生效；未实现命令的行为在文档里是明确契约而非遗漏；相关用例通过。
- 来源：[native-abi/keyring.md](../../native-abi/keyring.md)。

### ABI-07 子进程退出通知的完整性

- 现状：Linux `SIGCHLD`/wait 与 Native exit 事件有实现。
- 缺口：线程组、vfork、parent-death 与 `CLONE_PARENT` 的组合没有完整矩阵。
- 关闭条件：每种组合都有 guest 用例，退出与回收恰好一次。
- 来源：[kernel/linux-abi/child-exit-notification.md](../../kernel/linux-abi/child-exit-notification.md)。

### ABI-08 终端没有 controlling terminal 与会话归属

- 现状：会话与进程组有了（`task_t.sid`/`pgid`，`setsid`/`setpgid`），tty 行规也有了（ISIG 在接收路径上判定并向后台上报，见 [drivers/guide/lock-order.md](../../drivers/guide/lock-order.md)）。但终端本身不记录"属于哪个会话、前台是哪个进程组"：控制台的前台进程组是 `kernel/drivers/char/uart.c` 里的一个全局值；`TIOCSCTTY` 在 pty 上直接返回 0、在控制台上是 `-ENOTTY`；`vhangup` 因"没有 controlling terminal"而拒绝（[abi/linux/syscall_coverage.md](../../../kernel/abi/linux/syscall_coverage.md)）。
- 缺口：会话首进程退出时不发 SIGHUP，也不对会话内进程补 SIGCONT；后台进程组读写终端不受 SIGTTIN/SIGTTOU 约束；pty 的 `pgrp` 只能由 `TIOCSPGRP` 直接写入，没有"取得/放弃控制终端"这一步；控制台与 pty 各自实现一套，而不是同一条契约。
- 阻塞点（2026-10 实测）：`ping` 运行期间按 ^C 完全没有效果——终端 termios 是对的（子进程里 `stty -a` 显示 `isig icanon …，intr = ^C`），`tcsetpgrp` 也生效（`TIOCSPGRP` 把控制台前台组设成该作业的 pgid），但**没有任务在读控制台时输入不被处理**：ISIG 判定在接收路径上，而接收路径只在有读者轮询 16550 时被走到（`arch_uart_init` 已写 IER=0x01、PLIC 也 enable 了 IRQ 10，所以要么 RX 中断实际没进来，要么处理函数没被调用，尚未定位）。后果是前台作业不读终端时 ^C 只能等它结束后被 shell 读到，这时前台组已经回落到 shell 自己。修这一项要先让控制台输入变成中断驱动（或由 tick 代 poll），ISIG 才可能在"没人读"的时候生效；驱动侧的这一项单独记在 [drivers.md](drivers.md) DRIVER-08。
- 关闭条件：`TIOCSCTTY(0)`/`TIOCNOTTY` 按 Linux 语义生效（只有会话首进程能取得，重复取得 `EPERM`，steal 位可夺取）；会话首进程退出时向该终端的前台进程组发 SIGHUP、向会话内其余进程发 SIGCONT，并解除归属；后台进程组读终端收到 SIGTTIN（写且置 `TOSTOP` 时收到 SIGTTOU）；`vhangup` 有真正可撤销的终端；上述行为各有 guest 门禁覆盖。
- 来源：`kernel/fs/devfs/tty.c`、`kernel/drivers/char/{uart,pty}.c`、[abi/linux/syscall_coverage.md](../../../kernel/abi/linux/syscall_coverage.md)。
### ABI-09 `a20_envelope_acquire_race_test` 未登记

- 现状：`check-abi-boundary` 报 `missing annotations: a20_envelope_acquire_race_test`；`check-envelope-coverage` 的生成器以 `unclassified syscall: a20_envelope_acquire_race_test` 退出。该符号是随 Rust 信封工作一起进来的测试钩子（`stamps.py`、`ENVELOPE_ACQUIRE_RACE_TEST`、`BUILD_VARIANT` 都有它的位置）。
- 缺口：新增的调用/测试钩子没有进 ABI 注解表与信封覆盖分类，两条 CI 宿主侧门禁为红。
- 关闭条件：该符号被登记并分类后两条门禁通过。
- 来源：DELIVERY 的 CI `toolchain-gates` job 本地复现（[current/delivery-baseline.md](../current/delivery-baseline.md) §6）。

### ABI-10 IPC 通道重现被禁止的 task 指针模式

- 现状：`check-task-state-boundary` 报 `task_t *waiter` 出现在 `kernel/ipc/a20_channel.c:491`，而该模式被门禁明确禁止（11 条断言中 1 条失败）。
- 缺口：task 状态的持有方式偏离既定边界，门禁为红。
- 关闭条件：断言在候选上通过，或按当前契约改写断言并有依据。
- 来源：同上。
### ABI-11 arm32 用户态在 fork 出的命令返回后崩溃

- 现状：`ARCH=arm32 ABI=both BRINGUP=0` 的镜像能启动到 shell，内建命令（`echo`）正常；执行任何 fork 出去的命令（`/bin/echo`、`poweroff`）时子进程正常输出，随后父进程 mksh 在 `execute()` 里以空指针崩溃：`SIGSEGV ... sepc=0x23a3c stval=0x17`，指令是 `ldrb r3,[r9,#22]`，r9=0。此行为在本轮合并前后同样复现（同一镜像、同一输入），不是合并引入。BRINGUP=1 的内核镜像不受影响，能 `part ok` 并干净关机。
- 缺口：arm32 上子进程退出后的等待/信号返回路径会让 mksh 走到一个空指针；用户态镜像因此无法完成任何真正的命令，也无法干净关机。
- 关闭条件：arm32 用户态镜像里 `/bin/echo` 连续执行两次后 mksh 仍存活，且 `poweroff` 能触发 `System is going down for power-off NOW`。
- 来源：CORE 包在合并后重跑 `smoke-arch-mmu-matrix` 的 arm32 用例时暴露；崩溃现场见 QEMU 日志（内核 `SIGSEGV: pid=6 ... comm=mksh`）。

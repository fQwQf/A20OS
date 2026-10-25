# ABI / IPC / SIGNAL 缺口

对应 V1-ABI。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)、
[current/native-abi-runtime-status.md](../current/native-abi-runtime-status.md)。

本包关闭了 ABI-01..ABI-07、ABI-09、ABI-10，验证入口：

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

以下两条不在本包范围，保留原状（条目编号不重排）：

### ABI-08 终端没有 controlling terminal 与会话归属

- 现状：会话与进程组有了（`task_t.sid`/`pgid`，`setsid`/`setpgid`），tty 行规也有了（ISIG 在接收路径上判定并向后台上报，见 [drivers/guide/lock-order.md](../../drivers/guide/lock-order.md)）。但终端本身不记录"属于哪个会话、前台是哪个进程组"：控制台的前台进程组是 `kernel/drivers/char/uart.c` 里的一个全局值；`TIOCSCTTY` 在 pty 上直接返回 0、在控制台上是 `-ENOTTY`；`vhangup` 因"没有 controlling terminal"而拒绝（[abi/linux/syscall_coverage.md](../../../kernel/abi/linux/syscall_coverage.md)）。
- 缺口：会话首进程退出时不发 SIGHUP，也不对会话内进程补 SIGCONT；后台进程组读写终端不受 SIGTTIN/SIGTTOU 约束；pty 的 `pgrp` 只能由 `TIOCSPGRP` 直接写入，没有"取得/放弃控制终端"这一步；控制台与 pty 各自实现一套，而不是同一条契约。
- 阻塞点（2026-10 实测）：`ping` 运行期间按 ^C 完全没有效果——终端 termios 是对的（子进程里 `stty -a` 显示 `isig icanon …，intr = ^C`），`tcsetpgrp` 也生效（`TIOCSPGRP` 把控制台前台组设成该作业的 pgid），但**没有任务在读控制台时输入不被处理**：ISIG 判定在接收路径上，而接收路径只在有读者轮询 16550 时被走到（`arch_uart_init` 已写 IER=0x01、PLIC 也 enable 了 IRQ 10，所以要么 RX 中断实际没进来，要么处理函数没被调用，尚未定位）。后果是前台作业不读终端时 ^C 只能等它结束后被 shell 读到，这时前台组已经回落到 shell 自己。修这一项要先让控制台输入变成中断驱动（或由 tick 代 poll），ISIG 才可能在「没人读」的时候生效；驱动侧的这一项单独记在 [drivers.md](drivers.md) DRIVER-08。2026-10-24 定位进展：RX 中断从未触发已确认——同一 smoke 日志里 virtio 源经 claim/分发成功、而 UART 顶半部探针从未运行（见 DRIVER-08），阻塞面收窄到 16550→PLIC 源 10 的投递段。
- 关闭条件：`TIOCSCTTY(0)`/`TIOCNOTTY` 按 Linux 语义生效（只有会话首进程能取得，重复取得 `EPERM`，steal 位可夺取）；会话首进程退出时向该终端的前台进程组发 SIGHUP、向会话内其余进程发 SIGCONT，并解除归属；后台进程组读终端收到 SIGTTIN（写且置 `TOSTOP` 时收到 SIGTTOU）；`vhangup` 有真正可撤销的终端；上述行为各有 guest 门禁覆盖。
- 来源：`kernel/fs/devfs/tty.c`、`kernel/drivers/char/{uart,pty}.c`、[abi/linux/syscall_coverage.md](../../../kernel/abi/linux/syscall_coverage.md)。本包未处理（属于终端/驱动工作流）。

### ABI-11 ARM32 fork/exec 运行时（QEMU 关闭条件已满足）

- 当前实现：C/Rust 内存接口统一采用原生地址和计数宽度，避免 ILP32 参数错位释放仍被映射的页框。异常处理进入任务自己的内核栈后允许调度；trap、切换和信号帧保存对应 VFP 状态。
- 验证范围：QEMU virt、64 MiB、`ABI=both`、RAMFS，连续 10 轮，每轮 40 次顺序 fork/exec，并验证父子 VFP 隔离、抢占、空闲唤醒、文件读写和正常关机，共 400 次 fork/exec 通过，无 SIGSEGV、内核异常或内存耗尽。
- 回归入口：以 `ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 RAMFS_USER=1 RAMFS_FPU_TESTS=1 DRIVER_DEPLOYMENT=embedded` 构建，执行 `python3 tools/check_serial_shell.py --arch arm32 --kernel <kernel.elf> --memory-mb 64 --fpu --timers --stress-forks 40`。
- 边界：此结论只覆盖上述配置；V3s 板级 UART、DDR、SD 和真实中断时序尚待实机验收，异步信号与长期负载仍需独立覆盖。

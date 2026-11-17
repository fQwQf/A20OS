# 控制终端与作业控制

控制台与 64 个 PTY 共用 `kernel/fs/tty_control.c` 的会话、前台进程组与撤销协议。`task_t` 的控制终端关联按线程组共享，fork 继承，成功的 setsid 清除；`vfile_t` 保存打开时的 I/O 代数。关联代数使整个旧会话脱离终端，I/O 代数使已经打开的旧描述符失效，PTY 编号重用不重置这些代数。

## 行为

| 操作 | 当前契约 |
| --- | --- |
| 打开终端 | 会话首进程以可读方式打开未归属终端时自动取得；`O_NOCTTY` 与仅写打开不自动取得；`/dev/tty` 解析调用者实际控制终端，无终端为 `ENXIO` |
| `TIOCSCTTY` | 会话首进程取得；重复取得同一终端成功，已有其他控制终端为 `EPERM`；夺取其他会话的终端要求参数 1 与初始 user namespace 的 `CAP_SYS_ADMIN` |
| 前台组查询/设置 | slave 要求是调用者控制终端；PTY master 可查询。设置要求真实的同会话进程组，不存在为 `ESRCH`，跨会话为 `EPERM` |
| 后台读 | 向进程组送 `SIGTTIN`；屏蔽/忽略该信号或孤儿进程组返回 `EIO` |
| 后台写和变更 ioctl | `TOSTOP` 写及终端变更请求送 `SIGTTOU`；屏蔽/忽略时允许操作，未屏蔽的孤儿组返回 `EIO`。变更 ioctl 不依赖 `TOSTOP` |
| `TIOCNOTTY` | 非会话首进程只脱离当前进程；首进程脱离整个会话，并向前台组发送 `SIGHUP` 和 `SIGCONT` |
| 会话首进程退出 | 最后一个线程退出时解除会话归属，向终端前台组发送 `SIGHUP` 和 `SIGCONT` |
| PTY master 关闭 / `vhangup` | 挂断终端并撤销旧 slave 描述符；通知会话首进程 `SIGHUP` / `SIGCONT`。`vhangup` 要求初始 user namespace 的 `CAP_SYS_TTY_CONFIG` |
| 旧描述符 | 挂断后的 read 为 EOF，write/ioctl 为 `EIO`，poll 报挂断/错误；阻塞读被唤醒。重新打开取得新 I/O 代数，不能恢复旧描述符权限 |
| 普通文件 | 控制终端 ioctl 为 `ENOTTY`，不回退到控制台，也不伪造 PTY 编号 |

主动脱离与挂断的通知对象不同，按 [Linux tty_jobctrl.c](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/tty_jobctrl.c) 与 [tty_io.c](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/tty_io.c) 区分。测试使用自己创建的会话和 PTY，不夺取运行测试工具的宿主终端。

公共层不在控制锁内遍历任务、发送信号或调用驱动。PTY pair 锁可嵌套控制锁，信号在 pair 锁释放后发送；任务列表固定引用后才发送内核信号，不重新解析裸 PID，也不依赖当前中断任务的 uid。

## 验证

`tty_jobcontrol_test` 检查编号/数据流/锁、普通文件拒绝、所有权、前后台读写、后台 ioctl、信号屏蔽/忽略与主动脱离。`tty_lifecycle_test` 检查 HUP/CONT、会话退出、master 关闭、线程共享、孤儿组访问、自动取得与 `/dev/tty`；`--revocation` 检查特权挂断、阻塞读撤销、拒绝越权、特权夺取与旧描述符失效。

Linux 宿主运行同一份普通权限测试，作为独立行为参考；特权挂断/夺取在 A20OS guest 验证。公开重复冷启动入口：

```sh
python3 tools/probes/tty_jobcontrol_probe.py \
  --arch riscv64 --kernel <kernel.elf> --fat <fat32.img> \
  --user-bin <matching-user-build-directory> --cpus 2 \
  --cycles 3 --boots 2 --output <evidence-directory>
```

AArch64 使用 `--arch aarch64 --kernel <kernel.bin> --cpus 4 --require-audit`。门禁复制输入 FAT，按需注入对应架构的两个程序；逐段核对结果，要求 QEMU 自然退出，运行 `fsck.fat -n` 并核对输入哈希。`--require-audit` 还要求关机审计 0。

2026-11-16 的 RV64 SMP2 与 AArch64 SMP4 均完成两次冷启动、每次三轮所有分项；四次自然关机与 FAT 检查通过，AArch64 两次审计均为 0。RV64 UART 无读者 Ctrl-C 回归约 11 ms，IRQ 字节增加 1、poll 字节保持 0。详细输入哈希、失败尝试和后续桌面回归见[收口记录](../../archive/v1-closeout-2026-11-16.md)。这些结果不等于所有平台的发布认证；Native 普通通知另遵守[检查点式设计边界](../../native-abi/checkpoint-signals.md)。

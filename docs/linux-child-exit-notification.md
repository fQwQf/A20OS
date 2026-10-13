# 线程组退出与 SIGCHLD

父进程必须在线程组结束时收到退出通知。仅发布主线程的 ZOMBIE 不够：
仍有活跃成员时，wait4(WNOHANG) 不会回收该组。若此时提前发送 SIGCHLD，
随后最后一个成员只唤醒 wait4 等待者、没有发送信号，使用 sigsuspend 的
shell 就可能永远等待已经结束的命令。

XFCE 实测中，zsh 等待已经成为 zombie 的 xfconf-query，信号 pending 为零，
主线程和工作线程均已退出。该问题解释配置命令偶发没有返回提示符，
也会影响依靠 SIGCHLD 回收子进程的桌面程序。

`proc_exit` 现在在线程组全部结束后、tasklist_lock 下认领一次通知；
主线程先退出时，由最后一个成员发送。引用保留父进程与组长的身份直到
完成清理和信号排队。阻塞 wait4 的唤醒仍保留。通知携带组长身份、退出状态
和 CLD_EXITED/CLD_KILLED/CLD_DUMPED，PID 与 UID 按父进程命名空间转换。
没有架构或应用名称判断。

## 验证

`sigchld_group_test` fork 后创建一个阻塞的工作线程，让组长先执行单线程
exit。父进程验证此时没有 SIGCHLD 且 WNOHANG 返回零；释放工作线程后，
验证收到的 SIGCHLD 包含正确 PID、UID、CLD_EXITED 和状态，并能 waitpid 回收。

```sh
make smoke-sigchld-group
```

本轮证据位于 `build/browser-followup/`：

- 宿主 Linux 的同一测试通过。
- `sigchld-negative2-guest.log`：旧内核失败于
  `leader exit must not notify before group completion`。
- `sigchld-group-x86-guest.log`：四核抢占 x86_64 上新测试、signalfd、SCM、
  VM_FAULT 全部通过。
- `sigchld-group-rv-guest.log`：四核抢占 RISC-V 上新测试、signalfd、SCM 通过。
- `firefox-final-1.log`：XFCE 配置写入、读回和删除通过；网页完整验收另见
  [XFCE 验证记录](distro/xfce-x86_64-validation.md)。

RISC-V 首次并行构建碰到共享 drvmod 文件被另一递归构建截断的既有竞态，
重跑构建后通过；上述 guest 结果来自重跑成功的构建。

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

`sigchld_group_test` 覆盖组长先退出、工作线程仍阻塞的情况：父进程验证此时
没有 SIGCHLD 且 WNOHANG 返回零；释放工作线程后，验证收到的 SIGCHLD 包含
正确 PID、UID、CLD_EXITED 和状态，并能 waitpid 回收。

`child_exit_matrix` 覆盖完整组合矩阵，每个用例都断言"恰好回收一次"：一次
成功的 waitpid、随后 ECHILD、且没有第二条 pending SIGCHLD。

| 用例 | 断言 |
| --- | --- |
| worker-first | 工作线程先退出、组长最后退出；通知携带组长 PID 和状态 |
| exit-group | `exit_group()` 终止仍存活的工作线程；只产生一条组长状态的通知 |
| concurrent-waiters | 两个线程同时 waitpid；一个回收，另一个得到 ECHILD |
| vfork | vfork 子进程退出后 SIGCHLD 的 PID/状态正确 |
| vfork + SIG_IGN | 子进程自动回收，不投递 SIGCHLD，waitpid 返回 ECHILD |
| SA_NOCLDWAIT | 子进程自动回收，不投递 SIGCHLD |
| CLONE_PARENT | 创建者不是子进程的父进程（waitpid 得 ECHILD）；祖父母回收一次并收到正确 ppid |
| PDEATHSIG | 父进程死亡后处理器恰好运行一次 |
| CLONE_PARENT + PDEATHSIG | 创建者先死亡不误投递；实际父进程（祖父母）死亡时恰好投递一次 |

```sh
make smoke-sigchld-group
make smoke-child-exit-matrix      # riscv64, ABI=linux, NR_CPUS=2 / -smp 2
```

`smoke-child-exit-matrix` 要求 guest 日志同时出现
`[SMP] 2/2 configured CPUs online` 与 `CHILD_EXIT_MATRIX: PASS`，并把
`CHILD_EXIT: FAIL` 作为禁止模式。

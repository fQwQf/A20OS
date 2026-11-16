# 目标应用的 Linux ABI 契约与证据

对应 [N08](../../status/gaps/triage-matrix.md)。按当前已开展的应用工作负载登记调用、关键错误和验证入口，不要求所有 syscall 标成 full，也不把应用所带依赖包的每个可选 API 都变成独立验收项。包清单、入口存在和历史运行记录均不能单独证明当前候选通过。

## 应用与关键行为

| 应用 / 来源 | 必需调用与关键边界 | 入口和当前证据状态 |
| --- | --- | --- |
| BusyBox shell、CLI 的 zsh/bash；[CLI world](../../../../packages/world/cli.world) | clone/execve/wait4、pipe2/dup/close、rt_sigaction/rt_sigprocmask、setsid/setpgid、终端 ioctl、get/setrlimit/prlimit64。管道 EOF/EPIPE、子进程收割、CLOEXEC、后台读写拒绝及非法限制组合必须有明确结果。 | `make smoke-child-exit-matrix`、`make smoke-sigchld-group`；[TTY 生产程序](../../../../user/cmds/core/tty_jobcontrol_test.c)及[运行契约](tty-jobcontrol.md)。RV64 SMP2/AArch64 SMP4 的历史记录见[本轮归档](../../archive/v1-closeout-2026-11-16.md#控制终端与作业控制)；本轮 prlimit 主机检查见下表，不能替代完整 shell 客体运行。 |
| OpenRC PID 1 / supervise-daemon；[XFCE world](../../../../packages/world/xfce.world) | mknodat/openat/read/write/poll、kill/wait4、procfs、mount/reboot。FIFO 无对端的非阻塞写打开为 ENXIO，断开写为 EPIPE；PID 1 保留、监督重启、正常关机和停止错误须如实交付。 | [init 接口契约](init-compatibility.md)，[FIFO 客体检查](../../../../tools/tests/fifo_guest.py)、[环境读取检查](../../../../tools/tests/proc_environ_guest.py)、[OpenRC 监督检查](../../../../tools/tests/openrc_supervision_guest.py)。AArch64 实际 world 的历史监督恢复见[桌面状态](../../status/current/desktop-minecraft-status.md#aarch64-原生服务恢复2026-11-16)；不把进程存活当作服务恢复通过。 |
| Python 3 / Node.js/npm；[pynode world](../../../../packages/world/pynode.world) | execve、mmap/mprotect、futex、poll/epoll、clock_gettime/timerfd、socket 与基础文件 I/O。事件循环的超时/中断、关闭描述符、错误路径和安装期子进程退出须有可观察结果。 | `make smoke-poll-edge`、`make smoke-timer-edge`、`make smoke-futex-stress` 提供共享机制入口；[脚本执行与共享内存探针](../../../../tools/tests/desktop_abi_probe.py)提供客体/宿主对照。world 声明并不证明 pip/npm 安装期脚本已通过；对应应用级结果尚未在本表登记。 |
| labwc / XFCE / Firefox；[XFCE world](../../../../packages/world/xfce.world) | clone(CLONE_SIGHAND)、execve、Linux 信号与 syscall restart、seccomp、poll/epoll、Unix socket 的 SCM_RIGHTS、mmap 与设备 ioctl。线程共享处理表、SIGSYS 处理、正确的架构重启长度、共享对象关闭与沙箱拒绝是关键边界。 | [syscall 重启程序](../../../../user/cmds/core/syscall_restart_test.c)、`make smoke-scm-stress`、[桌面 ABI 探针](../../../../tools/tests/desktop_abi_probe.py)、[浏览器客体入口](../../../../tools/tests/xfce-web-smoke.sh)。[x86_64 历史修复记录](../../status/history/desktop-regressions-2026-10-22.md)记录了真实浏览器交互；脚本本身注明视觉交互需另行确认，不能仅凭启动标记关闭应用路径。 |
| OpenJDK 21 / Minecraft；[客户端范围](../../distro/minecraft.md) | clone/TLS/futex、Linux 信号、文件映射、动态库加载、socket/ioctl(SIOCGIFCONF)、设备 ioctl 与共享描述符。接口尺寸查询、短缓冲区不越界、线程与共享映射寿命、退出及存档重载需保持一致。 | [网卡枚举契约](interface-enumeration.md)、`make smoke-net-iface`；[客户端客体入口](../../../../tools/tests/xfce-minecraft-smoke.sh)与[实际保存/重载步骤](../../distro/minecraft.md#验证方法)。[历史桌面记录](../../status/history/desktop-regressions-2026-10-22.md)不升级为当前候选或所有架构通过；账号登录、Realms 和联机仍按现有应用范围披露。 |
| CLI/桌面的 curl 与网络查询工具；[CLI world](../../../../packages/world/cli.world) | socket/connect/send/recv、setsockopt/getsockopt、接口 ioctl/netlink 与地址族校验。TCP 连通、接口枚举和 UDP DNS 是不同契约，不能互相替代；不支持的选项/地址族必须给出真实错误。 | [上游用户态接口探针](../../../../user/tests/test_upstream_userland.c)、`make smoke-net-iface`及[HTTP/HTTPS 入口](../../../../tools/tests/xfce-web-smoke.sh)。CLI world 中保留的旧网络失败描述须与当前实现核对，不能直接当作现行故障，也不能用一次 HTTP 200 宣称网络工具全部可用。 |

## 本轮直接验证的共享调用

| 消费路径 | 已验证行为 | 当前直接证据 |
| --- | --- | --- |
| musl getentropy/getrandom 调用者 | 合法零长度 NULL 返回 0；非法 flag 仍拒绝；首次复制失败为 EFAULT，后续分块失败返回此前完整复制的前缀。 | `python3 -m unittest tools.tests.test_getrandom_abi`，1 项通过；实际 Linux 对照与生产 A20 适配函数，见[记录](../../archive/v1-closeout-2026-11-16.md#linux-getrandom-的零长度与部分完成返回)，实现提交 `18c25c123`。RNG/usercopy 为替代边界，不证明整个应用或熵质量。 |
| shell ulimit / prlimit 调用者 | soft > hard 拒绝且不修改状态；新旧值共用缓冲区正确；跨 uid 查询拒绝；显式 self pid 的引用归还；无效 pid 不因空请求而成功。 | `python3 tools/tests/test_rlimit_policy_host.py` 11 项、`python3 -m unittest tools.tests.test_prlimit_abi` 1 项通过，见[记录](../../archive/v1-closeout-2026-11-16.md#linux-资源限制的错误别名和任务引用)，实现提交 `23add78a3`。现有软硬限制单值映射及简化凭据匹配的 partial 边界仍保留在覆盖表。 |

## 证据与关闭规则

以上路径来自现有 world、接口说明和应用记录，不新增全 syscall 或全架构穷尽矩阵。N08 尚未关闭：清单建立完成不等于表中关键契约全部完成。

- 保留 [syscall 覆盖表](../../../../kernel/abi/linux/syscall_coverage.md) 的 partial/拒绝边界；每个已确认的应用阻塞对应具体调用和可复现行为，再安排修复。
- 复用仍适用的直接检查，不逐项重复历史全量。应用级运行与正式支持平台的关键验证合并到候选验收，记录内核/镜像、ABI、CPU/设备、命令、结果和跳过范围；平台矩阵仍归 N18/N20。
- 2026-11-16 本地核对发现 `tty-control-desktop-regression`、`aarch64-desktop-supervision-rust`、`aarch64-desktop-supervision-c-public` 的原始报告目录已缺失。对应 Markdown 历史记录保留，但不能当作当前工作区可复核的候选报告。
- Native 检查点通知另遵守[设计边界](../../native-abi/checkpoint-signals.md)，不在本条重新要求完整 Native 异步处理器。

本页没有把依赖包安装、主机替代边界、历史通过或脚本存在升级为应用已通过。后续登记新证据时更新对应行，达到原要求后从缺口台账删除 N08。

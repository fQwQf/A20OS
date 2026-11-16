# 目标应用的 Linux ABI 契约与证据

对应 [N08](../../status/gaps/triage-matrix.md)。按当前已开展的应用工作负载登记调用、关键错误和验证入口，不要求所有 syscall 标成 full，也不把应用所带依赖包的每个可选 API 都变成独立验收项。包清单、入口存在和历史运行记录均不能单独证明当前候选通过。

## 应用与关键行为

| 应用 / 来源 | 必需调用与关键边界 | 入口和当前证据状态 |
| --- | --- | --- |
| BusyBox shell、CLI 的 zsh/bash；[CLI world](../../../../packages/world/cli.world) | clone/execve/wait4、pipe2/dup/close、rt_sigaction/rt_sigprocmask、setsid/setpgid、终端 ioctl、get/setrlimit/prlimit64。管道 EOF/EPIPE、子进程收割、CLOEXEC、后台读写拒绝及非法限制组合必须有明确结果。 | `make smoke-child-exit-matrix`、`make smoke-sigchld-group`；[TTY 生产程序](../../../../user/cmds/core/tty_jobcontrol_test.c)及[运行契约](tty-jobcontrol.md)。RV64 SMP2/AArch64 SMP4 的历史记录见[本轮归档](../../archive/v1-closeout-2026-11-16.md#控制终端与作业控制)；本轮 prlimit 主机检查见下表，不能替代完整 shell 客体运行。CLI 的 bash/zsh 管道、命令替换、后台子进程等待和退出码已在 RV64 SMP2 通过；NPROC 默认无限制与无特权零上限拒绝也通过，见下方运行记录。 |
| OpenRC PID 1 / supervise-daemon；[XFCE world](../../../../packages/world/xfce.world) | mknodat/openat/read/write/poll、kill/wait4、procfs、mount/reboot。FIFO 无对端的非阻塞写打开为 ENXIO，断开写为 EPIPE；PID 1 保留、监督重启、正常关机和停止错误须如实交付。 | [init 接口契约](init-compatibility.md)，[FIFO 客体检查](../../../../tools/tests/fifo_guest.py)、[环境读取检查](../../../../tools/tests/proc_environ_guest.py)、[OpenRC 监督检查](../../../../tools/tests/openrc_supervision_guest.py)。AArch64 实际 world 的历史监督恢复见[桌面状态](../../status/current/desktop-minecraft-status.md#aarch64-原生服务恢复2026-11-16)；不把进程存活当作服务恢复通过。 |
| Python 3 / Node.js/npm；[pynode world](../../../../packages/world/pynode.world) | execve、mmap/mprotect、futex、poll/epoll、clock_gettime/timerfd、socket 与基础文件 I/O。事件循环的超时/中断、关闭描述符、错误路径和安装期子进程退出须有可观察结果。 | `make smoke-poll-edge`、`make smoke-timer-edge`、`make smoke-futex-stress` 提供共享机制入口；[脚本执行与共享内存探针](../../../../tools/tests/desktop_abi_probe.py)提供客体/宿主对照。新增[离线安装期应用检查](../../../../tools/tests/application_runtime_guest.py)。Python 离线构建/安装/导入/清理、Node/npm 子进程/定时器/I/O 及各自正常写回关机均有 RV64 SMP2 通过记录，见下方运行记录。 |
| labwc / XFCE / Firefox；[XFCE world](../../../../packages/world/xfce.world) | clone(CLONE_SIGHAND)、execve、Linux 信号与 syscall restart、seccomp、poll/epoll、Unix socket 的 SCM_RIGHTS、mmap 与设备 ioctl。线程共享处理表、SIGSYS 处理、正确的架构重启长度、共享对象关闭与沙箱拒绝是关键边界。 | [syscall 重启程序](../../../../user/cmds/core/syscall_restart_test.c)、`make smoke-scm-stress`、[桌面 ABI 探针](../../../../tools/tests/desktop_abi_probe.py)、[浏览器客体入口](../../../../tools/tests/xfce-web-smoke.sh)。[x86_64 历史修复记录](../../status/history/desktop-regressions-2026-10-22.md)记录了真实浏览器交互；脚本本身注明视觉交互需另行确认，不能仅凭启动标记关闭应用路径。 |
| OpenJDK 21 / Minecraft；[客户端范围](../../distro/minecraft.md) | clone/TLS/futex、Linux 信号、文件映射、动态库加载、socket/ioctl(SIOCGIFCONF)、设备 ioctl 与共享描述符。接口尺寸查询、短缓冲区不越界、线程与共享映射寿命、退出及存档重载需保持一致。 | [网卡枚举契约](interface-enumeration.md)、`make smoke-net-iface`；[客户端客体入口](../../../../tools/tests/xfce-minecraft-smoke.sh)与[实际保存/重载步骤](../../distro/minecraft.md#验证方法)。[历史桌面记录](../../status/history/desktop-regressions-2026-10-22.md)不升级为当前候选或所有架构通过；账号登录、Realms 和联机仍按现有应用范围披露。 |
| CLI/桌面的 curl 与网络查询工具；[CLI world](../../../../packages/world/cli.world) | socket/connect/send/recv、setsockopt/getsockopt、接口 ioctl/netlink 与地址族校验。TCP 连通、接口枚举和 UDP DNS 是不同契约，不能互相替代；不支持的选项/地址族必须给出真实错误。 | [上游用户态接口探针](../../../../user/tests/test_upstream_userland.c)、`make smoke-net-iface`及[HTTP/HTTPS 入口](../../../../tools/tests/xfce-web-smoke.sh)。本轮 CLI 的 ip 接口/地址/路由、本地 HTTP 及 dig/nslookup 的真实 UDP DNS 查询均通过；[UDP 错误队列](udp-error-queue.md)的 IPv4/IPv6 ICMP 拒绝、就绪与消费边界通过。测试范围为本地服务与回环错误，不扩大为公网/全部后端认证。 |

## 本轮直接验证的共享调用

| 消费路径 | 已验证行为 | 当前直接证据 |
| --- | --- | --- |
| musl getentropy/getrandom 调用者 | 合法零长度 NULL 返回 0；非法 flag 仍拒绝；首次复制失败为 EFAULT，后续分块失败返回此前完整复制的前缀。 | `python3 -m unittest tools.tests.test_getrandom_abi`，1 项通过；实际 Linux 对照与生产 A20 适配函数，见[记录](../../archive/v1-closeout-2026-11-16.md#linux-getrandom-的零长度与部分完成返回)，实现提交 `18c25c123`。RNG/usercopy 为替代边界，不证明整个应用或熵质量。 |
| shell ulimit / prlimit 调用者 | soft > hard 拒绝且不修改状态；新旧值共用缓冲区正确；跨 uid 查询拒绝；显式 self pid 的引用归还；无效 pid 不因空请求而成功。 | `python3 tools/tests/test_rlimit_policy_host.py` 11 项、`python3 -m unittest tools.tests.test_prlimit_abi` 1 项通过，见[记录](../../archive/v1-closeout-2026-11-16.md#linux-资源限制的错误别名和任务引用)，实现提交 `23add78a3`。现有软硬限制单值映射及简化凭据匹配的 partial 边界仍保留在覆盖表。 |

## 本轮真实客体运行（2026-11-16）

以下证据均为 RV64 QEMU virt、ABI=both、SMP2；使用私有磁盘副本；安装期检查使用离线临时包，CLI 网络检查仅访问宿主本地 HTTP/UDP DNS 夹具。各报告记录实际冻结内核/镜像及脚本哈希；这些有界运行不自动代表全部正式支持配置的最终候选通过。证据目录前缀为 `build/evidence/v1-closeout-20261116/`。

| 已完成行为 | 可复核证据与范围 |
| --- | --- |
| shell 管道/等待、脚本执行、IPC_RMID 后共享内存 | `application-runtime/report.json` 的对应分项 PASS；该早期运行的其它分项失败，整体不计为通过。 |
| madvise fork-policy 的范围、清零与继承恢复 | `madvise-fork-range/verified/report.json` 的 madvise 分项通过，另有宿主 Linux 对照；该轮尚未修复的 Node 输出失败不计为通过。 |
| 无名 Unix 地址、npm 安装期子进程输出、定时器与文件 I/O | `ordered-file-data/verified/report.json` 的 unix-names/node-install 均 PASS，正常写回、自然关机，ext4/FAT 检查 0；宿主 Linux 地址对照另存于 `unix-socket-names/linux-reference.json`。 |
| 目录跨父移动/替换/exchange、自身子树拒绝与同 inode no-op | `directory-rename/final/report.json` 为 PASS，自然关机、ext4/FAT 检查 0；有宿主 Linux 对照与旧内核错误计数/清理失败负控。 |
| Python 离线构建、pip target 安装、模块导入及完整临时目录清理 | `directory-rename/pip-verified/report.json` 为 PASS，正常写回、自然关机，ext4/FAT 检查 0。 |
| syscall trace 慢调用扫描与真实 Node 父/子进程 | `syscall-trace-race/verified/report.json` 为 PASS，实际 44 条慢扫描记录、预期子进程输出、无 UBSAN/内核故障，自然关机及 ext4/FAT 检查 0；另有确定性交错宿主检查。 |
| CLI 的 Netlink 缓冲区、ip 接口/地址/路由及本地 HTTP | `cli-application/netlink-fixed/report.json` 的 interfaces/http 分项 PASS；缓冲区设置/查询与超限发送拒绝均通过，HTTP 夹具实际收到请求。该次 CLI 整体 FAIL：当时的 bash 管道崩溃已由后续 NPROC 修复关闭；chroot mountinfo 已由后续挂载视图修复关闭；该轮没有 DNS 请求的问题已由后续错误队列修复关闭。QEMU 自然退出，ext4/FAT 只读检查均为 0。 |
| CLI bash/zsh 的管道、命令替换、后台等待与 NPROC 边界 | `cli-application/bash-nproc-verified/report.json` 为 PASS，两个 shell 均输出明确通过标记；默认 NPROC 为无限制，降权子进程设置零上限后 fork 返回 EAGAIN。自然关机及 ext4/FAT 只读检查均为 0；仅选择 shells 分项，没有重跑无关网络或挂载路径。 |
| CLI 挂载及 chroot / 目标进程挂载视图 | `cli-application/mount-view-verified/report.json` 为 PASS，耗时 11.366 秒；含空格的临时挂载点、文件写读/卸载、当前进程三个挂载文件，以及父进程读取具有私有 mount namespace 和独立 chroot 的子进程视图均通过。子进程卸载、退出与父命名空间隔离通过；正常关机及 ext4/FAT 只读检查均为 0。 |
| UDP 扩展错误与 dig/nslookup DNS | `cli-application/udp-error-queue-final/report.json` 为 PASS，耗时 11.754 秒；IPv4/IPv6 回环端口不可达、POLLERR、SO_ERROR 与错误队列独立消费、普通接收错误、短 payload/control、零 iov/零名称容量和禁用清队列通过；dig/nslookup 获得 DNS 答案，宿主夹具收到 2 个 A 查询和 1 个 AAAA 查询。正常关机及 ext4/FAT 只读检查均为 0；同一错误队列探针有宿主 Linux 对照。 |

此前复现的 Python 清理 EIO、Node fork/输出失败、npm 写回关机拒绝、CLI bash 管道崩溃、chroot mountinfo 路径及目标 PID 丢失、UDP DNS 阻塞和 trace 扫描器竞争均已修复，不再作为当前缺口。初始失败、根因、实现和验证边界保留在[本轮归档](../../archive/v1-closeout-2026-11-16.md#rv64-pynode-实际运行与构建阻塞)；具体修复见归档后续的 fork advice、Unix 地址、目录 rename、ordered data 与 trace 小节。CLI 启动时 console-size 仍无法打开 `/dev/tty`，尺寸显示 `?x?`，不把这些分项通过升级为完整交互工作站通过。N08 继续跟踪该终端问题、表中其余应用关键边界与候选证据，N21 保留其已声明的其它恢复要求。

## 证据与关闭规则

以上路径来自现有 world、接口说明和应用记录，不新增全 syscall 或全架构穷尽矩阵。N08 尚未关闭：清单建立完成不等于表中关键契约全部完成。

- 保留 [syscall 覆盖表](../../../../kernel/abi/linux/syscall_coverage.md) 的 partial/拒绝边界；每个已确认的应用阻塞对应具体调用和可复现行为，再安排修复。
- 复用仍适用的直接检查，不逐项重复历史全量。应用级运行与正式支持平台的关键验证合并到候选验收，记录内核/镜像、ABI、CPU/设备、命令、结果和跳过范围；平台矩阵仍归 N18/N20。
- 2026-11-16 本地核对发现 `tty-control-desktop-regression`、`aarch64-desktop-supervision-rust`、`aarch64-desktop-supervision-c-public` 的原始报告目录已缺失。对应 Markdown 历史记录保留，但不能当作当前工作区可复核的候选报告。
- Native 检查点通知另遵守[设计边界](../../native-abi/checkpoint-signals.md)，不在本条重新要求完整 Native 异步处理器。

本页没有把依赖包安装、主机替代边界、历史通过或脚本存在升级为应用已通过。后续登记新证据时更新对应行，达到原要求后从缺口台账删除 N08。

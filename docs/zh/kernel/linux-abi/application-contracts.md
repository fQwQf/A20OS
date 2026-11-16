# 目标应用的 Linux ABI 契约与证据

原 N08 已按本页的[应用关键边界收口](#应用关键边界收口2026-11-17)关闭。正式支持平台与发布候选复验仍见 [N18/N20](../../status/gaps/triage-matrix.md)。按当前已开展的应用工作负载登记调用、关键错误和验证入口，不要求所有 syscall 标成 full，也不把应用所带依赖包的每个可选 API 都变成独立验收项。包清单、入口存在和历史运行记录均不能单独证明当前候选通过。

## 应用与关键行为

| 应用 / 来源 | 必需调用与关键边界 | 入口和当前证据状态 |
| --- | --- | --- |
| BusyBox shell、CLI 的 zsh/bash；[CLI world](../../../../packages/world/cli.world) | clone/execve/wait4、pipe2/dup/close、rt_sigaction/rt_sigprocmask、setsid/setpgid、终端 ioctl、get/setrlimit/prlimit64。管道 EOF/EPIPE、子进程收割、CLOEXEC、后台读写拒绝及非法限制组合必须有明确结果。 | `make smoke-child-exit-matrix`、`make smoke-sigchld-group`；[TTY 生产程序](../../../../user/cmds/core/tty_jobcontrol_test.c)及[运行契约](tty-jobcontrol.md)。RV64 SMP2/AArch64 SMP4 的历史记录见[本轮归档](../../archive/v1-closeout-2026-11-16.md#控制终端与作业控制)；本轮 prlimit 主机检查见下表，不能替代完整 shell 客体运行。CLI 的[控制台会话](cli-console.md)、bash/zsh 管道、命令替换、后台子进程等待和退出码已在 RV64 SMP2 通过；NPROC 默认无限制与无特权零上限拒绝也通过，见下方运行记录。 |
| OpenRC PID 1 / supervise-daemon；[XFCE world](../../../../packages/world/xfce.world) | mknodat/openat/read/write/poll、kill/wait4、procfs、mount/reboot。FIFO 无对端的非阻塞写打开为 ENXIO，断开写为 EPIPE；PID 1 保留、监督重启、正常关机和停止错误须如实交付。 | [init 接口契约](init-compatibility.md)，[FIFO 客体检查](../../../../tools/tests/fifo_guest.py)、[环境读取检查](../../../../tools/tests/proc_environ_guest.py)、[OpenRC 监督检查](../../../../tools/tests/openrc_supervision_guest.py)。AArch64 实际 world 的历史监督恢复见[桌面状态](../../status/current/desktop-minecraft-status.md#aarch64-原生服务恢复2026-11-16)；本轮原版 OpenRC 0.63-r1 的 RV64 PID 1/default runlevel、环境读取、两次子进程退出/重启、控制 FIFO 信号、状态/停止与普通关机通过，见下方直接证据。不把精简镜像通过计为桌面/设备恢复通过。 |
| Python 3 / Node.js/npm；[pynode world](../../../../packages/world/pynode.world) | execve、mmap/mprotect、futex、poll/epoll、clock_gettime/timerfd、socket 与基础文件 I/O。事件循环的超时/中断、关闭描述符、错误路径和安装期子进程退出须有可观察结果。 | `make smoke-poll-edge`、`make smoke-timer-edge`、`make smoke-futex-stress` 提供共享机制入口；[脚本执行与共享内存探针](../../../../tools/tests/desktop_abi_probe.py)提供客体/宿主对照。新增[离线安装期应用检查](../../../../tools/tests/application_runtime_guest.py)。Python 离线构建/安装/导入/清理、Node/npm 子进程/定时器/I/O 及各自正常写回关机均有 RV64 SMP2 通过记录，见下方运行记录。 |
| labwc / XFCE / Firefox；[XFCE world](../../../../packages/world/xfce.world) | clone(CLONE_SIGHAND)、execve、Linux 信号与 syscall restart、seccomp、poll/epoll、Unix socket 的 SCM_RIGHTS、mmap 与设备 ioctl。线程共享处理表、SIGSYS 处理、正确的架构重启长度、共享对象关闭与沙箱拒绝是关键边界。 | [syscall 重启程序](../../../../user/cmds/core/syscall_restart_test.c)、`make smoke-scm-stress`、[桌面 ABI 探针](../../../../tools/tests/desktop_abi_probe.py)、[浏览器客体入口](../../../../tools/tests/xfce-web-smoke.sh)。[x86_64 历史修复记录](../../status/history/desktop-regressions-2026-10-22.md)记录了真实浏览器交互；脚本本身注明视觉交互需另行确认，不能仅凭启动标记关闭应用路径。本轮 x86_64 原版 Wayland 路径的真实鼠标/英文/Rime 中文输入、HTTP/TLS、标签页/刷新和正常退出已通过，见下方记录。 |
| OpenJDK 21 / Minecraft；[客户端范围](../../distro/minecraft.md) | clone/TLS/futex、Linux 信号、文件映射、动态库加载、socket/ioctl(SIOCGIFCONF)、设备 ioctl 与共享描述符。接口尺寸查询、短缓冲区不越界、线程与共享映射寿命、退出及存档重载需保持一致。 | [网卡枚举契约](interface-enumeration.md)、`make smoke-net-iface`；[客户端客体入口](../../../../tools/tests/xfce-minecraft-smoke.sh)与[实际保存/重载步骤](../../distro/minecraft.md#验证方法)。[历史桌面记录](../../status/history/desktop-regressions-2026-10-22.md)不升级为当前候选或所有架构通过；账号登录、Realms 和联机仍按现有应用范围披露。本轮 x86_64 OpenJDK 的映射 I/O、线程/TLS、Java 子进程、网卡枚举、回环 TCP、LWJGL/GLFW 加载与正常退出钩子已直接通过；Minecraft 的移动/跳跃、背包、鼠标转向、保存/退出及同盘冷启动重载也已通过，见下方本轮记录。 |
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
| CLI 登录会话、尺寸、回收、respawn 与普通关机 | `cli-console/verified/report.json` 为 PASS，耗时 27.946 秒；同一启动中的两个 getty 会话 SID 11/91 均验证控制终端/前台组、120×40 尺寸、termios 恢复、当前 zsh 的 resize-console 变量更新和孤儿回收。exit 后新会话恢复；普通 poweroff 由 BusyBox init 完成，自然关机及 ext4/FAT 只读检查均为 0。 |
| 原版 OpenRC PID 1、监督/控制 FIFO 与普通关机 | `openrc-runtime/control-verified/report.json` 为 PASS，耗时 11.335 秒；PID 1 的 `/proc/1/exe` 为 `/sbin/init`，文件哈希与 OpenRC 相同；default runlevel、proc environ、监督进程 161 的子进程 162→168→170、真实控制 FIFO 信号与服务停止通过；PID 1 接收普通关机 FIFO 命令并完成 TERM/KILL/sync，自然退出 0、ext4/FAT 只读检查均为 0。范围为精简 OpenRC world，不代表图形桌面、可选硬件服务或实机复位通过。 |

此前复现的 Python 清理 EIO、Node fork/输出失败、npm 写回关机拒绝、CLI bash 管道崩溃、chroot mountinfo 路径及目标 PID 丢失、UDP DNS 阻塞和 trace 扫描器竞争均已修复，不再作为当前缺口。初始失败、根因、实现和验证边界保留在[本轮归档](../../archive/v1-closeout-2026-11-16.md#rv64-pynode-实际运行与构建阻塞)；具体修复见归档后续的 fork advice、Unix 地址、目录 rename、ordered data 与 trace 小节。CLI 启动控制终端与尺寸问题已关闭，契约和范围见[CLI 控制台](cli-console.md)。OpenRC 的上述有界服务生命周期不再列为未验证项；上述应用功能与下节关键错误/权限证据共同关闭 N08；N21 保留其已声明的其它恢复要求。

## x86_64 桌面与 JVM 的直接运行（2026-11-16）

证据目录为 `build/evidence/v1-closeout-20261116/desktop-current/`。本轮构建的
x86_64 ABI=both SMP4 内核，运行真实 Alpine XFCE/OpenRC world、q35/KVM、4 GiB、
virtio-gpu/virgl、虚拟键鼠与 HDA。输入哈希、参数与客体源码记录在各次 `inputs.json`、
`argv.json`；不是正式支持矩阵的最终候选认证。

- `third/report.json` 的 **JVM 分项通过**：OpenJDK 21.0.13.0.3，实际
  [Java 应用探针](../../../../tools/tests/A20JvmRuntime.java)完成映射文件写回、四线程
  TLS/等待、Java 子进程输出和退出码、网卡枚举、回环 TCP、真实 LWJGL/GLFW 装载，
  Java 正常退出后检查 shutdown hook 的文件内容。该分项不再列为未验证要求。
- 修复桌面 overlay 的 `libdl.so.2`：旧链接指向无效的 Windows reparse 错误文本，
  现恢复既有设计的 `libc.so.6` 别名，与镜像的 gcompat/musl 路径配合；没有增加任意
  glibc native 兼容承诺。上述 JVM native 装载及 Minecraft 真实欢迎画面有直接证据。
- `third/report.json` **整轮仍为 FAIL**：Firefox HTTP/HTTPS DOM、标签页和刷新已运行，但硬件输入阶段
  Marionette 连接关闭；缺少该次视觉输入通过证据。桌面 D-Bus 经 OpenRC 正常 S5，
  QEMU 自然退出 0、ext4/FAT 只读检查 0，不能用这些成功项覆盖浏览器失败。
- Minecraft 完整的本轮应用行为见 `minecraft-contract.json`，对应
  `minecraft-second` 与 `minecraft-reload` 两次真实启动：移动/跳跃、背包、相对鼠标
  转向、Save and Quit / Quit Game 均通过。首轮离线 level.dat 哈希与客体一致；
  同盘冷启动以相同哈希重入世界，服务器在精确保存位置登录。两次正常退出 0、桌面
  D-Bus/OpenRC 自然关机及 ext4/FAT 检查 0；OpenAL ALSA Default 与非静音 WAV
  有直接证据。此前指针在客户端外的尝试不计转向通过；指针进入客户端后激活锁定，
  三次实际相对事件将朝向从 0/0 改为 6/-1.5，F3、Wayland trace 与磁盘 NBT 一致。

首个 `first` 尝试在 QEMU 启动参数处失败；`second` 的宿主截图检查失败后未完成输入，
均保留原始记录。`minecraft-first` 在欢迎界面后观察进程/客体终止，也不升级为通过。
N08 不再重复要求上述 Minecraft 保存/重载与实际交互。Firefox 的真实输入/正常退出也已
通过 `firefox-focus-verified/report.json`（77.723 秒）：HTTP 与验证证书的 HTTPS DOM、
标签页/刷新、可信鼠标点击、真实键盘 `a20` 和 Rime `你好`，浏览器正常退出 0，
桌面 D-Bus/OpenRC 自然关机，ext4/FAT 只读检查 0；实际截图已核对。

此前 `firefox-english` 的 SIGSEGV（-11）和其它失败保持原记录；它们使用的 Marionette
默认把 `focusmanager.testmode` 设为 true，DOM 虽有焦点，GTK IME 的窗口指针却始终为
NULL。临时测试 profile 显式恢复 false 后，原版 `im=wayland` 路径收到 OnFocusWindow，
英文与中文提交均通过；不是更改生产信号、沙箱或输入法策略。IBus GTK 模块的候选启动器
只能避开崩溃，未解决中文输入，已经撤掉，未进入生产 overlay。
[Mozilla 说明](https://firefox-source-docs.mozilla.org/remote/Prefs.html)明确用户偏好优先于
自动化推荐值；公共浏览器探针现在使用上述正常焦点配置的临时 profile。

下节补齐本表关键错误/权限语义并关闭 N08。最终支持平台/候选收口仍归 N18/N20；
不重复要求已通过的浏览器、JVM/Minecraft 行为，也不新增长驻留或穷尽矩阵。

## 应用关键边界收口（2026-11-17）

本轮在 x86_64 KVM/q35、ABI=both SMP4、真实 XFCE/OpenRC world 中补齐下列现有
短检查；直接证据仍位于上述 `desktop-current/`。没有重跑浏览器、游戏或全量套件。

| 契约边界 | 实际证据 |
| --- | --- |
| SA_RESTART/EINTR；线程共享处理表、SIGSYS siginfo/寄存器和 exec 分离 | `application-boundaries/report.json` 的 syscall_restart_test / seccomp_context_test 两个分项通过；该轮 sandbox 失败，整轮仍为 FAIL。VFS 修复不改这两个已通过的信号机制。 |
| 沙箱错误/继承、dumpable/ptrace、proc 链接权限；SCM_RIGHTS 的截断/回滚/CLOEXEC/关闭复用 | `application-boundaries-fixed/report.json` 的 sandbox_test / scm_stress 通过；不可转储的同 uid 子进程 readlink/open exe 为 EACCES，允许状态可访问，不存在 PID 为 ENOENT。 |
| poll/epoll 的错误/关闭；定时器与中断；网卡尺寸/短输出；终端与 FIFO 权限/断开 | 同一 fixed 报告的 poll_edge、timer_edge、net_iface_test（24 个断言）、tty_jobcontrol_test、tty_lifecycle_test、revocation 和 fifo 全部通过；FIFO 覆盖 `/tmp` 与 `/dev/shm`、ENXIO/EPIPE/EAGAIN、Landlock 直接/符号链接拒绝及两套 mknod 入口。 |
| 子进程恰好一次收割/ECHILD、futex 超时/隔离、管道 EOF/EPIPE；真实 exec 的 CLOEXEC | `process-boundaries/report.json` 全部通过。exec 检查明确使用 close_fds=False：带 CLOEXEC 的 fd 在 exec 后消失，清除该位后同一验证器按预期失败，证明 fd 实际继承，未由 Python 替内核关闭。 |

初次沙箱失败是 procfs 的 vnode 跟随路径返回了 chroot 外的全局可执行路径，而
readlink 快捷入口已返回可见路径，两者不一致。实现提交 `90d63fe4e` 共用既有路径
转换，并让数字 PID 的 exe/cwd 快捷入口执行既有访问检查，缺失进程返回 ENOENT。
现有 sandbox_test 增补实际访问正反检查；宿主 Linux 对照与当前报告一致。

两个最终补充运行分别耗时 37.892 / 31.017 秒，均自然关机、QEMU 退出 0、ext4/FAT
只读检查 0，无 FATAL/内核异常。最初在桌面组件仍启动时立即请求关机，退出阶段曾出现
xfsettingsd SIGSEGV，原始 FAIL 和日志保留；不把这条观察标为已修复，也不用该次早停
支持正常桌面生命周期通过。系统关机验收仍检查 OpenRC 收敛、写回、实际退出与磁盘
状态，不把被终止的每个用户进程零退出作为既有发布条件。

上述关键边界与前文六组真实应用运行共同满足 N08 的有界关闭条件。逐项索引为
`build/evidence/v1-closeout-20261116/proc-exe-follow/n08-closeout.json`，其中失败报告
只引用明确通过的分项，不把整轮升级为 PASS。N08 从开放清单删除；覆盖表保留 partial，
不声称支持所有 Linux syscall/flag，也不取代 N18/N20 的正式支持配置与发布候选验证。

## 证据与关闭规则

以上路径来自现有 world、接口说明和应用记录，不新增全 syscall 或全架构穷尽矩阵。N08 的关闭依据是上述实际应用与关键边界，不是清单建立或入口存在。

- 保留 [syscall 覆盖表](../../../../kernel/abi/linux/syscall_coverage.md) 的 partial/拒绝边界；每个已确认的应用阻塞对应具体调用和可复现行为，再安排修复。
- 复用仍适用的直接检查，不逐项重复历史全量。应用级运行与正式支持平台的关键验证合并到候选验收，记录内核/镜像、ABI、CPU/设备、命令、结果和跳过范围；平台矩阵仍归 N18/N20。
- 2026-11-16 本地核对发现 `tty-control-desktop-regression`、`aarch64-desktop-supervision-rust`、`aarch64-desktop-supervision-c-public` 的原始报告目录已缺失。对应 Markdown 历史记录保留，但不能当作当前工作区可复核的候选报告。
- Native 检查点通知另遵守[设计边界](../../native-abi/checkpoint-signals.md)，不在本条重新要求完整 Native 异步处理器。

本页不把依赖包安装、主机替代边界、历史通过或脚本存在升级为应用通过。后续发现已承诺路径的实际回归时，按具体缺陷重新登记；发布候选仍按交付矩阵执行。

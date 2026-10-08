# 2026-10-08 开发中断恢复与集成验收

## 远端同步与旧备份清理

2026-10-08 按用户要求执行 `git fetch --prune origin`，核对远端 main
`41cb8217c` 已是本地 main 的祖先；`git merge origin/main` 返回 `Already up to date`，
无需额外合并提交。本次推送目标为 `origin/main`，包含已验收源码及本次清理记录。

以 2026-09-08 为一个月前的界线，删除两个本地旧备份引用：

| 分支 | 删除前 HEAD | 备份日期 |
| --- | --- | --- |
| `backup/main-snapshot-20260820` | `07c31bb36` | 2026-08-20 |
| `backup/riir-original-20260816` | `728fc137e` | 2026-08-16 |

这两条引用没有可用的创建 reflog，按分支名称中的明确备份日期判断，并核对其 HEAD
提交日期也早于界线。保留其余六个较新的备份分支、research/riir 分支及独立恢复归档。
下文未推送与保留八个备份的说明属于此前阶段记录。

## 两个原始开发目标的补充验收

此前的恢复报告把网络范围收窄为既有 lane 切片的集成，没有完成用户原本要求的 lwIP
全局核心锁拆分。这一判断不完整；下面旧阶段的门禁通过不能代表该目标已经实现。
后续工作在 `codex/lwip-lane-locks` 中接续，按独立 lane 锁执行普通 TCP/UDP 核心路径，
保留共享协议控制屏障，并加入真实外部连接的并行验收。当前结构和验收边界见
[网络 lane 文档](net/net-lanes.md)及[网络锁契约](net/network-lock-contract.md)。

Hypervisor 的基础目标是让 A20OS guest 从 RAMFS 启动到 shell，能输入命令并正常退出。
在 main `fa2e505f3` 上重新运行 `make -j8 smoke-hyp-shell` 已确认：`hypvm -k
/bin/boot/guest-kernel-ramfs.elf` 启动 guest init/mksh，两条不同 echo 命令正确返回，guest
`exit` 后 host prompt 恢复并正常关机。日志为本机
`/tmp/a20-recovery-logs/hyp-shell-user-path-fa2e505.log` 及 guest
`.kernel-build/smoke/hyp-shell-riscv64.log`。这不扩大为多 vCPU、任意 Linux/rootfs 或
完整 guest 中断/设备模型的完成声明。

补充实现把 ingress/staging 与协议执行分离：普通单播 TCP/UDP 在 owner-lane 锁下运行，
逐包临时状态按 lane 保存，TCP 定时器逐 lane 推进。ARP/ND、分片、配置和全局维护仍由
全 lane 屏障保护；共享 heap、PCB 回调、缓存与 TX staging 各有短锁。Lane 根据协议元组
确定，不绑定 CPU。物理 pbuf arena 分片、多队列硬件/RSS 和 loopback 并行不在本次完成
声明内，见网络文档中的现状与后续范围。

补充验收使用实际外部 TCP 连接，而非仅凭 loopback 成功或没有 panic 判断并发成立。
首轮暴露 raw wire/sockaddr 端口与 PCB 端口字节序不一致，修复统一 owner hash 后复测。
最终四路各传输并校验双向 64 KiB；N1/N4 的子进程数据摘要与最终 verdict 一致。
N4 的 opt-in TCP 输入探针报告 `peak_active_lanes=2`、`probe_hits=406`，四 lane 输入计数
为 `107,102,92,105`，TCP timer 计数均为 `89`。它证明观察到了跨 lane 重叠执行，
不代表吞吐提升，也不要求四个 lane 在同一时刻活跃。

跨架构补充构建首轮还发现了既有入口绕锁问题：kernel bring-up 的 driver blob 会通过
`USER_BUILD_STAMP` 构建用户输出，而矩阵的 `check-<arch>-user` 直接运行 user Makefile，
二者可同时写同一目录。LoongArch 的 `.build-id` 重复初始化及同一目标重复编译后出现
ccache 目标文件缺失。修复把七个矩阵用户入口统一接到已有 stamp/输出根锁协议，保持
不同架构输出根独立并行；宿主回归覆盖入口接线、同根互斥与异根不互相阻塞。首轮失败
日志保留为 `lwip-build-matrix-first-failed.log`。

补充审查也修正了 socket/lane 交界的生命周期与配置问题：bind 在全 lane 屏障内
rebucket 后发布 owner，热路径先复查发布值才解引用 PCB；TCP/IP 选项在 socket 锁内
快照，释放后取得 core ownership 并用版本号校验重试，TCP 按请求字段应用。LISTEN
PCB 的完整 TCP 字段访问改为 listener 元数据继承，并补真实 lwIP 模式的监听后选项
回归。该门禁首轮还揭示长度为 4 的 `reno` 被末字节终止覆盖成 `ren`，已修正为有界
复制后追加终止符，未放宽验收预期；失败日志另存为 `lwip-netopt-first-failed.log`。

补充源码验收基线为 `a801e798b`：共享 allocator 保护为 `7168f6a08`，核心 owner-lane
并发为 `4b8288c09`，矩阵构建入口修复为 `446c67eda`，最后的 socket 生命周期与选项
同步为 `a801e798b`。后续只更新文档与合并记录。补充测试记录（完整命令输出位于
`/tmp/a20-recovery-logs/`）：

| 验证 | 结果与日志 |
| --- | --- |
| `make -j8 check` | 19 项宿主门禁通过，包含 285 项 Python 工具回归；`lwip-check-final.log` |
| `make host-tests` | 实际 lwIP allocator/pbuf 并发及实际 lane queue/context 宿主回归通过；宿主 CPU/IRQ shim 不替代内核实测 |
| `tools/test-tcp-cubic-host.sh` | 22366 项检查通过 |
| `make -j8 smoke-netopt-lwip` | 显式 lwIP 模式下 99 项检查通过，包含监听后选项与真实 accept 继承；`lwip-netopt-final.log` |
| `make -j8 smoke-net-lanes-hostfwd` | N1/N4 外部四路 TCP 数据校验及 N4 并行探针通过；`lwip-hostfwd-final.log` |
| `make -j8 NR_CPUS=4 NET_LANES=4 OPT='-O3 -DCONFIG_NET_PCB_SANE=1' smoke-netfilter-nat` | 四核四 lane 下 DNAT 后 owner 分派及连接通过；`lwip-nat-n4-final.log` |
| `make -j8 smoke-hyp-shell` | 补充网络实现上 guest shell 两条 echo、exit、host 恢复及关机通过；`lwip-hyp-shell-final.log` |
| `smoke-net-tcp-lanes`、`smoke-net-lanes-n1`、`smoke-lwip-memp`、`smoke-network-suite` | 专项回归通过；network suite 的 1 项既有声明缺失仍不算已实现 |
| `make -j8 ARCH=aarch64 NR_CPUS=4 NET_LANES=4 ABI=linux BRINGUP=0 kernel-only` | AArch64 四核四 lane 内核构建通过；`lwip-aarch64-lanes4-build.log` |
| `make -j8 check-build-matrix`（使用下文的临时 ARM 工具链环境） | 七个 hosted 架构的 bring-up 内核和用户态构建通过；`lwip-build-matrix-final.log` |

补充实现已以 `85401f000` 合入本地 main。核对 `a801e798b` 是 main 祖先且工作树干净后，
以非强制方式删除 `/home/fqwqf/OS/A20OS-lwip-core` 与 `codex/lwip-lane-locks`；现在仅保留
main 工作树，未合并的 backup/research/riir 引用继续保留。补充 guest 日志已复制到 main
的 `.kernel-build/smoke/`，并独立归档至 `/tmp/a20-recovery-logs/lwip-final-artifacts/smoke/`，
同目录保存源码基线和恢复报告草稿。该验收阶段尚未推送，后续同步见文首。

下面的范围、提交及验收记录描述此前恢复阶段，基线与本次补充实现分别标明；不能把
旧基线上的整套运行门禁说成已在补充实现上全部重跑。

## 范围与现场保留

本轮恢复的是已存在的开发切片：RISC-V H 扩展虚拟化、UART 接收保真、网络 lane 统计与门禁、MM 合并及跨架构构建，以及未提交的 AArch64 接口探针。没有把长期路线图中的全部功能列为本轮完成条件。

开始时 main 停在合并 `feat/virt-foundation` 的过程中，4 个文件在 index 中仍为 unmerged；虚拟化和 UART worktree 中还有未提交代码。修改前保留了各 worktree 的 HEAD、index diff、工作区 diff 和修改/未跟踪文件归档，保存在本机 `/home/fqwqf/OS/recovery-backup-20261008-004937/`。恢复验收时三个辅助 worktree 均干净、main 无待解决合并。随后按用户要求再次核对 HEAD 与完整工作树状态，使用非强制方式删除已完全合并的三个 worktree：`/home/fqwqf/OS/A20OS-virt`、`/home/fqwqf/OS/A20OS-wt3-lockshard`、`/home/fqwqf/OS/A20OS-wt3-uart`。同时删除所有已是 main 祖先的本地分支；未合并的 backup、research 与 riir 分支，以及独立恢复归档继续保留。

同步远端还发现了 origin/main 上 6 个集群提交，已以 `12e13c575` 合入；合并保留虚拟化与集群两组 Native ABI 登记。`feat/completion` 已是 main 的祖先，没有重复回放。其余未合并的备份、研究与 riir 分支不作为待合并功能分支处理，并按用户要求保留。

清理前逐一核对的开发分支包括 `feat/completion`、`feat/virt-foundation`、`wt/{correctness,drivers,embedded,features,netfilter}`、`wt2/{correctness,drivers,embedded,features,gates}`、`wt3/{lockshard,uart}`，均为 main 祖先，随后已删除这些 14 个本地引用。已合并的 `archive/legacy-desktop` 也一并删除。独立保留的未合并引用为 8 个 `backup*` 分支、`research/osdi-envelopes` 和 `riir`；远端引用未改动。主要合并点是 `0ad3ea143`（完成中断的合并）、`b34367255`（guest shell 收尾）、`39ff150f8`（网络分支）、`ea16644cc`（UART）；后续修复按缺陷和文档分别提交，保留可追溯历史。

## 最终行为与文档

| 切片 | 本轮结果 | 说明 |
| --- | --- | --- |
| 虚拟化 | guest RAM 缺页、浮点状态、控制台收尾修复；guest shell 运行；Linux hyp 控制面对 envelope 调用者拒绝 | [虚拟化用法](hypervisor/03-usage.md)、[边界与后续计划](hypervisor/02-roadmap.md) |
| UART | 硬件 RX 读取与 ring 插入共用互斥；有界 burst 缓冲与丢弃计数；往返测量要求 poll 路径实际执行 | [串口保真测量](measured/serial-fidelity.md) |
| RISC-V 陷阱入口 | 先保存原 t0，再借用它记录被中断的栈指针，避免异步陷阱破坏寄存器 | 本文运行时故障记录 |
| 调度与 park | 本地 yield 的 READY 转换与事件唤醒分离，保留尚未提交的等待 token | [进程与调度契约](process-scheduler.md) |
| MM | fork 在节点锁内取得快照、引用并降权；mprotect 保持共享页 COW；状态 COW 与旧回退路径共用地址空间锁 | [fork/mprotect COW 契约](mm-fork-mprotect-cow.md) |
| 网络 | 修复 lane 数字列渲染及 drain 重入全局锁；门禁解析真实 N=4/N=1 报告 | [网络 lane 模型与阶段记录](net/net-lanes.md) |
| Linux auxv | `/proc/<pid>/auxv` 输出 ABI 原生字长二进制 pair，固定 mm 生命周期；探针验证短读、offset、EOF 和初始栈一致性 | [AArch64 接口审计](aarch64-fex-preconditions.md) |
| 构建与门禁 | 补 ARM32 无 sidecar 后端接口、全部 hosted 架构 UAPI 头路径，修复符号链接构建根清理、构建目录漂移和递归 make 用户输出竞争 | [构建说明](build.md)、[门禁说明](testing-gates.md) |

COW 本轮选择先保证旧路径与状态路径之间的互斥，因此状态驱动匿名 COW 当前按地址空间串行化；不能沿用此前“无需 mm->lock”的性能描述。匿名 demand-fault 的其他状态路径与该选择分开讨论。

此前网络 C2 的 lane 索引与计数不表示物理池已完成分片；本报告旧阶段也没有拆分全局 lwIP 核心锁，后续补充工作见文首。多 vCPU、完整中断注入、virtio DMA、真实 H 扩展硬件，以及 FEX 完整运行，仍按各子系统文档明确的后续范围处理。

恢复验收额外修正了两个失败路径：H 扩展探测先检查启动 FDT 的早期能力快照，避免 QEMU `h=false` 时仅凭可读 `hstatus` 误判能力；lwIP lane drain 区分已持全局锁的 socket poll 与锁外 scheduler 调用，避免同一非递归锁重入。`a66a030c6` 在 MMU/PFA 初始化前保存 H/SSTC 能力，晚期查询不再依赖固件临时物理缓冲区；FDT 仍依赖固件如实声明平台能力。

`make -j8 dev-build` 暴露的递归构建竞争由 `14435993d` 修复：普通与 RAMFS 镜像可能各自启动 user stamp recipe，现在针对实际共享的用户输出根持有跨进程锁，在锁内重验 stamp，并让用户重建使 Native stamp 失效。不同构建选项不能绕过同一输出根的锁；相同内核输出目录仍不能由多个独立顶层 make 同时写入。

完整运行时验收还发现了独立的 RISC-V 陷阱入口缺陷。`__trap_from_kernel` 原先先用 t0 记录旧 sp，再把已覆盖的 t0 保存进 x5 槽；返回时恢复的是栈指针。它能破坏被异步中断的状态恢复代码，表现为 init/fork 阶段偶发以用户权限取内核指令。诊断确认实际 SATP 根、ASID 及高半叶 PTE 均正确，短启动循环第 9 次复现；`017c65057` 改为先保存原 t0，再重建旧 sp。此问题与用户构建目录竞争分别处理，没有把“重新构建后暂未复现”当作根因结论。

四核网络压力测试还捕获了 park 准备阶段的抢占交错：任务已经 PREPARING、仍为 RUNNING，IRQ-return 走 `proc_yield()`；旧实现调用的 `proc_make_ready()` 将它当成事件早到唤醒，只置 WOKEN，仍保留 RUNNING。调度切出后，switch completion 只给 READY 任务入队，导致该任务既不占 CPU 也不在队列。GDB 现场确认了空的四个运行队列、空闲 CPU、已解除锁的 RUNNING/WOKEN 父任务及其等待栈。`b6a0b47e9` 让 yield 独立发布本地 READY，保持 park token 的等待语义，避免把调度请求伪装成对象事件；`d79edb9c0` 增加 prepare→真正切出→恢复→commit→EVENT 唤醒→finish 的定点回归。

最终 `make -j8 check-doc-test-gates` 还暴露了聚合门禁的镜像竞争：顶层 `.NOTPARALLEL` 仅约束其直接前置，不会向下传递。blocking 子聚合中的 proc/futex smoke 仍同时递归构建同一 FAT 镜像，导致 mcopy 卡在损坏的分配链。只读 fsck 确认两份 FAT 表损坏，现场镜像与失败日志保存在本机。`cc4b5cce5` 显式串行化共享镜像的 blocking、timeout、MM 与 userland 子聚合；隔离 `make -j2` 回归使用真实串行声明通过，删除声明的负对照确定复现重叠。单独 ABI smoke 重建并运行通过，随后重新运行整个聚合，未通过修改镜像内容或延长 mcopy 等待掩盖问题。静态 blocking 门禁还发现只读快照的 `on_rq/cpu_id` 名称与 task 所有权写规则混淆；`79d029010` 将诊断快照字段改名为 `task_on_rq/task_cpu_id`，保留实际 task 字段和门禁规则；`3d1e5b24f` 修正了另一种 runqueue 快照读取者误用新名的编译回归。

## 验收记录

最终集成验证基线为 `3d1e5b24f`，后续提交仅更新文档。专项运行测试主要基于 `5736d7ba5`；随后增加聚合门禁串行规则及宿主回归，并将只读诊断快照字段改名以区别于 task 所有权字段，最终统一回归覆盖这些提交。测试使用实际构建变量解析出的目录，未继承旧镜像的历史 PASS。初轮发现的 ABI 表、能力覆盖分类、外部引用误解析、ARM32 链接缺口、用户态 UAPI 头路径及编译告警，均经过修复再复跑。

本机完整日志保存在 `/tmp/a20-recovery-logs/`，guest 日志按门禁定义保存在 `.kernel-build/smoke/`。它们是本机验收证据，不是发布产物；复现时应重新运行下面的命令。

- `make check`：19 项宿主门禁全部通过，包含 278 项 Python 工具回归；最终文档树上再次运行也通过，日志 `check-host-integrated.log`（前一轮为 `check-host-final.log`）。
- `make -j8 check-doc-test-gates`：在 `3d1e5b24f` 上完整通过，日志 `check-doc-test-gates-final.log`。实际运行并通过 SMP bring-up、proc、futex、timeout、sched、MM stress、MM fork/exec、MM seg-index overflow、VFS stress 与 driver lifecycle 十项 QEMU smoke，随后源码契约、DRM ABI 与文档引用检查也全部通过。首次失败的并发镜像现场及中间快照命名错误另存为失败日志，未沿用其部分 PASS 充当最终聚合通过。
- `make check-build-matrix`：七个 hosted 架构的 bring-up 内核和用户态完整通过，日志 `check-build-matrix-final.log`。运行前清空了专用内核缓存和七架构用户输出。ARM32 工具链和 UAPI 头临时解包在 `/tmp/a20-arm-toolchain/root/`，实际命令为 `PATH=/tmp/a20-arm-toolchain/root/usr/bin:$PATH LD_LIBRARY_PATH=/tmp/a20-arm-toolchain/root/usr/lib/x86_64-linux-gnu make -j8 FF_LINUX_UAPI_ROOT=/tmp/a20-arm-toolchain/root/usr check-build-matrix`；正常安装交叉工具链的环境可按 [build.md](build.md) 配置。在 `5736d7ba5` 上以同一命令再次增量运行，七架构全部通过，日志 `check-build-matrix-head.log`；最终集成基线 `3d1e5b24f` 再次完整迭代七架构增量构建也通过，日志 `check-build-matrix-integrated.log`。
- `make -j8 smoke-abi-linux` 与 `make -j8 smoke-riscv64`：在最终集成基线上均通过，前者输出 `SYSCALL_SMOKE: PASS`，后者完成 bring-up 并主动关机；不接受 watchdog 超时替代通过。日志分别为 `smoke-abi-linux-integrated.log`、`smoke-riscv64-final.log`，guest 日志为 `smoke-abi-linux.log`、`smoke-riscv64.log`。
- 集群 Python 自测 503 项通过；`make host-tests` 通过。实际内核 frame codec 对 golden vectors 的宿主比对通过（91 个 frame、16 个 SLIP vector，共 1889 项检查）；SLIP 宿主 runner 是独立移植实现，不能据此宣称已测内核 UART 字节流或多机互通。
- `make smoke-rv64-trap-t0`：真实定时器 IRQ 前后 t0 哨兵保持不变。暂时恢复旧保存序列的负对照触发 `RV64_TRAP_T0: FAIL` 和 panic，恢复修复后通过；日志 `/tmp/a20-trap-t0-negative.log` 及 `.kernel-build/smoke/rv64-trap-t0.log`。最终无诊断的 `017c65057` 内核短启动 20/20 次到达 shell 并正常关机（`/tmp/a20-shortboot-final-{1..20}.log`）。
- `make -j8 smoke-serial-fidelity`：在最终集成基线上四核加 guest flood 运行通过，300/300 条命令逐字节往返一致，`corrupted=0`、`never_returned=0`、`shell_not_found=0`，RX `dropped=0`、`poll_bytes=1`、`irq_bytes=16572`。`interleaved_tokens=2` 表示控制台输出交错，返回命令本身仍完整匹配；不将日志输出交错误判为 RX 损坏。日志 `smoke-serial-fidelity-final.log` 及 guest `serial-fidelity-riscv64.{log,summary}`。
- `make -j8 smoke-mm-pt-race`、`make -j8 smoke-mm-fork-exec-race`、`make -j8 smoke-mm-stress`：无临时诊断代码时全部通过，guest 日志分别为 `mm-pt-race-riscv64.log`、`mm-fork-exec-race-riscv64.log`、`mm-stress-riscv64.log`。其中 fork/exec 门禁包含新增的只读→读写 mprotect 后父子页隔离检查。
- `make -j8 smoke-rv64-sched-park-yield`：修复的本地 yield 逻辑两次通过；临时恢复旧的 `proc_make_ready()` 调用时，60 秒超时且没有 PASS，恢复修复并重建后再次通过。它要求 helper 真实观察到 READY/PREPARING 原任务在运行队列中，随后由 EVENT 完成提交的等待，并回收 helper，不能仅靠没有切换的 yield 通过。guest 日志 `rv64-sched-park-yield.log`。
- 四项网络门禁修复后复跑全部通过：`make -j8 smoke-lwip-memp`、`make -j8 smoke-net-lanes`、`make -j8 smoke-net-lanes-n1`、`make -j8 smoke-network-suite`。N4 传输为四路各 4 轮 × 1 MiB，occupancy 为 `0 0 1 0`，RX 计数总和为 1、drop=0、staged=0；N1/N4 的 stress verdict 完全一致。网络套件为 11 项通过、1 项声明缺失，不能将声明缺失项计作已实现。最终日志无 panic；当前结果另登记于 `a2e7a1ef6`。
- `tools/a20 test check-fex-precond-aarch64`：auxv 初始栈与 proc 二进制内容、PHDR/PAGESZ/AT_NULL、短读/offset/EOF 及 BTI/MTE/非法标志行为均匹配预期。日志 `virt-fex-precond.log`；输出明确为 `FEX_SUPPORT=NOT_ESTABLISHED`，不能据此宣称完整 FEX 可用。
- `make -j8 smoke-native-cluster`：`78eefc248` 将过时的 W0 stub 预期更新为 WA1 ABI 的有效 export/connect/loopback/status 契约，真实 guest 复跑通过。日志 `virt-native-cluster-rerun3.log`；不能据此宣称多机互通已完成。
- 虚拟化八项正向运行门禁全部通过：`smoke-hyp-selftest`、`smoke-hyp-vcpu`、`smoke-hyp-console-p0`、`smoke-hyp-a20os`、`smoke-hyp-vm`、`smoke-hyp-vm-96`、`smoke-hyp-console`、`smoke-hyp-shell`；负向门禁 `smoke-hyp-no-h` 确认无 H 扩展时安全跳过。对应最终日志为 `virt-hyp-{selftest-cache,vcpu,console-p0,a20os-final,vm-final,vm-96,console,shell,no-h-cache}.log`。a20os/vm banner 案例刻意没有 guest init/rootfs，其 guest panic/shutdown 是预期收尾；shell 门禁另行验证 guest 命令，不能混为一项能力。
- `make -j8 smoke-envelope`：功能、类型拒绝、权限、操作/数据预算、过期、重开、网络、hyp 控制拒绝、40 worker revoke-kill、SCM、pidfd、shmat 与审计均通过并正常关机，日志 `virt-smoke-envelope-rerun.log`。门禁超时预算增加至 180 秒以容纳 TCG 下的完整撤销压力，未删除验收标记。

## 尚存的独立限制

本地检查不能证明 GitHub Actions 已通过；初始验收阶段没有推送或发布，后续同步见文首，远端 CI 需在推送后重新观察。[CI 说明](packaging/ci.md) 中 2026-10-07 的红灯属于历史记录，2026-10-08 本地矩阵结果单独登记。

H/SSTC 缓存没有为所有消费者保留完整 DTB。当前 QEMU 板级设备按固定配置枚举；硬件平台的晚期 raw FDT 枚举路径仍需在 PFA 前复制 DTB 或排除其物理页，见 [虚拟化使用说明](hypervisor/03-usage.md)。这属于现有硬件平台层限制，不能将能力缓存描述为完整 DTB 生命周期修复。

额外运行的 `make check-format` 报告 119 个不在格式基线中的文件不合规（`format-inventory.log`），涉及既有 main 和导入的开发代码。新增 AArch64 探针已格式化；本轮没有扩张格式豁免名单，也没有把全仓格式重排混入功能恢复。该检查不属于 `make check` 的 19 项宿主门禁，不能将上述通过结果表述为“仓库全部检查均通过”。

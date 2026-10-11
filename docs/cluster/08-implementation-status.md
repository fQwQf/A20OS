# 统一集群实现与验证记录

开始日期：2026-10-11。独立 worktree：`unified-cluster`，分支：
`codex/unified-cluster`。原工作目录的未提交改动没有复制或修改。
目标和验收要求见 [07-unified-runtime.md](07-unified-runtime.md)。

## 1. 基线与本轮实现

基线已有六个 Native syscall、loopback、协议金样、UART 状态机与 MCU leaf。
关键缺口是 POLLING 没有执行者、UDP 不存在、可靠连接一律拒绝、实际跨内核
调用未验证；clusterd/jobd 也未实现。

本轮已实现：

- 独立周期泵；不会被服务队列阻塞的 RX 注入；初始化成功后才发布 ready。
- IPv4 UDP 适配器，独立 socket 引用生命周期、有界 peer 表、地址/身份绑定、
  challenge 握手、心跳和重启取消。
- RELAY 的 TTL/CRC 转发与路由哈希冲突拒绝；失败路由安装回滚、删路由取消代理。
- 协商 wire v1 的可靠单帧事务：双方会话、接纳前缓存预留、去重、回复重放、
  完成收据、8 次重试上限和 30 s 期限。v0 金样入口保持严格兼容。
- MCU 与主机共用会话/序号窗口、工作请求、可注册算子表、DOT/SUM64、能力查询；
  Native 用户态 agent 支持预先配置的进程任务、期限取消与重启恢复。
- 新增只读内核快照与固定远端发现槽位，目录不依赖用户守护进程寿命。
- Native clusterd、成员状态机、直接/间接探测、原子能力目录与租约、any 解析；
  连接 v2 在内核 admission 时核对 expected_boot_epoch。
- 共用有界作业状态机、能力/并发约束、重派与结果去重、二叉树归约，及真实 Native jobd。
- UART 有界 TX 队列与单字节流写入者、有界 RX、锁内发布状态快照。
- 代理 generation 与退出前禁止复用；导出引用不消费用户其他句柄的引用；
  服务撤销/替换结束未回复事务，旧 worker 不会干扰新导出。
- 空调用可以往返；超 MTU、队列/在途容量不足与内存分配失败不会静默挂起。
- MCU trim 构建缺失的必要兼容入口与单核 IRQ 保护 64 位 CAS 已补齐。

## 2. 实际运行证据

日志是工作产物，放在忽略的 `.kernel-build/cluster/` 下，不作为源码提交。
以下是本轮已经执行的场景；后续修改仍须重跑相关项。

| 命令/配置 | 已观察结果 | 证据范围 |
|---|---|---|
| `python3 tools/check-cluster-unit.py` | 60 项 C 帧检查、UART codec、共享算子、leaf、实际主/叶配对、10 万次序号回绕及 10 万次畸形工作请求通过；参考自检 503 项、C 对拍 1889 项通过 | 主机执行生产协议代码；不是物理板 |
| `make check-cluster-fuzz` | ASan/UBSan 下 100 万次确定性变异通过，198720 个帧通过解码，实际固件分发参与运行 | 主机内存/未定义行为检测；不是目标吞吐 |
| `python3 tools/check-cluster-loopback.py` | 29/29 通过，含哈希碰撞、路由回滚、在途满结束调用、删路由取消代理 | 单内核回归 |
| `python3 tools/check-cluster-udp.py --faults --restart` | 两端各 96 次可靠调用，服务重复执行为 0；丢包/重复/延迟、8 次重试耗尽、旧会话注入、重启重连通过 | 两套独立 RISC-V 内核；TCG |
| `python3 tools/check-cluster-mesh.py` | 三个独立内核 any、跨节点树、执行后丢回复重派、间接探测、成员死亡/重启、控制面替换通过 | Native 用户态 + 真实 UDP |
| `make smoke-native-cluster` | PASS，包含引用回归、真实 Native agent/子进程、期限终止、agent/clusterd 重启与 jobd 15 节点树 | Native 用户态 ABI |
| `make check-build-matrix` | 7 个主机架构内核及用户程序构建通过 | RISC-V64/32、LoongArch64、AArch64、x86_64、ARM32、PPC64LE 编译 |
| `CLUSTER_PROFILE=1/3` 的 RISC-V 内核构建 | MCU/服务器资源档均成功 | 默认档由矩阵覆盖；固件另行构建 |
| `make ARCH=armv7m BOARD=stm32f103 kernel-only` | 已成功链接真实板配置 | STM32 固件；没有物理板运行证据 |

ARM 固件构建使用隔离提取的 Debian `gcc-arm-none-eabi` 与 `binutils-arm-none-eabi`，
通过 PATH 提供工具链，没有修改原工作区或系统安装。

最新 UDP 故障日志可由 `make check-cluster-reliable` 重建到
`.kernel-build/cluster/reliable/`。检查器只拥有和清理自己的 QEMU 进程/socket。
详细场景见 [02-udp-demo.md](02-udp-demo.md)。

## 3. 验收进度：还没有完成全部设计

| 门槛 | 当前状态 | 仍需完成 |
|---|---|---|
| G1 基础数据面 | 协议和独立内核路径已验证 | STM32 持久配置已接入；x86 COM2 头节点绑定已接入；UDP→UART 独立双内核网关已通过；仍缺物理持久写入与硬件网关联调 |
| G2 可靠事务 | 单帧直连与主要故障已验证 | UART 双向会话确认已通过；协商分片和 64 KiB 业务负载已通过；间接目的双向会话与路径 MTU 已通过；完整 UDP 路径的多跳分片已通过默认/服务器/混合档故障测试；仍需更多网关故障场景 |
| G3 统一服务 | 工作协议、能力查询、注册表与实际 Native agent 已验证 | 物理 MCU/服务器混合联调；专用串口固件已接入 |
| G4 动态集群 | 成员/间接探测、租约、any 与 Native clusterd 已实现；本机恢复测试通过 | 三独立内核故障验证通过；gossip/自动租约路由已通过 UDP→UART 独立内核验证；持久配置与 Native 服务监管已通过；仍需物理部署与更复杂拓扑验证 |
| G5 分布式作业 | 共享图调度、重派、去重、二叉树和 jobd 已实现；UART/主机仿真混合树及 Native jobd 通过 | 通用字节结果、CONCAT 和真实混合词频树已通过；16 片/8 节点两种配比及执行节点退出重派已通过；仍缺物理混合部署及完整故障矩阵 |
| G6 硬化 | 正在补充可复现门禁 | 有界 fuzz 和资源档构建已通过；Native 运行资源快照与 UART 延迟记录已接入；10000 次 COM2 连续调用及任务资源回收已通过 |

这些缺口保持为未完成，不通过删除设计要求来标记成功。
当前大规模配置仍有全表扫描和每连接线程开销；尚无生产吞吐或规模承诺。
STM32 专用集群 main 已使用 UID、两页 epoch journal 和独立 USART2/3；原始 harness 默认演示配置和头节点
未配置 session 时的 tick 混合值仍不能作为量产持久配置。
当前服务导出按 FIFO 回复配对，任意乱序服务完成还需要显式事务接口。

## 4. 仓库基线门禁

全库格式门禁存在本轮以外的不合规文件；本轮修改的集群 C/头文件独立格式化，
不扩大格式豁免名单。文档引用检查曾发现已有的
`docs/roadmap/single-level-mm-model.md` 对 drvmod loader 的越界行号引用。
初次 `make check` 的 `host-tests` 因 lwIP 并发 harness 缺少 CPU 适配头与
CPU 数配置失败，`check-doc-drift` 因上述越界引用失败。已复用宿主 CPU shim、
保留四 lane 配置及真实分配器测试，并将引用更新到实际函数。最终 21 项
宿主门禁全部通过，含 8 线程各 20000 次 lwIP 分配器并发操作。
全库格式检查仍报告 114 个既有非豁免文件不合规，未扩大豁免名单。
`make check-doc-test-gates` 最终通过，包含实际 SMP、阻塞/退出、进程生命周期、
MM、VFS 与驱动运行检查；日志为 `.kernel-build/cluster/final-doc-test-gates.log`。

统一服务格式、资源限制、进程取消和目录枚举详见 [09-work-agent.md](09-work-agent.md)。

控制面与作业的实际格式、状态机及边界见 [10-control-plane.md](10-control-plane.md)、[11-jobs.md](11-jobs.md)。

三节点日志位于 `.kernel-build/cluster/mesh/`，`events.json` 记录一次运行的实际
注入：6 个 A→B 目录请求被阻断、2 次关联的 C 间接探测成功、3 个执行后作业回复
被丢弃、新 attempt 回复已观察到。数值是单次记录，不是固定性能指标。

UART 双向 challenge、双方重启后的旧 HELLO/ACK 重放及缓存保持已通过；两页
journal 通过 1000 次启动、216 个掉电点和已提交记录损坏拒绝。STM32 生产 main
接入持久身份，详细配置与维护边界见 [12-mcu-identity.md](12-mcu-identity.md)。

专用 MCU 配置 `STM32_CLUSTER_PORT=2/3` 与原外设配置输出目录隔离。
原配置禁用 leaf，集群配置使用独立 1 ms 泵和原始字节 I/O，USART1 仅控制台。
主机板级测试验证全部 256 个字节不转换、TX 失败、RX 溢出/硬件错误/回绕；
实际主叶配对也经绑定 I/O 运行。8 KiB QEMU probe 启动到 SysTick/调度器，
该配置禁用 leaf，不能作为集群硬件运行证据。

x86 COM2 实际绑定已通过 `make check-cluster-uart`：完整 x86 TCG 内核的
Native 客户端经 emulated 16550 串口调用独立进程中运行的生产 leaf，96 次
可靠 DOT、丢请求/丢回复及去重、固定目录、clusterd any、7 任务主叶混合树通过。
日志及源码 SHA256、环境、时间记录在 `.kernel-build/cluster/uart/`。
部署方法和证据含义见 [13-uart-head.md](13-uart-head.md)。

COM2 连续负载门禁还发现并修复 RX 时钟下溢误断链、Native 父进程持有内部
工作线程 zombie 两个问题。新增 detached 内核线程路径不继承用户资源，
10000 次新建连接后任务对象回到 9、僵尸为 0、生命周期错误为 0；普通
进程压力、SIGCHLD/退出边界与等待窗口检查也通过。详细测量口径见 UART 文档。

协商 UDP 分片已接入默认/服务器资源档：完整 64 KiB 请求与回复、乱序/重复/
丢片、首片期限、槽位及字节额度、OOM、会话拒绝和回收均有回归。服务器与
默认档混用时按较小预算发送。Native kind=4 查询重组资源；详细格式、测试
与本轮发现的 lwIP 发送 pbuf 泄漏修复见 [14-fragmentation.md](14-fragmentation.md)。

端到端 CS/1 会话、租约刷新、路径 MTU 与 leaf 固定来源窗口已接入，
`make check-cluster-gateway` 的独立 RISC-V 客户端经 UDP→x86 COM2 网关→
生产 leaf 完成可靠调用、双向 CS 丢包、leaf 重启、旧回复/调用重放、any 及
混合树。网关重启不能清除 MCU 原调用者去重窗口。格式、配置顺序及边界见
[15-routed-sessions.md](15-routed-sessions.md)。


## 10. 有界 gossip 与路由租约

`python3 tools/check-cluster-gateway.py --gossip` 已通过：客户端只有网关种子，
自动发现 leaf、完成 96 次 DOT、重启 leaf 后重新解析 epoch、拒绝旧 epoch 连接、
重放旧 CS/CALL、any 与混合树，以及拔掉串口后自动删除租约路由。97 个独立
DOT 事务对应 97 次实际执行；调用前后任务数 9→9、僵尸 0、生命周期错误 0。
这是两套 TCG 内核与生产 leaf 主机进程，不是物理硬件证据。

新增条件路由操作由 Native smoke 覆盖：显式路由优先、精确条件删除、动态
upsert、用户替换后旧清理不能误删。共享传播策略测试覆盖表满、跳数、优先级、
split horizon 和租约。Native 外层增量构建原来漏跟踪 UAPI 头文件，现已补齐，
头文件单独更新触发重编译的回归通过。详见
[16-gossip-routes.md](16-gossip-routes.md)。


## 11. 持久配置与服务监管

配置解析、三服务独立退避、singleton、子进程回收和 owner 强杀清理已接入；
Native 磁盘配置 smoke 实际启动 agent/clusterd/jobd 并完成 DOT 调用，覆盖崩溃
重启、配置恢复、正常停止和强杀恢复。共享解析器 10 万次变异通过 ASan/UBSan。
Native spawn 可执行文件重复释放引用的缺陷一并修复。协议和部署契约见
[17-deployment-supervision.md](17-deployment-supervision.md)。外层启动/监管者重启
由系统 init 或操作者负责；不声称已自动修改各平台启动策略。


## 12. 通用结果与词频树

共享作业状态机支持精确 0–168 B 输出、自定义有界归约封装；DOT 的原 8 B 接口
保持兼容。默认主机和 leaf 共用 WORD_MAP/WORD_MERGE/CONCAT。Native jobd 新增
CJ WORDCOUNT，文件 CLI 已纳入构建和真实 Native smoke。

`make check-cluster-unit`、`make check-cluster-fuzz`、`make smoke-native-cluster`
和 `python3 tools/check-cluster-gateway.py --gossip` 已通过。最后一项完成 736 B
词频树，同时使用主机与 UART leaf，并丢弃首个 MAP 回复：MAP 实际执行 1 次，
独立事务 1 个，最终结果与整体参考逐字节一致。STM32 64 KiB Flash/20 KiB RAM
USART2 固件链接成功，text=47496 B、data=0、bss=8992 B；这不是全部 RAM 用量，
线程栈/堆与持久 Flash 区另有链接预算。详见 [18-wordcount.md](18-wordcount.md)。


## 13. 放弃调用的缓存回收

调用者关闭在途 endpoint 时发送原会话/事务绑定的放弃收据，避免全部回复丢失
后 MCU 单缓存持续占用 35 s。执行中记录只标记放弃，worker 完成后才可复用；
通知先于原请求时先提交序号窗口，阻止迟到执行。通知是尽力交付，不承诺取消
远端业务；丢失时仍有缓存保留期限。

`python3 tools/check-cluster-gateway.py --gossip --abandon` 已实际丢弃首个 MAP
事务的全部回复；作业重派一次，词频正确，后续 MCU 查询在 2.5 s 内成功。
生产主机回放测试及 UART 配对测试覆盖通知先于请求、执行中预留、旧 worker、
旧序号和 OOM；ASan/UBSan 通过。对应协议见 02、11、18 文档。


## 14. 多跳 UDP 分片

CS/1 传播最小 MTU 与只能缩减的分片资格；完整 UDP 路径按端点资源档较小值
分片，中继只转发物理帧。目的端先校验端到端会话与协商限额，再分配重组。
新旧 CS 混合或经过 UART 时保留单帧降级，协议格式和预算不变。

`tools/check-cluster-udp.py --routed --faults` 的默认档、`--profile 3` 服务器档
和 `--profile 3 --peer-profile 2` 混合档均已通过。三套独立 TCG 内核，两端各
96 次可靠调用与 8 次大消息，故障覆盖丢请求/回复/ACK/收据、丢片/乱序/重复、
9 次全丢请求（初次加 8 次重试）后结束。服务器档回显 65536 B 业务；混合档
回显上限 4042 B。所有配置大消息前后网络堆为 736→736 B，重组结束 0 个/0 B。
日志在 `.kernel-build/cluster/routed-default/`、`routed-server/`、`routed-mixed/`。
共享默认/服务器会话单测验证路径降级与限额，ASan/UBSan 通过；详情见
[19-routed-fragmentation.md](19-routed-fragmentation.md)。


## 15. 16 片、8 节点词频演示

`make demo-wordcount` 已实际通过两种配比：8 套 Native RISC-V 内核，以及 4 套
RISC-V、2 套 x86 COM2 网关和 2 个生产 MCU leaf 主机进程。固定文件 1014 B、
78 个词、8 个不同词，形成 16 个 MAP 与 15 个 MERGE，结果与独立 Python
参考逐字节一致。基线先按真实目录能力向每个节点各派一片，剩余图交给 Native
驱动；每个最终目标/attempt 均记录，确认全部 8 个执行节点参与。

第二棵树使用正常 any 派发；节点 2 执行 MAP 后，检查器丢弃其回复并关闭该
内核。被丢失的分片必须在别的节点以 attempt>1 完成。两种配比都通过此检查，
随后实际 jobd 和文件 CLI 继续输出正确词频。混合配比的 leaf 7/8 分别执行
2/2 个独立 MAP，对应接纳事务 2/2 个；请求事务 3/3 个，其中未接纳并放弃的
请求不误记为执行。这是本次观测，不要求轮询选择保证固定分布。

混合部署的多个 clusterd 都会探测 leaf，静态来源槽位从 4 扩为 8，以容纳六个
主机控制面的来源会话。单回复缓存和单在途不变，不逐出去重窗口；满表仍拒绝。
共享构图器支持 1024 B、16 片/31 节点，Native 驱动支持共享图最多 64 节点。
1025 种文本长度及精确 16 片树通过单测和 ASan/UBSan；原 DOT 接口保留。
数据集、命令和资源边界见 [18-wordcount.md](18-wordcount.md)。该验收仍是
QEMU 与主机 leaf 的证据，不含物理 STM32 板运行。

## 16. 故障硬化、诊断与最终构建

`make check-cluster-hardening` 的三个场景已通过：两端各 1000 次调用加 10%
随机丢可靠帧，重复执行为零；50–200 ms 单帧延迟、CL bit 损坏且 UDP 校验
仍合法；每端 1000 个伪造身份 HELLO；30 秒双向网络分区；服务端 10 秒不
应答而调用方按 2 秒预算关闭代理。分区调用 15 秒内结束，恢复后新请求成功；
慢服务执行一次且调用方在途/重试字节归零。重启及旧会话重放仍在矩阵中。

新增 Native snapshot kind=5，以原 192 B 信封读取聚合链路桶、收发、策略
丢弃、malformed、重传及桶数上限。损坏帧测试要求接收内核 malformed 增长，
不只依据宿主的“已注入”事件。完整语义与证据边界见
[20-hardening.md](20-hardening.md)。
公共链路状态也同步 UDP/UART 的 SUSPECT；分区测试实际观察到 SUSPECT 后
调用结束，恢复后 UP。UDP→x86 COM2→leaf 的 gossip/丢尽回复/重启回归仍通过。

最终七架构 `check-build-matrix` 通过；七架构 Native 集群测试程序、agent、
clusterd、jobd、supervisor 和文件 CLI 也全部链接成功。ARM32 使用独立
`arm-none-eabi-gcc`，RISC-V32 显式以 `NATIVE_CC_riscv32=riscv64-unknown-elf-gcc`
选择其真实 RV32 multilib。32 位 SDK 仍有既有指针宽度转换警告；这次编译
不能当作七架构实际运行证据。运行证据主要是 RISC-V 与 x86 独立内核。

`smoke-riscv64`、Native cluster/supervisor、三节点 mesh 最终回归通过。
STM32 64 KiB Flash/20 KiB RAM 的 USART2/3 固件均链接，当前每份 text
47504 B、data 0 B、bss 9376 B。Flash journal、固定栈与运行堆预算另计，
不能只以 text+bss 推断可用堆。所有物理板验证与 8 小时生产 soak 仍保持未完成。

CI 已配置宿主 unit/fuzz 与独立内核运行门禁，手动工作流增加完整硬化和词频
配比测试；未在本地把“配置存在”写成 GitHub 已执行成功。

## 17. 连续大消息、描述符回收与测量入口

新增 `make check-cluster-performance`，由真实 Native 融合 channel_call 测量
本机、loopback 和两套独立内核 UDP；每个阶段逐字节核对回复并检查真实服务
执行次数。基准程序七架构链接通过，不要求 MCU 承担主机侧采样缓冲。

连续大消息暴露了原 virtio-net 的完成编号误解：把描述符链头当作 available
ring 下标进行第二次映射，TX 回收错误槽位，RX 也做了相同转换。现在按完整
32 位描述符编号校验，移除每设备 1024 B 的错误映射。生产完成函数提取测试
证明旧版断言失败、修复版通过乱序/无效编号/30 万次回绕；修复后真实 UDP 的
1000 次 64 KiB 测量加 100 次预热通过，重复执行计数未增加。

新增五分钟 sanitizer 入口，300.960 秒执行 217 批、2.17 亿次变异，全部通过；
快速百万档保留。新描述符门禁加入本地与 CI 的宿主列表；本地 `make check`
现在为 22 项，已通过。七架构矩阵也在驱动修复后通过。配置远端工作流不代表
它已经执行；物理板、业务单向 SEND 与完整四节点 8 小时 soak 仍未验收。

基线与候选各五次独立启动、每次 10 万次本机调用，观察均值变化 −4.875%，
置换检验 p=0.00794；95% 区间 [−7.114%, −2.914%]。这不是 ±3% 严格等价证明，
也不归因于没有修改的本机 IPC 路径。数值、方法和瓶颈见
[21-performance.md](21-performance.md)。

修复后直连 UDP 的故障/重启、服务器档及混合档三节点路由、COM2 静态 leaf
网关回归全部通过；完整文档测试门禁在性能测量结束后独立运行通过。


## 18. 可靠单向业务 SEND

新增可选 ONEWAY 能力、只写 Native 连接和独立单向导出；MCU 可在握手前注册
一个无堆静态 sink。两端共用 epoch、序号窗口、空完成回执、退休收据、背压
与分片，CALL 的业务 FIFO 不接收 SEND。默认能力不变，升级约束见
[22-oneway.md](22-oneway.md)。

`make check-cluster-send` 已通过服务器档持续 10% 丢帧和三内核混合档路由，
两端各接纳 264 个 SEND，重复业务接收为零，另有各 96 个并发 CALL。
`--send --faults --restart` 已通过旧业务 CALL/SEND 重放隔离。生产 UART/leaf
静态 sink 在宿主通过 256 次接纳、768 个重复帧及错误/旧 epoch/路由测试。

本轮修复了丢退休收据导致完成缓存耗尽、流水线越过未接纳旧序号，以及 2 秒
重组期限早于事务重试周期的问题。重组期限改为不续期的 30 秒，同时在分配前
过滤已接纳请求/已结束回复，并在退休和代理关闭时清理部分重组。64 KiB SEND
复用扩展长度编码，边界 65534/65535/65536 B 已通过实际内核故障测试。

完整网关 SEND、物理 MCU、四节点性能与 8 小时 soak 仍未验收；本节没有把
新增接口存在、主机单测或短时双内核运行当作这些验收完成。


本轮 22 项宿主门禁、百万变异 ASan/UBSan、Native 集群 smoke、七个 CI 主机
架构的内核/用户构建及 Native 集群测试程序均通过。扩大到全部板级构建的
`check-build-matrix-all` 因环境缺少 `loongarch32-unknown-elf-gcc` 失败，未记为
通过；随后按 `print-ci-kernel-arches` 派生的七主机矩阵逐项通过。
STM32 USART2/3 均链接，text=47936 B、data=0、bss=9440 B，仍不代表物理运行。

本轮 `make check-doc-test-gates` 全部通过；1498 个文档行号引用检查通过。


## 19. 单向 SEND 的完整混合传输链路

`tools/check-cluster-gateway.py --send` 已通过：真实 RISC-V Native 程序经 UDP、
x86 COM2 和生产 leaf 接收 128 条 8–206 B 消息。MCU 业务回调按编号核对全部
字节，重复接收为零。故意丢退休收据后观察单缓存 NO_SPACE 背压与同事务收据
重发，后续接纳继续；丢请求/ACK/完成回执、重复帧、leaf 重启后旧 SEND 重放
均通过。原 96 次可靠 DOT、any、混合算子与词频树、任务资源回收也继续通过。

初次完整链路运行耗时 32.441 秒，DOT 实际执行/独立事务为 97/97，MAP 为
1/1。这是该次仿真记录，不是吞吐或硬件性能承诺。归档位于
`.kernel-build/cluster/evidence/gateway-send-initial/`；`check-cluster-send` 已增加
完整网关入口。四节点吞吐、8 小时 soak 和物理板仍待验收。


加强证据检查后再次以 `--send --abandon` 通过，33.267 秒；丢失的 SEND 退休
事务号为 7，实际观察到同事务 2 个退休帧，128 条消息全部精确接纳。该次
MAP 为 2/2，首次 attempt 的全部回复被丢弃后，作业以新 attempt 重派成功。
原 `--gossip --abandon` 场景随后独立通过，默认未启用 SEND 的行为保持可用。
Native 集群测试程序再次完成七架构链接，文档引用检查通过。


## 20. RX 与 UDP 就绪唤醒

RX 入队直接唤醒有界队列的消费者；UDP adapter poll owner 等待 socket 就绪，
最迟 1 ms 继续串口与链路控制。使用已有 park token 防止登记与 commit 之间
丢失唤醒，等待期间持有 socket 引用。RX 空闲期限 10 ms，重组仍由单一 owner
清理，没有向普通 Native channel syscall 增加分支。

新 `check-cluster-wakeup` 提取实际生产函数，在 ASan/UBSan 下通过 RX 与 UDP
各 25 万次交错边界及失败/引用平衡检查，并接入本地与 CI 宿主门禁。23 项
宿主门禁通过；服务器档持续丢帧、混合档多跳 SEND、完整 UART SEND 网关、
原默认可靠调用/重启及 gossip/放弃作业回归均通过。

完整性能测量通过：本机/loopback 各 10 万次，UDP 64 B 一万次、64 KiB 一千
次。loopback 均值 0.860 ms，UDP 64 B p50 6.484 ms；分别仍超过三倍本机与
5 ms 目标。64 KiB 顺序 CALL 双向有效负载 1.747 MB/s。数值和原始日志位置
见 [21-performance.md](21-performance.md)，四节点与长时验收仍未完成。

## 21. 单网络通道的 SMP 内存保护

新增双 CPU 集群故障测试时，曾在 `ip4_output_if_src` 触发 `p->ref == 1`
断言。根因是 lwIP 堆锁和 `SYS_ARCH_PROTECT` 的跨 CPU 锁错误地只在
`CONFIG_NET_LANES > 1` 时启用；单通道并不意味着只有一个 CPU 分配数据包，
socket 发送路径会在取得 lwIP core 锁前分配 pbuf。主机测试改为执行实际
`lwip_compat.c` 和生产堆锁宏，旧单通道代码也复现了双重释放。

现已在所有通道配置下启用堆锁与可嵌套的引用计数保护，保持 heap→protect
锁序和本 CPU IRQ 状态恢复。单通道、四通道分别以 8 线程×20000 次真实
堆分配/缩小/合并、memp 与 pbuf 引用操作通过，ASan/UBSan 通过。宿主门禁
同时运行两种配置，不再用独立的 pthread 算法代替生产保护逻辑。

真实双 CPU 与四 CPU 内核分别在持续 10% 丢帧下通过双向 96 次可靠调用、
最大 64 KiB 分片及每端 264 条 SEND/96 条并发 CALL；重复业务执行为零。
日志分别保留在 `rx-event-smp2-fixed/`、`rx-event-smp4-fixed/`；修复前失败在
`rx-event-smp2/` 和 `smp-allocator-before/`。`make check-cluster-smp` 将双 CPU
场景纳入可重跑门禁，并进入 CI 扩展检查。这些短时回归不替代长时 soak。

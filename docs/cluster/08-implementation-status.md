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

## 22. 四节点 SEND 的真实负载

新增 Native `native-cluster-load` 与 `make check-cluster-load`，四个独立内核
互相收发 64 B SEND。接收者拥有固定单调时间窗口，应用编号历史独立检查
重复；最终通过真实 CALL 查询所有消息接纳及资源归零。宿主仅转发真实
Ethernet 帧，性能达标布尔值与业务正确性通过分别报告。

60 秒正式运行通过数量与资源核对，每方向 165.767–193.283 条/秒，所有
成功提交消息最终送达，重复为零；明显未达 >10000 条/秒目标。初次忙循环
让出 CPU 的客户端曾发生链路超时，已保留失败现场；使用正常阻塞背压后
通过，但该进展故障尚未完成定位。程序完成七架构链接，CI 手动扩展任务
已接入。方法、原始日志与剩余验收见 [23-four-node-load.md](23-four-node-load.md)。


## 23. RX 空间背压与 slab CPU-array 窗口

增加共同 RX ring 满与分配失败的原因计数，仍使用 192 B kind=5 信封的
保留指标；无 next-hop 的诊断桶不再冒充 DOWN 物理链路。poll owner 在
32 项高水位等待空间，消费者降至 31 项时唤醒，ring 容量保持不变。
每个 adapter 连续 100 ms 未轮询时仍推进一次有界控制 I/O。
生产函数交错测试新增 25 万组空间等待边界。

负载触发了 slab 慢路径的既有 IRQ 窗口：检查 CPU-array 后恢复 IRQ，重入
可清空数组，drain 随后下溢，迁移也可留下旧 CPU 指针。现保持本 CPU IRQ
屏蔽直到 refill/drain 结束；原中断状态和锁外 OOM reclaim 保持原语义。
新增 `check-slab-cpu-window` 执行实际生产函数体，旧代码复现越界，修复后
10 万组边界、重入、迁移与 IRQ 恢复检查通过 ASan/UBSan，并加入 CI 门禁。

当前 100 ms 控制轮询保障版本的四节点无主动丢帧 60 秒核对通过，RX ring
满/分配失败均为零，吞吐 187.017–201.983 SEND/秒。持续 10% 丢帧的 60 秒
诊断版本也通过，但约 66–78 SEND/秒仍低于 soak 负载要求；此前一次
warmup-query 失败仍未定位，不能据后续通过结案。

全部 24 项宿主门禁、完整 `check-doc-test-gates`、当前双 CPU 持续丢帧场景
通过；七架构 hosted 构建和 Native 负载程序链接通过，STM32 USART2/3 固件
仍成功链接。隔离本机十万次 CALL 均值 69.864 µs，单次测量不作统计等价
结论。失败现场、指标成本、精确测量范围见
[23-four-node-load.md](23-four-node-load.md)。

## 24. UDP 邻接表的稀疏轮询

主机 UDP 维护与 peer 内容同锁发布的占用位图，轮询仅锁定快照中的已占用
槽位。SERVER 档只有一个 peer 时，实际生产函数的锁访问从每轮 4097 次降至
3 次。位图与轮询局部数组各增加 512 B；DEFAULT 档各 32 B，静态 leaf 不变。
其他 peer 查询仍有容量扫描，不能据此声称全部全表开销已消除。

新增生产函数提取测试，覆盖 4/65/256/4096 容量、网关别名、增删交错、容量
耗尽、槽位复用、心跳边界与每档 10000 次变更，ASan/UBSan 通过；旧源文件
稳定违反稀疏锁预算。入口已纳入 `check-cluster-unit`。

四节点各 60 秒无主动丢帧及持续 10% 丢帧均通过精确交付与资源核对，前者
216.333–239.950 SEND/秒，后者 67.017–83.250；ring 满与分配失败为零。
性能和长期验收目标仍未达到，旧查询失败继续保持未定位。方法与完整数字见
[23-four-node-load.md](23-four-node-load.md)，这不是多 MCU 单系统能力的运行证据。

本轮 24 项宿主门禁和七架构 hosted 矩阵通过，实际 UDP 翻译单元在七架构
均重新编译并链接；双 CPU 持续 10% 丢帧场景通过 96 CALL/8 分片大消息及
每端 264 SEND/96 并发 CALL，记录的 QEMU 参数确为 `-smp 2`。
UDP 64 B 一万次 CALL 的 p50 为 3.136 ms，本次低于 5 ms；64 KiB 一千次
顺序 CALL 为 1.575 MB/s，低于上一历史测量，原因未定位，不能声称普遍提速。
两阶段业务总执行 11200 次且回复逐字节一致。完整数字见
[21-performance.md](21-performance.md)。

## 25. 多物理 MCU 原生共享内存内核原型

2026-10-12，按用户允许增加共享 RAM 和硬件仲裁的方向，新增可选的
`MCU_SHARED_KERNEL=1`。每个 MCU 执行同一原生固件，各有私有启动栈和
当前任务指针；普通任务表、PID、运行队列、堆和任务栈只有一份，直接放在
共享地址空间。复用服务器的 SMP 接口、调度器和任务生命周期，第一版使用
同 ISA、静态拓扑、协作调度；未设置该选项时保留现有 MCU 构建路径。
不以性能作为这个功能里程碑的验收条件。

已交付独立共享链接布局、唯一冷启动初始化、从核释放、硬件门保护的
1/2/4/8 字节原子操作和共享堆、统一时间与周期 IPI。测试固件通过真实
Park/Wake 和 affinity 接口将同一 PID 的线程从 CPU1 迁到 CPU0；迁移后
原栈内容、原堆指针保持有效，并可在另一 CPU 释放分配。上下文切换保存
PRIMASK，异常调用 C 时保持栈对齐。单芯片复位被 sticky attach 拒绝，
不能清除运行中共享状态；恢复仍要求整域复位。

两个、三个独立 Cortex-M3 指令引擎执行实际编译 ELF 通过验收；双 CPU
还通过每次只执行一条指令的交错。检查覆盖唯一主入口、全部从核上线、
真实原子门争用、256 次累加和 64 位进位、迁移后定时唤醒、IPI、两个任务
退出后主核回到 idle，以及错误 MAGIC/拓扑/早期异常拒绝。移除固件副本
中 PRIMASK 恢复指令后测试失败，负向对照有效。最终 ELF 未含跨芯片不适用
的 LDREX/STREX。测试器不修改任务状态或替换调度算法，但它仍是指令模拟，
不是物理 MCU 运行证据。

新增 `hardware/shared-mcu/a20_fabric.sv` 事务核心，三端口 Verilator 测试
通过启动权、attached/release、IPI、互斥门、RAM/控制操作完成顺序、字节
通道、错误所有者释放、故障后写屏蔽和整域复位，RTL lint 无警告。
尚缺 FSMC 异步捕获/CDC 桥、实际 RAM 后端、FPGA 引脚/时序闭合和物理板
验证；RTL 与固件外设模型分开测试，尚无联合仿真证据。当前 FSMC 接线
只允许至多 1 MiB，未连接 NWAIT，不能直接当作 Minecraft 所需硬件。

回归：默认 MCU、Xuanwu、USART2/3 叶子、STM32 QEMU 和服务器双 CPU
`ABI=both` 构建通过；默认 STM32 QEMU 到达原 idle probe，服务器 QEMU
报告 2/2 CPU 上线、系统就绪并正常关机。24 项集群宿主门禁、trim 注册、
SMP 边界和 11 项新配置拒绝检查通过。全仓格式门禁仍有 114 个既有漂移
文件，新加 C/H 文件已格式化；本次尝试服务器 `ABI=native` 仍在原有
`sys_core.c` 到 `sys_execve` 链接处失败，不能计为通过。

可重跑入口是 `make check-mcu-shared-kernel` 和
`make check-mcu-shared-fabric`，报告位于
`.kernel-build/cluster/shared-kernel/`；提交对应的固件、报告和日志归档在
`.kernel-build/cluster/evidence/native-shared-<commit>/`。
完整硬件契约、构建方法与限制见
[25-native-shared-kernel.md](25-native-shared-kernel.md)。
后续仍需真实多 MCU 验收、用户程序执行、设备/IRQ 所有权、JVM 和图形输入
栈；当前没有启动真实 Minecraft Java 客户端，目标尚未完成，应用验收见
[24-single-system-minecraft.md](24-single-system-minecraft.md)。

## 26. FSMC 桥、推导 RAM 与完整固件联合仿真

2026-10-12，在第 25 阶段原生共享内核上补齐每 MCU 的 FSMC 异步事务桥、
带 byte enable 的推导 FPGA RAM 后端和顶层连接。新共享模式接 PD6/NWAIT，
启用 ASYNCWAIT；原固定周期方案不能覆盖任意仲裁等待，已由响应完成后才
释放 NWAIT 替代。默认 MCU 模式仍不执行这些配置。

三端口总线测试通过 40 笔事务，包含同发访问、共享门唯一所有者、长时间
保持 strobe 时破坏性寄存器只执行一次、DQ 方向/保持、跨端口字节写入、
RAM 跨整域复位保留和请求撤销后的全域错误围栏。RAM 故意延迟 3 µs，
超过旧固定 DATAST，仍正确等待。桥错误 `0x82` 锁存，已受理事务排空一次
且可能已经写入，不能声称回滚；其他端口可继续读出 FAULT。仲裁核心回归
及顶层 Verilator lint 通过。

新增 `make check-mcu-shared-rtl`，实际双 Cortex-M3 ELF 的每次共享 RAM
读写与控制寄存器访问均经 C++ 引脚适配进入同一套 RTL 桥、核心和 RAM。
原生调度器完成同 PID 的 CPU1→CPU0 迁移、同一栈/堆继续使用、定时唤醒、
跨核 free、原子累加、两任务退出及返回 idle；CPU1 隔离复位被实际 RTL
的 sticky attach 拒绝并锁存 fault=5。检查实际固件 BCR/PD6 配置，且与
独立寄存器 oracle 对比。两个执行顺序种子均通过，其中 seed=20 由最终
Make 入口复现；禁用高字节 RAM 写入的 RTL 副本令完整固件失败，反向对照
有效。日志和报告归档在本阶段提交的 `shared-bus-<commit>/` 证据目录。

联合测试限定逐条指令：Unicorn 2.1.4 的 RAM hook 配合某些多指令边界会
跳过 Thumb 指令，空 hook 也复现；原无 RAM hook 的指令测试继续通过。
早期失败和空 hook 复现日志保留。联合适配器逐笔完成总线事务，因此并发
在途的桥接证据来自独立三端口总线测试。这里没有真实 MCU 内部 FSMC、
完整 NVIC、电气 CDC 或物理时序证据。

当前共享正式固件与三 CPU 自检固件构建通过，三 CPU 指令测试再次通过；
默认 STM32 QEMU 固件重新构建与第 25 阶段归档逐字节相同。此前报告的全仓
格式漂移和服务器 native-only 链接失败没有在本阶段修复或宣称通过。
尚需 FPGA 目标、I/O wrapper、RAM 资源及综合/时序报告、布线/供电/整域复位
和真实板验证。推导 RAM 不等于已交付外部 SRAM/SDRAM 控制器；默认 1 MiB
参数也不证明某块 FPGA 容量足够。用户态、JVM、图形输入和真实 Minecraft
客户端仍未实现，整体目标保持未完成。接口与复现见
[25-native-shared-kernel.md](25-native-shared-kernel.md) 和
[hardware/shared-mcu/README.md](../../hardware/shared-mcu/README.md)。

## 27. 原生应用、阻塞 SVC 与公共 fabric 抽取

2026-10-12，增加独立可选的 ARMv7-M 静态 PIE 应用模式。实际 SDK 应用从
共享 RAM 执行，非特权 Thread/PSP 通过真实 SVC 入口转入特权 Thread 继续点，
使用与服务器相同的 Native sleep/yield/get_cpu/exit 处理函数。应用休眠期间
通过现有亲和性接口将同一 PID 从 CPU1 迁到 CPU0，随后保留原栈地址、局部
内容、重定位指针与 BSS，正常退出并释放程序镜像/栈。8 种坏 ELF 拒绝且堆
完整恢复，坏用户指针与未知 syscall 返回明确错误。

新增 `check-mcu-program`、`check-mcu-program-rtl`。完整固件与实际桥/仲裁/RAM
RTL 联合测试通过，共享数据、异常帧与共享代码字节核对进入 RTL；CPU1 单独
复位仍被 sticky attach 拒绝。测试模型补充 Cortex-M 异常压栈/退栈，执行
真实内核与应用指令，不修改任务状态。它不是完整 NVIC 或物理板；tick 注入
不代表实际 CPU 周期。移除返回用户态的非特权位后，负向固件被测试拒绝。
两个执行顺序种子通过，资源释放后主 CPU 回到 idle。

按用户关于通用性的最新要求，启动/加入、互斥门、通知与围栏已抽到
`core/shared_fabric.c`，通过 `a20_fabric_io_t` 接受总线、屏障和 IRQ 适配，
不依赖 MCU 宏或 ARM 指令。STM32 实际固件使用这份公共驱动。x86-64、
AArch64、RV64 执行同一份公共驱动的 2–16 端口测试通过，ARMv7-M/RV32
freestanding 编译通过。公共 SMP 接口、调度器与域间 CL/作业协议继续复用。
公共接口的通用性不表示各板的硬件适配已经完成。

回归：x86-64、AArch64、RV64 `ABI=both` 双 CPU 内核构建并在 QEMU 上报告
2/2 上线、System ready。RV64/AArch64 正常关机退出；x86 打印关机消息后由
测试夹具终止，不能声称其 QEMU 电源关闭已通过。默认 STM32 QEMU 到达原
idle probe，其固件与此前归档逐字节相同。旧共享内核线程测试及启动拒绝
回归、trim 注册、9 项 SMP 平台边界和 4 项新配置拒绝检查通过。全仓格式
门禁仍为 114 个既有漂移，新 C/H 文件格式检查没有增加失败。

本阶段没有完整 Native ABI、MPU 保护、独立应用内核栈、libc/TLS/VFS 启动、
JVM 或 Minecraft。ARM32 参数仍为单寄存器宽度。树莓派专用板级入口、
RISC-V MCU 多芯片适配、各平台物理验证及执行能力匹配也仍缺失。
报告在 `.kernel-build/cluster/shared-program/` 与 `fabric-portability/`，
本阶段提交对应归档为 `evidence/portable-domain-<commit>/`。
细节见 [26-native-programs.md](26-native-programs.md) 和
[27-portable-domains.md](27-portable-domains.md)。整体目标保持未完成。

## 28. 四 ISA 共用硬件门原子运行库

2026-10-12，将 ARMv7-M 的原子 libcall 实现移至 `core/fabric_atomics.c`，
由 `CONFIG_SHARED_FABRIC_ATOMICS` 选择。平台只绑定当前 CPU 的 fabric 与
不可返回的故障 stop hook；IRQ token 全程为 `uintptr_t`。默认一致性 SMP
原子路径保持原样，ARM 共享固件实际改用这个公共实现。

同一测试与生产驱动/原子库在 x86-64、AArch64、RV64、RV32 执行通过。
包含 1/2/4/8 字节原子读写、CAS、交换、算术/位操作、失败时 expected 更新、
回绕、64 位进位，以及每个端口 46 个实际 GCC libcall 的门屏障计数。
四 ISA 的独立故障进程也均证明：fabric 已故障时先恢复 IRQ token 并 stop，
不会执行随后对未映射地址的 atomic load。ARM/RV32 freestanding 编译通过。

实际 ARM 应用与 RTL 迁移测试、旧双 CPU 内核线程及三 CPU 指令回归通过；
默认 STM32 QEMU 固件仍与归档逐字节一致，RV64 双 CPU 内核再次构建通过。
报告仍使用 `fabric-portability/`、`shared-program/` 与 `shared-kernel/`。
本阶段证据单独归档为 `evidence/portable-atomics-<commit>/`。
RV32 的 Linux syscall shim 只是测试工具，不是一个新 MCU 板级端口。
树莓派、非 ARM 多芯片板级适配、应用同步、JVM/客户端和物理验证仍待完成。


## 29. 树莓派原生端口与跨架构 SMP 功能矩阵

2026-10-12，新增 BCM2711 Raspberry Pi 4 board，接入既有公共 SMP/调度/PID/
Park/Wake/退出机制。架构入口增加可选的布局与 EL 归一参数，board 仅绑定
PL011/GIC 地址、GPIO、固件 CPU/RAM 事实和 spin-table 启动。
有界 FDT reader 位于公共 core，静态元数据在分配器启动前收窄 RAM。
GICv2 device adapter 由 virt 和 Pi 共用，Pi 的时间换算读取实际 CNTFRQ。

修复 AArch64 C 与异常入口假定 Aff0 等于逻辑 CPU ID 的问题；CPU 本地
TPIDRRO_EL0 不随任务上下文保存/恢复。QEMU raspi4b 的四核、CPU 节点乱序、
只启用硬件 CPU0/CPU3、reserve map 和 reserved-memory 五类有效配置执行
真实线程自检；五类无效固件描述在初始化早期拒绝。单核构建另到达 System ready。
同一 `core/smp_selftest.c` 在 x86-64、AArch64、RV64 双核内核运行。
测试验证同 PID/栈迁移、跨 CPU 堆对象、公共 sleep、所有在线 CPU timer IRQ、
IPI 应答与 worker/controller 退出回收；不是性能或模拟任务状态测试。

新增 `raspberrypi4[-qemu]` 实例、`check-raspberrypi4`、`check-smp-native` 与
`check-fdt-view`。FDT reader 的宿主 ASan/UBSan 边界测试覆盖 926 个截断、
926 个字节变异及七个无效 header。报告在 `raspberrypi4/`、`smp-native/`、
`fdt-view/`；提交对应证据在 `evidence/raspberrypi4-<commit>/`。
AArch64/RV64 新功能测试为 QEMU 自然退出，x86 在关机输出后由夹具停止。
Pi poweroff/reboot 仍为 halt，
测试在终态输出后停止模拟器，不声明其电源关闭通过。

这里只完成树莓派的原生启动/SMP 功能端口。没有 Pi 物理验证、网络与存储、
完整用户态、远端 TLB shootdown 或跨板共享 RAM；非 ARM MCU fabric 适配、
应用线程同步、完整 Native ABI/JVM/图形输入和 Minecraft Java 客户端仍待实现。
源码和启动契约见 [树莓派端口](../platforms/raspberrypi4.md)；公共机制边界见
[27-portable-domains.md](27-portable-domains.md)。整体目标保持未完成。

本阶段回归：既有多 MCU Native 应用/RTL 联合测试通过，固件 ELF 仍为
`8fdfd50a4cc133c7fda7c790db4dc7179caca8752327842ac528376514277c19`；
默认 STM32 QEMU 固件仍为 `c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。
trim registry 与九项 SMP 平台边界检查通过。全仓格式门禁仍有 114 个既有
漂移；本次修改/新增 C/H 的格式检查没有增加失败，没有改写格式豁免基线。

## 30. MCU/服务器共用执行信息查询

新增 Native 0x0002 `execution_info` 与 CW/1 QUERY_EXECUTION=4，公共 64 B 描述
区分镜像 ISA/位宽/端序/调用 ABI、Native 版本、接口、地址模型及特权原子机制。
同一处理函数同时接入服务器 syscall 表和 MCU Native SVC。Native agent 从内核
查询事实后发布；旧 agent 未提供描述时返回 UNSUPPORTED，旧 CW QUERY 不变。
修复既有 abi_info 的指针宽度写死 64。公共匹配器检查镜像清单要求，但还没有
完整镜像清单工具、远程 ELF 传送或自动派发；PROCESS 仍为预装 manifest。
测试与剩余范围见 [28-execution-capabilities.md](28-execution-capabilities.md)。

本阶段验证：真实 RISC-V Native 用户态/agent 查询及原有集群作业 smoke、MCU
原生应用/RTL、三种服务器 ISA 公共 SMP 和树莓派十类启动配置均通过。
同一执行描述/CW 协议测试在 x86-64、AArch64、RV64 宿主指令环境和宿主
ASan/UBSan 通过；这些宿主测试不替代内核或物理板验证。默认 STM32
固件 SHA256 仍为 `c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。
ABI 边界、文档漂移/引用和裁剪注册表门禁通过，证据归档为
`evidence/execution-<commit>/`。完整目标保持未完成。

全仓格式门禁仍为 114 个既有漂移，本次没有增加失败，也未修改格式豁免基线。

## 公共 Native 应用同步（2026-10-12）

MCU Native 程序与服务器复用 `sys_native_sync.c` 和 `ipc/futex.c`，新增
0x0B02 atomic32 公共接口，解决非特权应用无法直接使用特权 fabric 原子库的
缺口。执行能力新增 ATOMIC32 位；MCU 同时公布真实接通的 FUTEX。默认 MCU
配置仍裁剪相关代码。详细契约、测试入口与限制见 [29-native-sync.md](29-native-sync.md)。
完整跨板硬件矩阵、Native 运行时和 Minecraft Java 客户端仍未完成。

实际验收：RV64、AArch64、x86-64 双核内核运行同一 Native 同步测试均通过，
MCU 原生应用及 RTL 联合验证通过，原有集群作业和 Linux futex 压力回归通过。
x86 测试暴露的 C 线程入口栈偏移已放入架构适配，Linux clone 不走该路径；
RV32 Native 同步程序编译链接通过，尚未声明其用户态开机验收。证据随提交
保存在 `evidence/native-sync-<commit>/`，格式门禁的既有 114 项失败未增加。

## 统一 Native 用户线程（2026-10-12）

共享 RAM MCU 与 MMU 服务器复用 thread_create/task_wait/handle_close 处理函数、
生产句柄表和公共调度配置接口。创建先完成句柄和用户写回，再发布任务；失败
回滚。Native 可加入线程保留退出记录，Linux clone 的自动回收规则保持不变。
flat ISA 适配仅准备上下文与共享资源引用，通用生命周期位于 proc 模块。
同一应用测试覆盖嵌套创建、共享堆/futex、跨 CPU 亲和性、join 与关闭；MCU
额外覆盖活跃栈不能 unmap，以及主线程先退出后子线程继续使用映像和初始参数。
执行能力新增 THREAD_CREATE=128，MCU 仍不公布完整 HANDLES。完整接口、
验证和未完成项见 [32-native-threads.md](32-native-threads.md)。

本阶段实际验证：三种服务器 ISA 双核的父/子线程均报告 CPU1→CPU0，RV64
单核路径报告 CPU0→CPU0；MCU
真实 ELF/SVC 与 RTL 联合运行通过，五次退出后程序资源全部回收，内核堆恢复
至应用启动前基线。生产 syscall 的七条发布/失败路径在 x86/AArch64/RV64
宿主环境通过。原有 Linux 进程压力、futex 压力与 Native 集群作业回归通过。
默认精简 MCU 固件保持原 SHA256；格式门禁仍为同一组 114 个既有漂移，未增加
失败或修改豁免。物理多板、其他 MCU ISA 适配、全局实例/对象代次和 Minecraft
仍待推进，不将这些回归表述为完整目标完成。

本阶段提交的报告、内核、用户 ELF 与日志归档于
`.kernel-build/cluster/evidence/native-thread-<commit>/`，清单记录 SHA256。

## 任务身份生命周期（2026-10-12）

TASK/THREAD/DEBUG 从 PID 值改为独立引用对象。任务回收先解除弱绑定，任务
最后引用释放后允许 PID 复用；保留的旧句柄、dup 和在途 IPC 引用仍标识原对象，
不会解析到新任务。对象不强引用任务，避免自身句柄环与地址空间长期占用。
等待、调度、资源、调试、内存、namespace 与事件键复用同一公共身份路径。
这补齐执行域内部生命周期，不等于全局对象目录或跨重启身份。
设计、复现命令和证据边界见 [33-task-object-lifetime.md](33-task-object-lifetime.md)。

## 公共实例命名空间（2026-10-12）

新增 runtime_info/runtime_bind/handle_identity 公共 Native 接口。管理员显式绑定
instance/domain/boot_epoch，绑定在本内核启动内不可变，控制服务重启只重读该
状态。任务对象增加独立序号；dup、join 后句柄和 PID 复用不混淆对象名。
固定结构与显式小端编解码适用于 32/64 位平台。配置可选 instance+epoch，旧
配置保持可用。细节与验证入口见 [34](34-runtime-namespace.md)。

BOUND 仅是命名空间配置状态；持久代次分配、全局对象目录与
Minecraft 仍未完成，不将这些接口描述为完整单系统映像已经实现。

实际验证：三种服务器 ISA 双核、共享 MCU 原生 ELF/SVC/RTL、第二种 MCU
交错、supervisor 身份冲突和重启均通过。新测试修复 ARMv7-M 迁移任务与 idle
栈被异常处理覆盖的问题；异常处理改用独立 CPU 私有栈。默认精简 MCU 固件
及 RV64 Linux-only 原始内核二进制与上一提交完全一致。详细证据及性能边界见
[34](34-runtime-namespace.md)。

## 运行实例成员准入（2026-10-12）

公共 runtime 身份已接入 CD/2 和 CM/2；绑定的 clusterd 按实例/域检查直接和
间接发现，不接纳错误实例、未绑定节点或与保留记录矛盾的启动代次。首次绑定
清空旧服务视图，控制进程重启重读内核绑定。旧 CD/1、CM/1 和普通 any 接口
保留。MCU leaf 和服务器共用协议，具体状态、成本、持久历史限制与实际验收
入口见 [35](35-instance-membership.md)。完整全局运行状态和 Minecraft 未完成。


实际 RV64/AArch64/x86 混合实例流程两次通过，覆盖跨 ISA 工作调用、首次绑定、
外部实例/未绑定域排除、重启复用代次拒绝、全新代次和控制进程恢复。新增统一
可选 timing-scale 配置，默认值保持原行为。混合运行暴露的 AArch64 异常返回
IRQ 竞争已修复，双核 Native 同步/线程/分配器及 AArch64 Linux futex 回归
通过；旧三节点集群和普通
Native IPC/signal、Linux proc/futex 等回归通过。默认精简 MCU 二进制保持
不变，实际 UART leaf 固件编译链接通过。物理板运行、完整全局对象/应用状态
与 Minecraft 客户端继续保留为未完成；原始证据及成本见 [35](35-instance-membership.md)。

## 全局任务对象目录（2026-10-12）

新增与 ISA/板卡无关、无需堆分配的 CO/1 目录状态机和可选 Native objectd。
对象由持有本地能力的进程显式发布，以实例/域/启动代次/对象序号定位；SDK
在跨域打开时核验 CD/2 身份与传输代次。目录可查询 LIVE/RETIRED，管理续租、
撤回、断开清理与重新发布序号。旧通道、旧 publication 和旧内核对象名分别
隔离目录重启、重发布和域重启。普通应用与内核热路径没有自动目录工作。
这是对象元数据查询，跨域原生任务操作和统一应用生命周期仍待实现。
协议、部署、MCU 适配及实际验收见 [36](36-object-directory.md)。

## 实际应用启动与负载分配（2026-10-12）

新增公共 app owner、可选 Native appd 与统一 SDK。真实 ELF 按清单匹配 ISA
变体，经真实 task_spawn/wait/kill 管理；按活动程序数选择兼容执行域，启动时
复核采样负载。重复请求和终态保留避免重复执行，未知结果只查询原 owner。
三种服务器 ISA 联合运行实际程序；MCU 测试引导使用同一 owner 调用真实装载器。
接口、复现命令与按程序数量均衡的准确边界见 [37](37-application-execution.md)。
物理多板、MCU 常规网络启动服务、完整应用资源代理和 Minecraft 仍待实现。

普通应用部署补充了带 argv/envp 的清单文件和实际 `appctl-run` 命令，复用
原应用 owner 与按活动数分配接口，见 [38](38-application-launch-inputs.md)。

CA/2 增加 owner-issued slot token 与显式终态回收；新版 CLI 在观察退出后
回收记录，固定表可持续执行，运行任务和原 CA/1 记录保持。机制与验收见
[39](39-application-recycling.md)。
首次双入口真实运行暴露了一次 AArch64 init 引用下溢；后续重跑通过但根因
尚未定位，仍保留为长期稳定性验收的未解决项，不能据重跑通过视为修复。

## 同一应用内的工作领取与 CPU 分摊（2026-10-13）

公共 SDK 新增 a20_work_range_init/take，复用既有 atomic32 syscall，在共享地址
空间中由工作线程竞争领取索引，不预先分配固定份额或目的 CPU。单个 Native
应用的两个真实线程共同处理 32 项计算，检查没有重复和遗漏、结果与实际 CPU。
统一用户态代码用于共享 MCU、RV64、AArch64、x86，接口与验收见
[40](40-native-application-balance.md)。跨网络域的实际应用启动仍按活动程序数
分配，不据此宣称已经按 CPU 利用率均衡。AArch64 引用下溢补充失败现场输出，
尚未定位根因；物理多 MCU 与 Minecraft 继续保留未完成。

## Native 程序文件读取（2026-10-13）

精简共享内核新增显式启用的只读 FAT32 扇区后端，复用服务器 Native 文件
系统调用验证、生产 vfile 引用和句柄接口。公共用户程序在两个工作线程中实际
读取文件，再共同领取计算任务。服务器 Native open 的临时 fd 泄漏已修复，
新增 4100 次打开关闭回归并用父提交内核观察到失败。构建与证据见
[41](41-native-files.md)。文件支持不等于 JVM、物理 SD、多板或 Minecraft
已经运行；执行能力发现的文件支持位和 AArch64 偶发引用下溢仍待处理。

## 从文件启动 Native 子进程（2026-10-13）

共享 flat 内核接入既有 task_spawn v1/v2，程序可打开 ELF 文件、传参并等待
独立子进程退出。标准 CRT、参数/环境快照、Native ELF 标记和发布前回滚
边界见 [42](42-native-file-spawn.md)。普通 full VFS 装载路径保留，JVM、
物理多 MCU、Minecraft 与 AArch64 偶发引用问题尚未完成。

实际 Native 任务的公共停止、信号检查点及资源回收见
[43-native-task-control.md](43-native-task-control.md)。

持续纯计算 Native 线程的用户态时间片、停止与实际执行检查见
[44-native-user-preemption.md](44-native-user-preemption.md)。

真实程序的调度记账、并发句柄所有权、就绪与等待中断修复见
[45-native-execution-reliability.md](45-native-execution-reliability.md)。

无 CPU 绑定的真实 pthread 执行、空闲 CPU 接取和线程组终结通知见
[46-unbound-program-execution.md](46-unbound-program-execution.md)。

真实 JamVM Native 启动、完整类库构建和文件映射修复见
[47-jamvm-native-bootstrap.md](47-jamvm-native-bootstrap.md)。

真实 pthread 定向通知、每线程 mask、信号等待及多架构执行回归见
[48-native-thread-signals.md](48-native-thread-signals.md)。

真实 Java 多线程计算、Native JNI 文件操作和单核/双核自动执行验证见
[49-native-java-execution.md](49-native-java-execution.md)。Java 应用阶段已通过；
GC、物理 MCU Java 运行时与 Minecraft 客户端仍待继续实现。

原生 Java 的并发回收、栈根保存和实际对象移动已在 RV64 Native 单核/双核
验证，见 [50-native-java-gc.md](50-native-java-gc.md)。物理多 MCU、其他架构
完整 VM 和真实 Minecraft 客户端仍未验收。

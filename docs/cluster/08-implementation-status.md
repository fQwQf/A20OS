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
| G1 基础数据面 | 协议和独立内核路径已验证 | STM32 持久配置已接入；仍缺生产头节点 UART 注册、物理持久写入与网关联调 |
| G2 可靠事务 | 单帧直连与主要故障已验证 | UART 双向会话确认已通过；仍缺分片重组、间接目的会话发现、资源统计和更多网关故障场景 |
| G3 统一服务 | 工作协议、能力查询、注册表与实际 Native agent 已验证 | 物理 MCU/服务器混合联调；专用串口固件已接入 |
| G4 动态集群 | 成员/间接探测、租约、any 与 Native clusterd 已实现；本机恢复测试通过 | 三独立内核故障验证通过；gossip/自动路由与配置监管仍未完成 |
| G5 分布式作业 | 共享图调度、重派、去重、二叉树和 jobd 已实现；UART/主机仿真混合树及 Native jobd 通过 | 物理混合部署、通用结果/词频演示及完整故障矩阵 |
| G6 硬化 | 正在补充可复现门禁 | 有界 fuzz 和资源档构建已通过；持续负载、运行资源统计与可核验性能记录未完成 |

这些缺口保持为未完成，不通过删除设计要求来标记成功。
当前大规模配置仍有全表扫描和每连接线程开销；尚无生产吞吐或规模承诺。
STM32 专用集群 main 已使用 UID、两页 epoch journal 和独立 USART2/3；原始 harness 默认演示配置和头节点
未配置 session 时的 tick 混合值仍不能作为量产持久配置。
当前服务导出按 FIFO 回复配对，任意乱序服务完成还需要显式事务接口。

## 4. 仓库基线门禁

全库格式门禁存在本轮以外的不合规文件；本轮修改的 C/头文件独立格式化，
不扩大格式豁免名单。文档引用检查曾发现已有的
`docs/roadmap/single-level-mm-model.md` 对 drvmod loader 的越界行号引用。
这些不能报告为本轮验证通过，最终门禁结果继续在此记录。

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

# V1.0 人工 triage 需求与已确认的文件系统缺口

快照日期：2026-10-14。这是从所列活动计划与状态页中人工审阅出的一组有界开放需求，共二十条，外加一条
单独跟踪的已确认 ext4 完整性缺口；它不是完整的 1.0 TODO 清单。部分条目与首波台账里的优先队列重叠。
在所述行为真正交付之前，条目保持开放；一次历史通过、一个只有编译的结果、或一次没有覆盖该条契约的
QEMU 运行，都不关闭它。在注明的场合，当前 C 路径仍是行为参照。除非条目点名了已有的 Rust 实现，否则
不指派 Rust owner；其余条目在声称 Rust 完成之前需要另行划定 ownership 迁移。这些条目提供一份人工的
工作包对照表；扫描出的源码行在生成的 JSON 清单里仍是 `needs_triage`，直到逐条显式链接进去。

| ID | 工作包 / 需求链接 | 来源与子系统 | 要求的行为 | 当前 C/Rust 归属 | 下一步可验证交付 |
| --- | --- | --- | --- | --- | --- |
| N01 | V1-CORE / `V1-CORE-IRQ` | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，调度/IRQ | 线程化 IRQ 工作，以及长 syscall 期间低优先级任务的有界进展；正常的按需分页/COW 不得误触该界。 | C 调度器与 IRQ 返回路径；无 Rust owner。 | 增加有界进展断言与故障注入用例，在单 CPU 与 SMP 上运行，保留延迟分布与 fatal 扫描。 |
| N02 | V1-DELIVERY | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，CI/发布 | 在配置好的云端 CI 里重跑 Native CI 与发布前置流程，记录确切的候选版本与被跳过的硬件专属 job。 | 现有 C/Rust 构建与工作流文件；由 CI 操作者执行。 | 取得该候选的完整工作流结果；对每个被跳过的 job 分类，跳过的不得计为覆盖。 |
| N03 | V1-ABI / `V1-ABI-CROSS` | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，ABI | 证明选定的 Linux ABI 调用在 `ABI=linux` 与 `ABI=both` 下对外可见的成功、错误、权限与副作用行为一致。 | C Linux 与 Native syscall 路径；无 Rust owner。 | 在两种档位下跑同一 fixture，把归一化结果与独立预期值比对。 |
| N04 | V1-ABI / `V1-ABI-IDL` | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，IPC/IDL | 完成 C/Rust 绑定与版本协商，覆盖重叠、不相交版本、畸形确认与遗留回退，双向都要。 | C 服务 `user/svc/echod.c`；Rust 对端 `user/svc/rust_idl_peer.rs`；`user/svc/idl_negotiation.rs` 里的协商逻辑是 helper，不是对端。 | 用 guest 测试当前与捕获的遗留 C 对端，再测 C 客户端对 Rust 服务；断言确切的协商版本与业务负载。 |
| N05 | V1-DRIVER / `V1-DRIVER-DUAL` | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，驱动/IOMMU | 让同一份生产驱动源码在 kernel 与 user 两种模式下部署；验证按设备的动态 DMA 域绑定、越权 DMA 故障处理与拆除。 | 目前是两套独立的 C 驱动实现；没有共享的 Rust owner。 | 选一个完整驱动，在两种模式下证明同样的 probe/data/IRQ/DMA/removal 契约，包括被拒绝的 DMA 与清理。 |
| N06 | V1-ABI | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，Linux personality | 在 Native 原语上补齐 fd table、mmap/VMO、sockets 与 epoll 的必需语义子集，并做行为与性能对照。 | C personality façade 与 Native 内核；无 Rust owner。 | 逐操作注册语义差异测试与 guest 负载；功能契约通过之后才测性能。 |
| N07 | V1-CORE | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md) 与 [锁路线图](../../roadmap/perf-overhaul.md)，同步 | 先测只做注册的 B1 锁基线，再在锁改动之后用足够多轮次重复同样的争用负载，以分辨噪声。 | C 锁实现与 perf 计数器；benchmark owner 是子系统维护者。 | 冻结负载、亲和性与指标；归档 B1 与候选的成对序列，报告不确定性，不在看到结果之后放宽阈值。 |
| N08 | V1-ABI | [Linux syscall 覆盖](../../../kernel/abi/linux/syscall_coverage.md)，Linux ABI | 只把记录在案的边界情况与错误行为确实被执行过的 syscall 区域提升为完成；不要用相邻 syscall 推断整个区域。 | C syscall 实现；无 Rust owner。 | 每批选一个部分区域，加表内链接的边界/错误测试；只有运行时证据到位后才更新覆盖。 |
| N09 | V1-MM / `V1-MM-DONTUNMAP` | [路线图 TODO §P0](../../roadmap/a20os-improvement-todo.md)，MM/swap | 验证源 PTE 已换出时的 `MREMAP_DONTUNMAP`，包括槽/引用保留、源 refault 语义、目标数据与槽释放。 | C 与可选择的 Rust 事务；当前 guest 注入只覆盖私有匿名常驻页。 | 增加确定性的 swap-in guest 用例，核对源/目标字节与换入换出计数，然后同进程重试并审计。 |
| N10 | V1-DRIVER | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，PCI/MSI-X | 暴露按 function/按 queue 的向量选择与命令行 CPU 亲和性，而不是只有全局 CPU ID。 | C PCI/MSI-X 与 x86_64 APIC 实现；无 Rust owner。 | 为两个 queue 验证所选表索引、向量路由与亲和性；保留中断计数与回退行为。 |
| N11 | V1-DRIVER | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，架构/中断 | 为保留的非 x86 平台实现 MSI-X 消息地址、向量设置与 CPU 路由，或按设备类给出显式且有测试的回退。 | C 架构适配层；无 Rust owner。 | 每个保留架构构建并运行一个支持 MSI-X 的设备，验证中断投递或记录在案的 INTx/轮询回退。 |
| N12 | V1-DRIVER | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，e1000e/网络 | 证明 e1000e 的 MSI-X 向量真的承载了包流量；仅解析并武装向量表不够。 | C e1000e 与 PCI MSI-X 路径；无 Rust owner。 | 在 QEMU 下收发数据包，同时独立核对 queue/vector 计数与干净的拆除。 |
| N13 | V1-DRIVER | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，USB 生命周期 | 验证拔出会中止 endpoint URB 并归还 hub/下游总线地址，不留下陈旧完成或泄漏槽位。 | C USB hub/HCD 与 `usb_disconnect_port()`；无 Rust owner。 | 使用 QEMU 支持的热拔拓扑或确定性的 fake HCD 接缝；断言请求被中止、地址被复用、审计零泄漏。 |
| N14 | V1-FS | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，ext4/JBD2 | 实现剩余的 JBD2 特性语义：barrier/async-commit 广告、ordered 与 journal-data 模式、fast commit，以及 revoke 记录的延迟回滚用途；绝不宣称实现并未提供的行为。 | Rust 拥有有界事务注册表/阶段/重试状态（`user/svc/jbd2_txn.rs`）与把 descriptor/revoke 解析为有界重放动作（`user/svc/jbd2_record.rs`）；C 保留块 IO、校验和原语与重放应用。 | 同种子 C/Rust 解析器冷恢复 guest 各自重放 1 个事务/5 个块，返回已提交的文件内容并产生相同的干净 postimage；独立的 e2fsprogs 恢复也通过。完整的 barrier/order 模式、其他崩溃切点、fast commit 与延迟回滚 revoke 语义仍未关闭。 |
| N15 | V1-FS / `V1-FS-PERSIST` | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，ext4 兼容性 | 按路线图要求给出 JBD2 校验和 v1/v2 的显式降级策略，而不是静默失败关闭；不要推断旧格式支持不在范围内。 | Rust 策略把干净、结构合法的 v1/v2 日志选为只读；C 校验格式/校验和并执行挂载。 | 独立的 e2fsprogs 已校验干净的 v1/v2 镜像；QEMU 以只读方式挂载它们，而脏镜像与校验和损坏的镜像被拒绝。不宣称遗留重放/写支持；crash/replay/remount 覆盖仍未关闭。 |
| N16 | V1-FS | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，ext4 完整性 | 以兼容格式实现文件数据块完整性校验和；路线图把静默数据损坏记为未解决的完整性缺口。 | 目前没有 data-checksum owner；C 的元数据/JBD2 校验和路径不覆盖文件内容。 | 定义兼容的校验和元数据，在读取时检测损坏的数据块，并验证重挂载与日志重放之后的行为。 |
| N17 | V1-CORE | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，VFS/SMP | 测量并发 open/write/unmount 下 bucket、vnode 与 mount table 的争用；当前只有网络负载，不触发这些锁。 | C VFS 与锁计数器；无 Rust owner。 | 增加带并发操作与稳定完成/计数断言的 SMP VFS 负载；比较同种子运行。 |
| N21 / `V1-FS-ORPHAN-01` | V1-FS / `V1-FS-PERSIST` | Ext4 unlink/orphan 生命周期 | 当最后一个链接被移除而 vnode 引用仍然存在时，保持 ext4 的磁盘 inode/目录不变量；在服务或系统重启之后恢复或回收这类 inode。 | 共享 C ext4 后端递减磁盘链接计数并记录 final-link orphan；Rust UFSD 生命周期/orphan 控制器拥有有界 open 计数、链校验、reap 决策与服务代次 quarantine。C 仍拥有 extent/块 IO 与补偿。 | R14 成对 C/Rust 控制器在 N=5 的部分截断失败/SYNC 重试中通过并产生相同的干净 postimage。R15 在同样的 Rust UFSD 协议/生命周期上注入 write=5 与 restore-write=6 失败：两者都返回 close errno 117、消耗 fd 并拒绝后续 create/SYNC；ext4 postimage 相同，但 `e2fsck -fn` 报位图差异。这说明补偿不确定后失败关闭式 quarantine，不是磁盘修复。硬链接/open-unlink 生命周期、服务死亡恢复、无日志崩溃原子性、重启后持久的 quarantine，以及完整元数据失败矩阵仍未关闭。缺陷与触发方式见 [ext4 孤儿 inode 记录](../current/ext4-orphan-gap.md)。 |
| N18 | V1-DELIVERY | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，smoke 矩阵 | 把适用的 smoke 门禁推进到 `NR_CPUS=1` 之外，同时保留显式的架构/设备跳过与原有完整 target 语义。 | C 内核测试与 Make smoke target；无 Rust owner。 | 增加有代表性的 SMP2 变体，同时运行既有单 CPU target 与 SMP 变体；分别保留结果。 |
| N19 | V1-CORE | [路线图 TODO §P1](../../roadmap/a20os-improvement-todo.md)，trap 诊断 | 注入一个被修复却反复重陷的故障；确认陷态风暴会终止出错任务而其他任务继续，并确保正常的按需分页/COW 不被误判。 | C 用户 trap 与 fault 路径；无 Rust owner。 | 增加确定性的修复/重陷注入，断言风暴记录与任务退出，再断言无关任务进展与干净关机。 |
| N20 | V1-DELIVERY | [路线图 TODO §P2](../../roadmap/a20os-improvement-todo.md)，交付/平台矩阵 | 用显式的宿主编译器变量与正确的活动矩阵，复核保留的 64 位与 ARM32/RISC-V32/ARMv7-M 构建/工具链档位。 | C/Rust 构建系统与架构工具链；需要 build/release owner。 | 按记录的工具链版本运行保留构建矩阵，区分构建证据与运行证据，并单独保留已退役架构的历史。 |

这个队列是人工划定并排序的后续工作。它不取代生成的源码清单、1.0 工作包表、各需求专属的记录，也不取代
剩余的 `needs_triage` 行。

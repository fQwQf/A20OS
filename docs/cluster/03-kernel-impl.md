# 内核实现指南（WA 轨道）

内容已按 2026-10 源码核对。本文告诉实现者**具体怎么改这棵树**，是 03 号规范：01-abi.md 说"是什么"，02-wire-protocol.md 说"线上长什么样"，本文说"内核里怎么落地"。

## 1. 核心集成决定：remote-ep 是"队列被内核线程消费的普通端点"

**不要改 `kernel/ipc/a20_channel.c` 的快路径。** 已核对的该文件结构（行号以现状为准）：

- 入口：`a20_channel_send`(:380) → `ch_try_enqueue`(:225)；`a20_channel_recv_begin`(:394)/`recv_finish`(:612)；`a20_channel_ep_peer_shutdown`(:734)、`a20_channel_ep_release`(:753)。
- 消息对象 `ch_msg_alloc`(:106)，队列在 `a20_channel_ep_t`（`kernel/include/ipc/ipc.h:199`，peer 指针 :203）。

集成方案：远程通信时，`cluster_connect` 创建**一对本机端点**：

```
调用方句柄 ep_A  <--peer-->  ep_B（代理端点，无用户持有者）
                                │ 队列由 cluster tx 工作线程消费
                                ▼
                        帧编码 → 路由 → 传输发送
对端 REPLY 到达 → 直接 ch 消息入队 ep_A 的对侧（与普通对端行为一致）
```

- 发送路径**零修改**：调用方 `channel_send/call` 把消息入 ep_B 的队列，与入任何本机端点完全一样。
- tx 工作线程（每传输实例一个，或 SERVER 档每 lane 一个）阻塞在 ep_B 的 recv 上，取出消息 → 编帧 → 发送。CALL 的 txid 在取出时分配，登记在途事务表。
- 接收方向：传输 RX → 帧解码 → 查在途事务表（txid 命中 → 把载荷作为 REPLY 消息入队到等待中的调用端点）或查导出表（dst_slot 命中 → 入队到被导出的本机端点，来源 txid 记下以便回 REPLY）。
- 关闭/错误：链路 DOWN 或收 CLOSE → 对相关代理端点调 `a20_channel_ep_peer_shutdown()`（现成函数），本地等待者自然被唤醒，语义与本机 peer 关闭逐位一致。
- handle 检查：现有 `ch_check_send_types`(:153) 已在入队时检查类型；代理端点的 chan_type 配置为"拒绝 handle"，跨机 handle 传递就在现成检查点上自然返回错误，再在 ABI 层映射为 `A20_ERR_CLUSTER_UNSUPPORTED`。

这个方案把对 `a20_channel.c` 的改动压缩到**理论上为零**；唯一可能需要的补丁是：CALL 语义（等 REPLY）当前由 ABI 层 `channel_call` 组合 send+recv 实现——确认 `sys_native_ipc.c` 的 channel_call 实现能直接用于代理端点（应该能，因为它只见普通端点）。若不能，允许对 `a20_channel.c` 打**最小补丁**并在本文登记 diff 理由。

## 2. 文件划分（新建 `kernel/cluster/`）

| 文件 | 职责 | 关键约束 |
|---|---|---|
| `frame.c/h` | 帧编解码（逐字段显式，禁结构体直发） | 纯函数，无锁无分配（调用方给缓冲），便于模糊测试 |
| `route.c` | 路由表：node_id → (transport, next_hop, metric) | 读写锁或 RCU，参照仓库现有并发惯例；查表在 RX 热路径，读侧不得分配 |
| `export.c` | 导出表：slot/服务名 → 本机端点引用（持引用计数） | slot 单调不复用 |
| `remote_ep.c` | 代理端点生命周期、tx 工作线程、在途事务表、deadline 定时器 | 见 §3 |
| `reasm.c` | 分片重组缓存（RELIABLE 档） | 限额驱动淘汰；MCU 档编译排除 |
| `reliable.c` | seq/ACK/重传/去重（RELIABLE 档） | MCU 档编译排除 |
| `transport.c` | 传输注册表、a20_clx_transport_t 派发 | 见 04-transports.md |
| `loopback.c` | loopback 传输 + 测试钩子（丢包/延迟/重复） | `CONFIG_CLUSTER_TEST_HOOKS` 控制 |
| `udp.c` | UDP 传输（WA2） | 用内核内部 socket API |
| `uart.c` | UART 传输（WC1，WC 轨道拥有） | 轮询档 |
| `stats.c` | 02-§10 计数器 | 只追加不改名 |

ABI 接线：新文件 `kernel/abi/native/sys_native_cluster.c`，输入校验用 `sys_validate.h` 惯例，句柄/权限用 `handle_table.c` 的 `a20_handle_lookup_*`。

## 3. 并发与内存纪律

- **锁层级**（从先到后，禁止反向）：路由表读锁 → 导出表锁 → 单代理端点锁。传输回调（可能在中断上下文）只允许：无锁入环形缓冲 + 唤醒 RX 线程；**禁止**在中断里查路由表之外的任何锁。MCU 档（UP、可全 polling）可简化，但代码路径必须与 SMP 档同源——用编译档裁剪实现，不是两套逻辑。
- **上下文**：syscall 上下文（connect/export/route）可睡眠；RX/TX 线程上下文可睡眠；中断上下文只做环形缓冲入队。
- **内存**：帧缓冲走专用 slab（SERVER 档预分配池，大小=限额）；MCU 档静态数组（对齐 `kernel/mcu/heap.c` 风格）。重组缓存总量硬上限（01-abi 限额），到达上限淘汰最旧，**永不阻塞发送方**。
- **定时器**：在途 CALL deadline、PING 周期、重组超时统一用 `core/timer.h` 现有设施；禁止新造定时器轮子。
- **NOMMU**：全部代码必须在 `CONFIG_NOMMU` 下编译运行（`kernel/mm/nommu.c` 恒等映射）；用户指针一律经 `sys/usercopy.h`，禁止直接解引用。

## 4. 分档编译（接入既有机制）

- 照搬 `Makefile:135` 的 `NET_PROFILE` 样式加 `CLUSTER_PROFILE ?= 2`（1=MCU 2=DEFAULT 3=SERVER），合法性检查照抄其写法。
- `components/trim.toml` 加 `cluster` 特性位与 `[profile.mcu]` 联动：PROFILE=mcu（`Makefile:290`）自动 `CLUSTER_PROFILE=1`，并从源列表排除 `reasm.c reliable.c udp.c`。
- `frame.c route.c export.c remote_ep.c transport.c` 三档共有——**档差异只允许用 `#if CLUSTER_PROFILE >= 2` 圈代码块，禁止分裂文件**。

## 5. 实现顺序（WA1 内部，严格按序）

1. `frame.c` + 离线金样向量自测（WB1 若未交付，先用 02-wire-protocol.md §9 要求手写最小向量集）。
2. `route.c` + `export.c` + 限额。
3. `transport.c` + `loopback.c`（含测试钩子）。
4. `remote_ep.c`：代理端点 + tx 线程 + 在途事务 + deadline。
5. `sys_native_cluster.c`：set_self/export/connect/link_status 接线（route/event_subscribe 可最后）。
6. 内核自检程序（位置参照现有内核自检惯例）：虚拟节点 A/B echo 往返、超时、CLOSE、handle 拒绝、超限。

每一步完成后必须全架构编译（至少 riscv64 + aarch64 + armv7m/mcu 档）再进下一步——不要攒到最后一次性编译。

## 6. 常见坑（预先声明，踩之前回来读）

- **不要让 syscall 上下文直接等远程应答**——`channel_call` 的等待必须复用现有端点等待机制（对调用方就是普通端点），否则超时/信号/取消语义全部要重造。
- **txid 与 seq 是两个空间**：txid 管 CALL 事务，seq 管链路可靠传输。混用是重构级灾难。
- **RX 路径不得分配大内存**：分片到达先入重组缓存（有上限），不为单帧动态 alloc/free 抖动。
- **peer_shutdown 幂等**：链路 DOWN 时可能对同一代理端点触发多次关闭，确认 `a20_channel_ep_peer_shutdown` 可重入，不能则在代理侧加状态位。
- **本机路径性能回归**：`A20_NODE_ID_LOCAL` 必须走到与改造前**逐指令相同**的路径。验收时跑现有 channel 密集型负载（ufsd 文件操作）对比。
- **时间片捐赠**：代理端点上的 donate 族调用按 01-abi §跨机限制拒绝；不要因为"本机端点支持 donate"就放行——代理端点的对端不在本机调度域。

## 7. 验收对照（WA 轨道完成标准）

- 02-wire-protocol.md §9 金样向量全过（离线，含畸形帧）。
- 06-production.md 中 WA 相关测试矩阵全绿。
- `make check-riscv64-bringup` + 至少另外两个架构（含一个 NOMMU、一个 mcu profile）通过。
- 本机 channel 微基准无统计显著回归（测量方法见 06-production.md §性能）。

## 实现期记录（WA1，2026-10 按源码核对）

WA1 落地 `kernel/cluster/`（frame/route/export/transport/loopback/remote_ep/selftest）
后对本节的实现期决定。冲突仲裁顺序：代码现实 > 子系统规范 > 顶层设计；改任何一条
请先改这里。

1. **对 `a20_channel.c` 的最小补丁（§1 预留条款的正式登记）**：新增两个 API
   （`kernel/ipc/a20_channel.c` 尾部、`kernel/include/ipc/ipc.h` 声明）——
   `a20_channel_ep_peer_ref()`（g_ch_lock 下取对端 + `refcount_inc_not_zero`，
   与内部 send/recv 路径同一模式；connect 需持有代理半、export 需持有服务对端，
   peer 指针离开 g_ch_lock 读是唯一的替代方案，故封成函数）与 channel type 标志
   `A20_CHAN_TYPE_REMOTE`。ABI 层 `a20_clx_map_send_err()`（`sys_native_ipc.c`
   的 send/call 各一处调用）把代理端点上 typed-channel 对 handle 的拒绝从
   `A20_ERR_TYPE_MISMATCH` 映射为 `A20_ERR_CLUSTER_UNSUPPORTED`（§1 指定的映射
   点）。快路径（`ch_try_enqueue`/`recv_begin`/`ep_release`）逐指令未改；diff 理由：
   connect/export 需要跨 pair 引用对端，errno 映射按 §1 指定在 ABI 层。
2. **TX 线程粒度是每代理端点一个、每导出一个**，不是 §1 的"每传输实例一个"：
   `proc_alloc()` 入口无参数（`kernel/proc/proc.c:659`），且 `recv` 只能阻塞在单个
   端点队列上，一个 TX 线程无法同时服务多个代理队列。线程经 spawn 表
   （`kernel/cluster/remote_ep.c` `clx_spawn_queue`）领取任务。SERVER 档的 lane 化
   留待 WA2 随 UDP 一起落地。
3. **deadline 定时器**：§3 要求统一用 `core/timer.h` 现有设施——落地为 250 ms
   周期的 housekeeping 线程（`clx-hk`，`proc_sleep_until` + `timer_get_ticks`，
   与 fb_autoflush 同款模式）扫描在途事务最早 deadline；没有新造定时器轮子。
4. **核心惰性初始化**：`a20_clx_core_init()` 由第一个 cluster syscall（或
   `clxselftest=1` bootarg）触发，注册 loopback/UART 传输并拉起 RX/hk 线程；
   从不触碰 cluster 的内核引导路径零开销（§6"本机路径性能回归"条款的结构性保障）。
5. **RX 上行有界分配**：`a20_clx_rx_frame()` 对每帧 `kmalloc` 一份拷贝入 64 项环形
   缓冲（§3"中断上下文只入环形缓冲"）。v0 的调用方全部在线程上下文（loopback 无
   中断路径；UART 是 POLLING 档），拷贝大小受传输 MTU 上限（loopback 64 KiB，
   03-§6"RX 路径不得分配大内存"在 v0 无重组缓存的前提下成立：分配小且有界、无
   抖动）。WA2 接 UDP 中断路径时需把拷贝换成预分配池（§3 SERVER 档要求），此处
   登记。
6. **自检落点**：`kernel/cluster/selftest.c`，bootarg `clxselftest=1` 门控
   （bootargs 惯例，同 CHTRACE_BOOTARG），`kernel/main.c` 在 `oom_kswapd` 拉起后
   单点挂钩（该树无 initcall 机制；正常引导一次 `strstr` 开销）。覆盖：虚拟节点
   A/B echo 往返、CALL 完成后 deadline 清零（扫描不误杀）、connect(自身 node_id)
   入站 CALL 走导出表、link_status LOCAL 汇总/未知节点 UNREACHABLE、export
   REPLACE 同名替换、REPLACE 后释放旧服务端句柄再远程 CALL 新 slot（旧 worker
   退出竞态回归）、connect(LOCAL) 独立通道往返、handle 拒绝、在途超限、CLOSE
   空载荷编码、CLOSE RX 两分支（export 清 pending / proxy 拆解）、远程 CLOSE 拆解、
   deadline 扫描拆解、限额 EXISTS/NO_SPACE。
7. **分档接线**：`Makefile` 的 `-DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE)` 与
   `components/trim.toml` cluster 特性位按 04-§4 note 5/11 的口径归 WA3；当前
   `kernel/cluster/uart.h:49-55` 自行推导档（`CONFIG_MCU`→1，否则 2），本目录全部
   代码用 `CLX_LIMIT_*` 宏随档取值，不需要新编译开关即可在三档下编译。
8. **worker 收发的两阶段取消息**：worker 用公开的 `recv_begin`/`recv_abort`/
   `recv_finish` 序列在锁外分配缓冲（core 持有的端点无其他消费者，重放取头
   安全）；未对 channel 增加任何新接口。
9. **引用纪律（实现期发现并修复）**：`a20_channel_create` 返回时两半各带一个
   "第二句柄"初始引用；proxy/export 路径从不安装该句柄，`peer_ref` 之后必须立即
   释放一次孤立初始引用，否则每个 connect/export 永久泄漏一个端点（已修，
   `remote_ep.c`/`export.c` 注释详述）。
10. **CALL 完成后重算 proxy deadline**（WA1 终审 F2）：`CALL_REPLY` 完成事务后按
    剩余在途项重算 proxy 的最早 deadline 缓存（无在途为 0，`clx_proxy_deadline_recalc`）。
    此前旧 deadline 残留在缓存里，`deadline_scan` 在首次 CALL 后 30 s 无条件下线
    每个 proxy。自检用例"CALL 完成后经过 deadline 扫描仍存活"钉住该回归。
11. **connect(LOCAL) 独立解析通道**（WA1 终审 F6）：`a20_clx_export_connect_local()`
    （export.c）替代已删除的 `a20_clx_export_peer_for_connect()`——建 fresh pair，
    连接半以 NONBLOCK 注入一条零长度、携带一个 `A20_OBJ_CHANNEL_ENDPOINT` 句柄的
    连接请求（服务队列满立即失败，不在建链处阻塞），不再复用导出 peer。服务端
    约定与失败语义见 01-abi 落地状态 #10；本机快路径指令序列不变（该路径本就不
    经过集群代码）。
12. **`A20_EXPORT_REPLACE` 落地**（WA1 终审 F5）：`a20_clx_export_register()` 在
    同名冲突且带 REPLACE 时按 revoke 语义当场退役旧导出（旧 slot 推
    `EXPORT_DROPPED`），再为新导出分配新 slot；退役先行保证"表满 + 同名替换"
    也必然成功。行为登记见 01-abi 落地状态 #14。
13. **导出回复 worker 绑定注册身份**（WA1 F5 后续竞态修复）：REPLACE 退役旧导出
    后其回复 worker 可能仍阻塞在旧 peer 的 recv 上；表项随即被新导出复用时，
    旧 worker 退出路径曾无条件清 `ex->peer` 并释放——把新导出唯一的自有引用
    清掉，新导出沦为 in_use/peer==NULL 僵尸（远程 CALL 得 REMOTE_CLOSED）。
    修复：`a20_clx_export_spawn_worker()` 在 `g_clx_lock` 下为 worker 取该注册
    peer 的**第二个引用**作为绑定身份（经 spawn 表 `payload2` 传入，绑定在
    spawn 时完成，避开"worker 首条指令晚于 REPLACE"的错绑窗口）；
    `clx_export_tx_main()` 只在 `ex->peer == 绑定 peer` 时视自己存活、配对待发
    FIFO 与清除/释放表项引用，退出时另释放自身引用。引用被 worker 全程持有，
    故指针值不会被回收复用，裸指针比较即可靠的所有权判定。自检用例"REPLACE
    survives old server-handle release"（1d2）钉住该回归：REPLACE 后才释放旧
    服务端句柄，再对新 slot 发远程 CALL，断言收到正常 REPLY。顺带登记：
    `clx_spawn_queue()` 在 `proc_alloc` 失败时回滚 spawn 槽，使 -1 严格表示
    "未入队"，调用方据此安全回收随槽传递的引用。

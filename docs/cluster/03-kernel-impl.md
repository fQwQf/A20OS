# 内核数据面实现说明（WA 轨道）

> **状态横幅（2026-10-07 核对）**：本文描述的 `kernel/cluster/` 数据面**已实现并在本仓库**：六个 cluster syscall 全部接线完成，loopback/UART 两种传输在位，内核自检 24/24 通过（实测，见 §7）。**未实现**：UDP 传输（WA2）、分片重组与可靠传输（WA3）、用户态 clusterd/jobd（WD）、生产硬化（WE）——见 §9，不要按本文的任何路线图段落假设它们可用。

本文是 03 号规范：01-abi.md 说"是什么"，02-wire-protocol.md 说"线上长什么样"，本文说"内核里是怎么落地的、现在怎么用"。行号按 2026-10-07 的源码核对。

## 1. 架构：remote-ep 代理端点 = "队列被内核 worker 消费的普通端点"

`kernel/ipc/a20_channel.c` 的快路径**没有被 cluster 改动**。该文件的现有结构：

- 入口：`a20_channel_send`(kernel/ipc/a20_channel.c:380) → `ch_try_enqueue`(:225)；`a20_channel_recv_begin`(:394) / `a20_channel_recv_abort`(:481) / `a20_channel_recv_finish`(:612)；`a20_channel_ep_peer_shutdown`(:734)、`a20_channel_ep_peer_ref`(:764)、`a20_channel_ep_release`(:778)。
- 消息对象 `ch_msg_alloc`(:106)；队列挂在 `struct a20_channel_ep` 上（`msg_head/msg_tail`，kernel/include/ipc/ipc.h:226-227；peer 指针 :221）。

远程通信时，`cluster_connect` 创建**一对本机端点**（`a20_clx_proxy_create`，kernel/cluster/remote_ep.c:461）：

```
调用方句柄 ep_A  <--peer-->  ep_B（代理端点，无用户持有者）
                                │ 队列由 cluster TX worker 消费
                                ▼
                        帧编码 → 路由 → 传输发送
对端 REPLY 到达 → 经代理半 a20_channel_send 入队 → 唤醒停在 ep_A 上的调用方
```

四条路径的现状：

- **发送零修改**：调用方 `channel_send/call` 把消息入 ep_B 的队列，与入任何本机端点完全一样。TX worker（`clx_proxy_tx_main`，kernel/cluster/remote_ep.c:597）阻塞在 ep_B 的 recv 上，取出消息 → 编帧 → 登记在途事务（txid 在取出时分配，remote_ep.c:651）→ `a20_clx_core_send_frame` 发送。
- **接收方向**：传输上行 → `a20_clx_rx_frame`（kernel/cluster/transport.c:197）入 64 项环形缓冲 → RX 线程 `clx_dispatch`（transport.c:248）解码后分发：CALL_REPLY/ERROR 走 `a20_clx_proxy_rx`（remote_ep.c:327，按 (src_hash, txid) 查在途事务，命中则把载荷作为普通 channel 消息 `a20_channel_send` 进代理半，落到调用方队列）；CALL 走 `a20_clx_export_rx`（kernel/cluster/export.c:316，按 dst_slot 查导出表，注入被导出端点的对端）。
- **关闭/错误**：链路 DOWN（`a20_clx_node_down`，remote_ep.c:402）、远程 CLOSE、ERROR 帧、deadline 超期，全部殊途同归到 `a20_clx_proxy_shutdown`（remote_ep.c:286）——对代理对的**两个半**各调一次现成的 `a20_channel_ep_peer_shutdown()`，本地等待者以 peer-closed 唤醒（syscall 层表现为 `A20_ERR_CANCELED`），语义与本机对端关闭逐位一致。`peer_shutdown` 可重入（remote_ep.c:21-24 头注释登记，自检 3c/4 覆盖）。
- **handle 拒绝**：代理端点的 chan_type 是 `clx_proxy_chan_type`（remote_ep.c:451），`send_handle_types = 0`，跨机 handle 在现成的 `ch_check_send_types`(kernel/ipc/a20_channel.c:153) 检查点上被拒；ABI 层 `a20_clx_map_send_err`（kernel/abi/native/sys_native_cluster.c:78，在 sys_native_ipc.c:375 的 send 与 :649 的 call 各挂一处）凭 `A20_CHAN_TYPE_REMOTE` 标志（kernel/include/ipc/ipc.h:172）把 `A20_ERR_TYPE_MISMATCH` 映射为 `A20_ERR_CLUSTER_UNSUPPORTED`。

**为什么这样设计**：远程等待的全部语义（阻塞、超时唤醒、peer-closed、取消）复用本机端点机制，cluster 不重造任何一套；`channel_call` 是 ABI 层 send+recv 的组合，它只见普通端点，所以对代理端点天然成立——§1 原预留的"确认 channel_call 可用于代理端点"条款**已验证成立**，未对快路径打任何补丁。

**对 `a20_channel.c` 的全部实际改动**（实现期记录 #1 的正式登记，快路径逐指令未动）：

1. 新增 `a20_channel_ep_peer_ref()`（kernel/ipc/a20_channel.c:764，声明 kernel/include/ipc/ipc.h:372）：`g_ch_lock` 下取对端 + `refcount_inc_not_zero`。connect 要持有代理半、export 要持有服务对端，peer 指针离开 `g_ch_lock` 读是唯一替代方案，故封成函数。
2. 新增 channel type 标志 `A20_CHAN_TYPE_REMOTE`（kernel/include/ipc/ipc.h:172），只被 remote_ep.c 设置，供 ABI 层做上述 errno 映射。

## 2. 文件职责（kernel/cluster/ 现状）

| 文件 | 职责 | 关键约束（现状） |
|---|---|---|
| `frame.c/h` | 帧编解码（kernel/cluster/frame.c:70 编码 / :122 解码），逐字段显式小端，禁结构体直发 | 纯函数、无锁无分配（调用方给缓冲）；判定顺序与 `tools/cluster-ref/refdec.c` 逐步对齐，verdict 字符串就是 clframe.py 的常量（kernel/cluster/frame.c:50），供离线对拍（§7.3） |
| `route.c` | 路由表：node_id → (transport, next_hop, metric)，精确匹配、每目的单条最优 | 固定数组（route.c:29）+ `g_clx_lock`，查表不分配（route.c:174）；本机节点恒解析到 loopback（route.c:183-190）；loopback 路由顺带注册虚拟节点（route.c:141） |
| `export.c` | 导出表：slot/服务名 → 服务端点的 peer 引用；入站 CALL 注入、pending FIFO、dedup 窗口 | slot 单调不复用、0 保留（export.c:25）；REPLACE 语义 export.c:76-94；dedup 窗口 64 项（export.c:189，clx_internal.h:52） |
| `remote_ep.c` | 代理端点生命周期、TX worker（每代理一个 + 每导出一个）、在途事务表、deadline 扫描 | 见 §3 |
| `transport.c/h` | 传输注册表（transport.c:126）、TX 派发（:162）、RX 环形缓冲与 RX/hk 线程（:315/:330）、每链路计数器（:49-60）、`link_status` 数据源（:417）、事件订阅（:520/:558） | 02-§10 计数器就在这里——**没有独立 stats.c 文件**（旧版本文档的计划，落地时并入本文件） |
| `loopback.c/h` | loopback 传输：send = 把帧拷贝投递给目标虚拟节点的 `a20_clx_rx_frame`（loopback.c:248/:268），零丢包保序；虚拟节点注册表；测试钩子 | 三档都有；链路地址 = 4B 虚拟节点号；钩子见 §8.2 |
| `uart.c/h` | UART 传输头节点侧（transport_id=2）：SLIP 变体成帧、MTU 256（kernel/cluster/uart.h:165）、50 ms 字节间静默重同步、2B 短地址、HELLO/PING 状态机 | WC1 轨道拥有（impl-prompts.md 文件所有权表）；DEFAULT 档经 transport.c:392-405 的适配层注册；MCU 档该文件只剩编码头 |
| `selftest.c` | 内核自检（24 项），bootarg `clxselftest=1` 门控 | 跑法见 §7.2 |
| `clx_internal.h` | 目录内部共享声明；外部禁止包含 | `CLX_LIMIT_*` 分档限额宏（clx_internal.h:34-49）也在这里 |

ABI 接线在 `kernel/abi/native/sys_native_cluster.c`：六个入口 set_self(:89)/export(:115)/connect(:166)/route(:256)/event_subscribe(:298)/link_status(:334)，输入校验沿用 `sys_validate.h` 惯例（`A20_VALIDATE_AND_COPY`），句柄用 `a20_handle_lookup_ref_internal`（export 处 sys_native_cluster.c:143）。set_self/export/route 需要管理权限——本树没有任务级 rights 位图，落地为 euid==0（`clx_admin_ok`，sys_native_cluster.c:64；登记见 01-abi 落地状态）；connect/event_subscribe/link_status 无权限要求。syscall 号 0x0520-0x0525（kernel/include/abi/native/syscall_nr.h:103-108）。

**计划中的文件（未实现，见 §9）**：`udp.c`（WA2）、`reasm.c`/`reliable.c`（WA3）。

## 3. 并发与内存纪律（实现成什么样）

- **锁层级**（clx_internal.h:6-10 头注释即现行规约）：`g_clx_lock`（route.c:18 定义，自/路由/导出/代理表共一把核心锁）→ channel 锁（只经公开 `a20_channel_*` 调用进入）；RX 环形缓冲有独立锁 `g_clx_rx_lock`（transport.c:43），层级在 `g_clx_lock` 之下；每链路计数器有 `g_clx_links_lock`（transport.c:60）；虚拟节点表与 worker spawn 表各有小锁（loopback.c:31、remote_ep.c:70）。v0 的传输回调全部在线程上下文（loopback 无中断路径，UART 是 POLLING 档），规约为未来中断路径保留"只入环形缓冲、不拿 `g_clx_lock`"的纪律（transport.c:9-11、transport.h:51-52）。
- **线程清单**（全部由 `a20_clx_core_init` 惰性拉起，见 §5）：`clx-rx`（transport.c:315，1 ms 轮询环形缓冲）、`clx-hk`（transport.c:330，250 ms 周期扫 deadline）、每代理一个 `clx-txp`（remote_ep.c:599）、每导出一个 `clx-txe`（remote_ep.c:689）。worker 粒度是"每端点一个"而不是"每传输实例一个"：`proc_alloc()` 入口无参数（kernel/proc/proc.c:659），worker 经 spawn 表领任务（remote_ep.c:60-101），且 recv 只能阻塞在单个端点队列上（实现期记录 #2）。SERVER 档 lane 化是 WA2 的计划，未实现。
- **上下文**：syscall 上下文（connect/export/route）可睡眠；RX/TX worker 上下文可睡眠；锁内不睡眠（RX 分发处理器先取锁查表、放锁后再做可能阻塞的 channel send，如 remote_ep.c:373-378、export.c:369-381）。
- **内存**：RX 上行每帧 `kmalloc` 一份拷贝入环形缓冲（transport.c:207），大小受传输 MTU 上限（loopback 64 KiB、UART 256 B），满则丢并计 rx_drops（transport.c:216-220）；worker 收发用"两阶段取消息"——`recv_begin` 报长度 → 锁外 `kmalloc` → 重放 `recv_begin`/`recv_finish` 消费（remote_ep.c:545-583，core 持有的端点无其他消费者，重放安全，未给 channel 加新接口）。**没有重组缓存**（WA3 未实现），故"重组上限淘汰"整条不适用于 v0；超限分配失败一律退化为丢帧/延迟重试，**发送方永不阻塞在内存上**。
- **定时器**：没有新造定时器轮子。CALL deadline（30 s 链路常数，clx_internal.h:62）以 tick 记在在途事务上，由 `clx-hk` 250 ms 周期扫描（`a20_clx_deadline_scan`，remote_ep.c:425）；每代理缓存最早 deadline 并在 CALL 完成时重算（`clx_proxy_deadline_recalc`，remote_ep.c:243，实现期记录 #10）。
- **NOMMU/用户指针**：ABI 层用户指针一律经 `sys/usercopy.h`（`A20_VALIDATE_AND_COPY` / `copy_from_user` / `a20_copy_struct_to_user`，sys_native_cluster.c:96/:149/:357），无直接解引用。kernel/cluster/ 本身不碰用户指针。

## 4. 分档编译（现状）

档由 `CONFIG_CLUSTER_PROFILE` 决定，三档取值在 `kernel/cluster/uart.h:49-60` **自行推导**：定义了 `CONFIG_MCU` 就是 1（MCU），否则 2（DEFAULT）；显式 `-DCONFIG_CLUSTER_PROFILE=` 总是赢（kernel/cluster/uart.h:40-46 注释）。SERVER=3 的宏档位在 clx_internal.h:34-49 已留好，但目前没有任何构建路径把它设为 3。

- **档差异的落地方式**：同一批源文件 + `#if CONFIG_CLUSTER_PROFILE >= ...` 圈代码块，没有按档分裂文件。限额经 `CLX_LIMIT_*` 宏（clx_internal.h:34-49）取 `kernel/include/abi/native/resource.h:58-78` 的分档值：路由 4/256/4096、slot 4/64/256、代理端点 2/128/1024、在途 1/64/512（MCU/DEFAULT/SERVER）。
- **MCU 档的实际形态**：`PROFILE=mcu`（Makefile:290 判定）整表替换源清单为 `components/trim.mk:8` 的 `TRIM_PROFILE_MCU_SOURCES`——`kernel/cluster/*.c` **不在其中**，MCU 构建里 cluster 核心根本不存在；叶子侧协议面在 `kernel/mcu/leaf.c`（HELLO 被动应答/PING→PONG/CALL→CALL_REPLY 单在飞/ERROR/CLOSE，512 B RX / 320 B TX 静态缓冲，kernel/mcu/leaf.h:15），与头节点共享 `kernel/cluster/uart.h` 编码头，两端不会漂移。
- **未落地的原计划（归 WA3，见实现期记录 #7）**：Makefile 加 `CLUSTER_PROFILE ?= 2`（NET_PROFILE 样式，Makefile:135 是参照物）与 `-DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE)`、`components/trim.toml` 加 `cluster` 特性位。当前 trim.toml 没有 cluster 特性位，不需要也不能开。

## 5. 核心惰性初始化

`a20_clx_core_init()`（transport.c:372）幂等，由第一个 cluster syscall（set_self/connect/route/event_subscribe/link_status 入口均调，见 sys_native_cluster.c:107/:183/:282/:311/:347）或 `clxselftest=1` bootarg 触发。它注册 loopback（transport.c:385-390，MTU 65536），DEFAULT 档再注册 UART 适配层（transport.c:392-405），然后拉起 `clx-rx` 与 `clx-hk`。从不触碰 cluster 的内核引导路径零开销——这是"本机路径性能回归"条款的结构性保障：cluster 代码在本机 channel 路径上的指令数是零（实现期记录 #4）。

## 6. 实现是按这个顺序组织的（原"实现顺序"的说明式转写）

代码的依赖分层决定了各部分落地的先后，这个顺序现在仍反映在各文件只依赖下层的事实里：

1. `frame.c` 最先：不依赖任何 cluster 内部设施，且离线金样对拍（§7.3）只要求它存在。
2. `route.c` + `export.c`：表结构 + 限额，依赖 frame 的类型常量。
3. `transport.c` + `loopback.c`：把路由/导出接到真实收发路径上；loopback 提供第一个可跑的传输与故障注入钩子。
4. `remote_ep.c`：代理端点 + TX worker + 在途事务 + deadline，是 CALL 语义的主体，依赖 1-3 全部。
5. `sys_native_cluster.c`：六个 syscall 的 ABI 接线，依赖 1-4。
6. `selftest.c`：最后，用前 5 步的内部 API 驱动全路径回归（§7.2）。

每一步落地时都做了全架构编译（riscv64 为主验证对象），没有攒到最后一次性编译——这个纪律使得 WA1 终审的 F1-F6 缺陷都能定位到单文件（见实现期记录 #10-#13）。

## 7. 验证：怎么跑自检、怎么对拍、怎么编译门禁

### 7.1 编译门禁

`make check-riscv64-bringup`（定义 tools/targets-build.mk:195）即 `make ARCH=riscv64 ABI=both BRINGUP=1 kernel-only`，全量编译含 `kernel/cluster/` 的内核。2026-10-07 本机实测通过（因仓库路径含空格触发 user/ 子 make 的 `-isystem` 分词问题，构建经 user-namespace bind mount 到无空格路径执行；这是本机环境限制，与 cluster 代码无关）。其余架构/档位的编译矩阵归 WE 轨道（未落地，§9）。

### 7.2 内核自检（clxselftest=1）

自检主体是 `kernel/cluster/selftest.c` 的检查线程（selftest.c:203），由 bootarg 门控的 `a20_clx_selftest_boot()`（selftest.c:688）拉起；挂钩点在 `kernel/main.c:215-223`（`oom_kswapd` 拉起之后，本树无 initcall 机制，正常引导代价是一次 `strstr`）。

**一个必须知道的引导分支事实**：`kernel/main.c:201-203` 的 `#if defined(BRINGUP) && !defined(CONFIG_RAMFS_USER)` 分支在纯 BRINGUP 引导下直接进 `bringup_smoke_test()`，**走不到** selftest 挂钩。所以自检要在普通（dev）引导、或 `BRINGUP=1 RAMFS_USER=1`（RAMFS_USER 在 Makefile:119/1134，内嵌最小用户态）下跑。实测命令（2026-10-07，本机 QEMU + TCG，24/24 通过）：

```bash
# 构建（riscv64，bringup + 内嵌用户态；产物不带磁盘依赖）
make ARCH=riscv64 BRINGUP=1 RAMFS_USER=1 kernel-only

# 跑自检：-append 把 clxselftest=1 写进 DTB /chosen/bootargs
qemu-system-riscv64 -machine virt -bios default \
  -global virtio-mmio.force-legacy=false -m 1G -nographic -smp 1 \
  -append clxselftest=1 \
  -kernel .kernel-build/riscv64-qemu-virt-riscv64-both-bringup-ramfs-user/kernel.elf
```

同一条命令行也可以让 make 推导（`tools/run-targets.mk:122` 的 `_qemu_argv`，`EXTRA_QEMU` 于 Makefile:848-850 并入）：

```bash
make ARCH=riscv64 BRINGUP=1 RAMFS_USER=1 EXTRA_QEMU="-append clxselftest=1" _qemu_argv
```

riscv64 上 `-append` 经 DTB `/chosen/bootargs` 由 `kernel/arch/riscv64/platform/fdt.c` 读出（实测引导日志 `[FDT] bootargs='clxselftest=1'`）。注意 x86_64 的 `-kernel` 引导路径拿不到 `-append`（Makefile:966-972 注释），自检请用 riscv64 或 aarch64。guest 跑完自检后停在 shell，用 `timeout`/手动终止 QEMU 即可，退出码 124 是预期的。

实测输出摘录（每条用例一行 `[CLX-TEST] <名>: PASS`，汇总行在末尾）：

```
[CLX] core ready transports=2
[CLX-TEST] set_self: PASS
...
[CLX-TEST] deadline scan teardown: PASS
[CLX-TEST] clx-selftest: 24/24 checks passed
```

24 项覆盖（selftest.c:1-44 头注释是全量清单）：虚拟节点 A/B echo 往返、CALL 完成后 deadline 清零且扫描不误杀、connect（自身 node_id）入站 CALL 只走导出表、link_status LOCAL 汇总/路由节点 UP/未知节点 UNREACHABLE、export REPLACE 同名替换、REPLACE 后释放旧服务端句柄再远程 CALL 新 slot（旧 worker 退出竞态回归，用例 1d2）、connect(LOCAL) 独立通道往返、handle 拒绝、在途超限 NO_SPACE、CLOSE 空载荷编码、CLOSE RX 两分支（export 清 pending / proxy 拆解）、远程 CLOSE 拆解、deadline 扫描拆解、限额 EXISTS。

### 7.3 金样对拍（离线）

帧编解码的离线验收在 `tools/cluster-ref/`（WB1 交付，不依赖内核代码）。`kernel/cluster/frame.c` 的判定顺序与 verdict 字符串按帧头注释（kernel/cluster/frame.c:1-19）与 `refdec.c`/clframe.py 逐步对齐，`a20_frame_verdict_str`（kernel/cluster/frame.c:50）返回的就是 clframe.py 的常量字符串。实测命令（2026-10-07 本机跑过，均绿）：

```bash
python3 tools/cluster-ref/selftest.py        # 编码→解码往返 + 向量自洽，503 checks
python3 tools/cluster-ref/check_c_side.py    # refdec.c 过全部金样，1889 checks（91 帧 + 16 SLIP 向量）
```

`check_c_side.py --decoder <bin>` 可对拍任何按 README.md 输出格式实现的解码器（内核/叶子解码器的主机 shim 走这个入口）；当前仓库内 shipped 的 C 解码器是 `refdec.c`，内核 `frame.c` 靠"同判定顺序 + 同字符串 + 同一组假设登记（A-01..A-16）"保持锁步。向量清单在 `tools/cluster-ref/vectors/MANIFEST.json` 与 `INDEX.md`。

### 7.4 传输层联调环境

双 QEMU 实例 L2 互联（WA2 的验收环境，现已可用于链路层验证）：`tools/cluster-net-up.sh` 一键起 `instances/qemu-riscv64-cluster-{a,b}.toml` 两实例并互 ping，拓扑与实测记录见 [02-udp-demo.md](02-udp-demo.md)。跨机 cluster 流量本身要等 WA2 的 `kernel/cluster/udp.c`（未实现）。

## 8. 使用要点与已规避的坑

### 8.1 给调用方的使用要点（现在就能用）

- **调 syscall**：六个入口语义按 01-abi.md；内核侧行为差异集中在 `sys_a20_cluster_connect`（sys_native_cluster.c:166）：`node_id` 全零（`A20_NODE_ID_LOCAL`）走本机解析（:207-221，按名或 slot 调 `a20_clx_export_connect_local`，拿到的是全新 channel 对的调用半，服务端收到一条零长度、携带一个 `A20_OBJ_CHANNEL_ENDPOINT` 句柄的连接请求）；非本机节点只按 slot 解析（:222-241），按名连远程返回 `A20_ERR_CLUSTER_UNSUPPORTED`（名字解析是 clusterd 的活，WD 未实现）；`A20_CONNECT_RELIABLE` 一律拒绝为 `A20_ERR_CLUSTER_UNSUPPORTED`（remote_ep.c:467-472，v0 无可靠传输）。
- **给服务端**：被导出端点收到两类消息——远程 CALL 注入（普通数据消息，handle_count==0）与本机 connect 请求（零长度、带一个端点句柄）；靠 `handle_count > 0` 区分（export.c:232-253 注释）。typed channel 若不接受端点句柄，本机 connect 在 send 处失败（-A20_ERR_TYPE_MISMATCH）。
- **读本机节点身份**：`link_status` 对全零 node_id 汇总所有链路（transport.c:439-465）；对未 set_self 的内核，一切远程操作返回 `A20_ERR_NODE_UNREACHABLE`。

### 8.2 故障注入（测试钩子）

loopback 钩子按 `CONFIG_CLUSTER_TEST_HOOKS` 编译（loopback.c:117，默认关；本树 Makefile 无专用开关，用 `EXTRA_CFLAGS=-DCONFIG_CLUSTER_TEST_HOOKS=1` 打开，EXTRA_CFLAGS 见 Makefile:973-974），运行期经 bootarg 注入（loopback.c:139-183）：

```
clxhooks=drop=10,delay=5,dup=1,corrupt=1,part=2.3
```

五种注入：随机丢帧（drop_pct 0-100）、延迟（ms）、重发、翻转一个载荷比特（走 CRC 拒绝路径）、虚拟节点对分区。判定用带种子的 LCG，每次引导确定（loopback.c:132-137）。无钩子构建下这些代码路径不存在。

### 8.3 已踩过/已规避的坑（原"常见坑"的实现后记账）

这些是 WA1 实现与终审（F1-F6）期间真实遇到并处理掉的坑，留存给 WA2/WA3：

- **syscall 上下文不直接等远程应答**——`channel_call` 的等待复用现有端点等待机制（调用方看到的是普通端点），超时/唤醒/取消语义零重造。落地后证实这是全案最省的决定（§1）。
- **txid 与 seq 是两个空间**：txid 管 CALL 事务（remote_ep.c:54 的 `g_clx_txid_next`），seq 留给 WA3 的链路可靠传输。v0 编码器对非 RELIABLE 帧强制 seq==0（kernel/cluster/frame.c:205-206），混用在解码层直接判 `seq_on_unreliable_frame`。
- **RX 路径不分配大内存**：v0 无重组缓存，上行每帧一次有界 `kmalloc`（MTU 上限），环形缓冲满即丢并计数；WA2 接 UDP 中断路径时要把拷贝换成预分配池（实现期记录 #5 登记）。
- **peer_shutdown 幂等**：链路 DOWN、远程 CLOSE、ERROR、deadline 可并发指向同一代理端点；`a20_clx_proxy_shutdown` 先在锁内置 `in_use=0`（remote_ep.c:299）再双侧 shutdown，重入安全。自检 3c/4/5 覆盖。
- **本机路径零回归靠结构保证**：`A20_NODE_ID_LOCAL` 的 connect 走 export.c 的独立解析（§8.1），本机 channel 快路径指令序列与改造前逐指令相同（cluster 代码在该路径指令数为零）。06-production.md §性能 的微基准实测归 WE，未跑（§9）。
- **时间片捐赠**：代理端点的对端不在本机调度域，donate 族在代理端点上不适用；跨机限制按 01-abi 拒绝，不因"本机端点支持 donate"放行。
- **BRINGUP 分支陷阱（本次核对发现，见实现期记录 #14）**：纯 `BRINGUP=1` 引导走 kernel/main.c:201 分支，selftest 挂钩不执行——跑自检要么 dev 引导，要么 `BRINGUP=1 RAMFS_USER=1`。

## 9. 未实现清单与路线图指针

以下各项**现在不可用**，相关描述仅作设计意图保留；轨道定义见 [impl-prompts.md](impl-prompts.md)：

| 项 | 状态 | 轨道 |
|---|---|---|
| `kernel/cluster/udp.c` UDP 传输、跨机 UDP `channel_call`、clusterd 骨架 | 未实现（文件不存在） | WA2（impl-prompts.md:124） |
| `reasm.c` 分片重组、`reliable.c` seq/ACK/重传/去重；SERVER 档 lane 化；`CLUSTER_PROFILE`/trim.toml 特性位接线 | 未实现 | WA3（impl-prompts.md:144） |
| 用户态 clusterd（SWIM、`service://` 名字解析）、jobd、tree-reduce、demo-wordcount | 未实现（user/svc/ 下无这些文件） | WD1/WD2（impl-prompts.md:188/:211） |
| check-cluster-* CI 门禁、故障注入矩阵、模糊、soak、性能实测（含 §8.3 的本机 channel 微基准） | 未落地 | WE1/WE2（impl-prompts.md:228/:248） |
| RX 上行改预分配池（替代逐帧 kmalloc）、timer-queue 版延迟钩子 | 计划（随 WA2/WA3） | 实现期记录 #5、loopback.c:223-225 |

## 10. 验收现状对照

- 02-wire-protocol.md §9 金样向量离线全过——**已跑**（§7.3，2026-10-07：selftest.py 503 checks、check_c_side.py 1889 checks，含畸形帧）。
- 内核自检 24/24——**已跑**（§7.2，2026-10-07，QEMU riscv64）。
- `make check-riscv64-bringup`——**已跑**（§7.1，2026-10-07 通过）。其余架构（含 NOMMU、mcu profile）的编译矩阵：未跑，归 WE。
- 06-production.md 测试矩阵与本机 channel 微基准无回归：未跑，归 WE（未落地，§9）。

## 实现期记录（WA1，2026-10 按源码核对；2026-10-07 复审）

WA1 落地 `kernel/cluster/`（frame/route/export/transport/loopback/remote_ep/selftest）
后对本节的实现期决定。冲突仲裁顺序：代码现实 > 子系统规范 > 顶层设计；改任何一条
请先改这里。2026-10-07 复审：全部 13 条与现状一致；§1/§2 引用的 `a20_channel.c`/`ipc.h`
行号已按当日源码刷新（peer 指针 ipc.h:221、`a20_channel_ep_release` :778 等），
新增 #14。

1. **对 `a20_channel.c` 的最小补丁（§1 预留条款的正式登记）**：新增两个 API
   （`kernel/ipc/a20_channel.c` 尾部、`kernel/include/ipc/ipc.h` 声明）——
   `a20_channel_ep_peer_ref()`（:764；g_ch_lock 下取对端 + `refcount_inc_not_zero`，
   与内部 send/recv 路径同一模式；connect 需持有代理半、export 需持有服务对端，
   peer 指针离开 g_ch_lock 读是唯一的替代方案，故封成函数）与 channel type 标志
   `A20_CHAN_TYPE_REMOTE`（ipc.h:172）。ABI 层 `a20_clx_map_send_err()`
   （sys_native_cluster.c:78；sys_native_ipc.c:375 send、:649 call 各一处调用）
   把代理端点上 typed-channel 对 handle 的拒绝从 `A20_ERR_TYPE_MISMATCH` 映射为
   `A20_ERR_CLUSTER_UNSUPPORTED`（§1 指定的映射点）。快路径（`ch_try_enqueue`/
   `recv_begin`/`ep_release`）逐指令未改；diff 理由：connect/export 需要跨 pair
   引用对端，errno 映射按 §1 指定在 ABI 层。
2. **TX 线程粒度是每代理端点一个、每导出一个**，不是 §1 的"每传输实例一个"：
   `proc_alloc()` 入口无参数（`kernel/proc/proc.c:659`），且 `recv` 只能阻塞在单个
   端点队列上，一个 TX 线程无法同时服务多个代理队列。线程经 spawn 表
   （`kernel/cluster/remote_ep.c` `clx_spawn_queue`，:103）领取任务。SERVER 档的
   lane 化留待 WA2 随 UDP 一起落地（未实现，§9）。
3. **deadline 定时器**：§3 要求统一用 `core/timer.h` 现有设施——落地为 250 ms
   周期的 housekeeping 线程（`clx-hk`，transport.c:330；`proc_sleep_until` +
   `timer_get_ticks`，与 fb_autoflush 同款模式）扫描在途事务最早 deadline；
   没有新造定时器轮子。
4. **核心惰性初始化**：`a20_clx_core_init()`（transport.c:372）由第一个 cluster
   syscall（或 `clxselftest=1` bootarg）触发，注册 loopback/UART 传输并拉起
   RX/hk 线程；从不触碰 cluster 的内核引导路径零开销（§8.3"本机路径零回归"
   条款的结构性保障）。
5. **RX 上行有界分配**：`a20_clx_rx_frame()`（transport.c:197）对每帧 `kmalloc`
   一份拷贝入 64 项环形缓冲。v0 的调用方全部在线程上下文（loopback 无中断
   路径；UART 是 POLLING 档），拷贝大小受传输 MTU 上限（loopback 64 KiB）。
   WA2 接 UDP 中断路径时需把拷贝换成预分配池，此处登记（§9 未实现清单）。
6. **自检落点**：`kernel/cluster/selftest.c`，bootarg `clxselftest=1` 门控
   （bootargs 惯例，同 CHTRACE_BOOTARG），`kernel/main.c:215-223` 在
   `oom_kswapd` 拉起后单点挂钩（该树无 initcall 机制；正常引导一次 `strstr`
   开销）。覆盖清单见 §7.2（与 selftest.c:1-44 头注释一致）。
7. **分档接线**：`Makefile` 的 `-DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE)` 与
   `components/trim.toml` cluster 特性位按 04-§4 note 5/11 的口径归 WA3
   （未实现，§9）；当前 `kernel/cluster/uart.h:49-55` 自行推导档
   （`CONFIG_MCU`→1，否则 2），本目录全部代码用 `CLX_LIMIT_*` 宏随档取值，
   不需要新编译开关即可在各档下编译。
8. **worker 收发的两阶段取消息**：worker 用公开的 `recv_begin`/`recv_abort`/
   `recv_finish` 序列在锁外分配缓冲（remote_ep.c:545-583；core 持有的端点无
   其他消费者，重放取头安全）；未对 channel 增加任何新接口。
9. **引用纪律（实现期发现并修复）**：`a20_channel_create` 返回时两半各带一个
   "第二句柄"初始引用；proxy/export 路径从不安装该句柄，`peer_ref` 之后必须立即
   释放一次孤立初始引用，否则每个 connect/export 永久泄漏一个端点（已修，
   `remote_ep.c:485-489`/`export.c:55-60` 注释详述）。
10. **CALL 完成后重算 proxy deadline**（WA1 终审 F2）：`CALL_REPLY` 完成事务后按
    剩余在途项重算 proxy 的最早 deadline 缓存（无在途为 0，
    `clx_proxy_deadline_recalc`，remote_ep.c:243）。此前旧 deadline 残留在缓存
    里，`deadline_scan` 在首次 CALL 后 30 s 无条件下线每个 proxy。自检用例
    "CALL 完成后经过 deadline 扫描仍存活"钉住该回归。
11. **connect(LOCAL) 独立解析通道**（WA1 终审 F6）：
    `a20_clx_export_connect_local()`（export.c:254）替代已删除的
    `a20_clx_export_peer_for_connect()`——建 fresh pair，连接半以 NONBLOCK 注入
    一条零长度、携带一个 `A20_OBJ_CHANNEL_ENDPOINT` 句柄的连接请求（服务队列满
    立即失败，不在建链处阻塞），不再复用导出 peer。服务端约定与失败语义见
    01-abi 落地状态 #10；本机快路径指令序列不变（该路径本就不经过集群代码）。
12. **`A20_EXPORT_REPLACE` 落地**（WA1 终审 F5）：`a20_clx_export_register()`
    （export.c:36）在同名冲突且带 REPLACE 时按 revoke 语义当场退役旧导出（旧
    slot 推 `EXPORT_DROPPED`），再为新导出分配新 slot；退役先行保证"表满 +
    同名替换"也必然成功。行为登记见 01-abi 落地状态 #14。
13. **导出回复 worker 绑定注册身份**（WA1 F5 后续竞态修复）：REPLACE 退役旧导出
    后其回复 worker 可能仍阻塞在旧 peer 的 recv 上；表项随即被新导出复用时，
    旧 worker 退出路径曾无条件清 `ex->peer` 并释放——把新导出唯一的自有引用
    清掉，新导出沦为 in_use/peer==NULL 僵尸（远程 CALL 得 REMOTE_CLOSED）。
    修复：`a20_clx_export_spawn_worker()`（remote_ep.c:133）在 `g_clx_lock` 下
    为 worker 取该注册 peer 的**第二个引用**作为绑定身份（经 spawn 表
    `payload2` 传入，绑定在 spawn 时完成，避开"worker 首条指令晚于 REPLACE"
    的错绑窗口）；`clx_export_tx_main()`（remote_ep.c:687）只在
    `ex->peer == 绑定 peer` 时视自己存活、配对待发 FIFO 与清除/释放表项引用，
    退出时另释放自身引用。引用被 worker 全程持有，故指针值不会被回收复用，
    裸指针比较即可靠的所有权判定。自检用例"REPLACE survives old
    server-handle release"（1d2）钉住该回归：REPLACE 后才释放旧服务端句柄，
    再对新 slot 发远程 CALL，断言收到正常 REPLY。顺带登记：`clx_spawn_queue()`
    （remote_ep.c:103）在 `proc_alloc` 失败时回滚 spawn 槽，使 -1 严格表示
    "未入队"，调用方据此安全回收随槽传递的引用。
14. **BRINGUP 引导不经过自检挂钩（2026-10-07 核对发现）**：`kernel/main.c:201`
    的 `#if defined(BRINGUP) && !defined(CONFIG_RAMFS_USER)` 分支直接进
    `bringup_smoke_test()`，`a20_clx_selftest_boot()`（kernel/main.c:222）不执行。
    实测纯 `BRINGUP=1` 引导日志无 `[CLX-TEST]` 输出；改用
    `BRINGUP=1 RAMFS_USER=1`（或 dev 引导）后 24/24 通过（§7.2）。这属于
    引导路径分支事实，不是自检缺陷；WA2 起任何"引导即跑"的 cluster 检查都要
    注意同一分支。

# 传输层使用与参考（loopback / UART；UDP 未实现）

> **实现状态（2026-10-07 按源码核对）**
>
> | 传输 | transport_id | 状态 | 代码 |
> |---|---|---|---|
> | loopback | 0 | **已实现**，三档都有 | `kernel/cluster/loopback.c` / `loopback.h`，注册于 `kernel/cluster/transport.c:383-390` |
> | UDP | 1 | **未实现（WA2 计划）** —— `kernel/cluster/udp.c` 不存在，跨机 UDP `channel_call` 尚不可用 | §3 保留规范作为 WA2 依据（`docs/cluster/impl-prompts.md:124`） |
> | UART | 2 | **已实现**（DEFAULT/SERVER 档头节点 + MCU 档叶子） | 头节点 `kernel/cluster/uart.c`，共享编解码 `kernel/cluster/uart.h`，叶子 `kernel/mcu/leaf.c` / `leaf.h`，注册于 `kernel/cluster/transport.c:392-405` |
>
> 可靠性（分片重组、seq/ACK/重传/去重，WA3）与用户态 clusterd（WD1）均未实现；本文涉及处均已就地标注。

帧格式本身见 [02-wire-protocol.md](02-wire-protocol.md)，本文只讲**每种传输怎么把帧送上链路、怎么用、现在跑到什么程度**。新传输（SPI/CAN，v1+）按 §1 契约追加到本文，不改其他文档。

## 1. 传输契约 `a20_clx_transport_t`（参考）

每种传输向 cluster 核心注册一个这样的结构（定义见 `kernel/cluster/transport.h:27-37`）：

```c
typedef struct a20_clx_transport {
    uint32_t transport_id;     /* 0=loopback 1=udp 2=uart，注册时固定 */
    uint32_t mtu;              /* 含帧头与 CRC 的单帧上限 */
    uint32_t flags;            /* RELIABLE_CAPABLE / BROADCAST_CAPABLE / POLLING */
    /* 发送一帧到下一跳链路地址。返回 0 或 -errno。
       调用上下文：cluster TX 线程（可睡眠）；MCU POLLING 档可为轮询上下文。
       实现允许丢帧（返回 0 但丢弃）——可靠性由上层或不存在，传输不承诺。 */
    int  (*send)(void *ctx, const uint8_t *next_hop, uint32_t nh_len,
                 const void *frame, uint32_t len);
    /* POLLING 档：驱动层收包泵。非 POLLING 档置 NULL，收包经中断→环形缓冲→RX 线程。 */
    void (*poll)(void *ctx);
    /* 传输私有状态 */
    void *ctx;
} a20_clx_transport_t;
```

配套常量与注册行为（全部为现有实现，非计划）：

- `transport_id` 取值 `A20_CLX_TRANSPORT_LOOPBACK_ID/UDP_ID/UART_ID`（`kernel/cluster/transport.h:15-17`），与 ABI 侧 `A20_CLX_TRANSPORT_LOOPBACK/UDP/UART` 同值（`kernel/include/abi/native/types.h:1144-1146`）。
- `flags` 取值 `A20_CLX_TFL_RELIABLE_CAPABLE/BROADCAST_CAPABLE/POLLING`（`kernel/cluster/transport.h:20-22`）。链路地址上限 16 字节、传输表上限 8 条（`transport.h:24-25`）。
- 注册经 `a20_clx_transport_register()`：先到先得、重复 id 拒绝（`transport.h:39-43`，实现 `kernel/cluster/transport.c:126-144`）。现有两个传输的注册点是 `a20_clx_core_init()`（`transport.c:372-413`）——它由第一个 cluster syscall 惰性触发，不用集群的内核不为它付任何启动成本；新传输照此接入即可。

上行接口（传输 → cluster 核心，唯一入口；声明 `transport.h:45-56`）：

- `a20_clx_rx_frame(transport_id, next_hop_src, frame, len)`：收一帧。实现（`transport.c:197-230`）只做 kmalloc 拷贝并入 64 槽环形缓冲、唤醒 RX 线程，可在（未来的）中断上下文调用；RX 线程（`transport.c:315-328`）出队后经帧校验屏（`frame.c`）、身份解析再分发到代理/导出表。
- `a20_clx_link_event(transport_id, next_hop, UP/DOWN)`：链路级事件。实现（`transport.c:495-507`）更新链路状态表并推 `A20_CLX_EV_LINK_UP/DOWN` 给事件订阅者。
- 链路计数上行 `a20_clx_link_rx_drop / rx_malformed / tx_drop`（`transport.h:60-66`）：02-§10 计数器的数据源，由 `cluster_link_status` 读出（`transport.c:417-491`）。

`next_hop` 链路地址格式随传输（见各节），长度 ≤16 字节，与 `cluster_route` 注入的值一致（syscall 入口与参数校验见 `kernel/abi/native/sys_native_cluster.c:256-294`）。

## 2. loopback（transport_id=0，已实现，三档都有）

用途：单内核内虚拟节点互调（WA1 验收基座）、故障注入测试基座、CI 无硬件回归。

**原理**：`send` 把帧拷贝后投递进本内核的 `a20_clx_rx_frame`（`kernel/cluster/loopback.c:248-258`、`268-302`），零丢包、保序，不共享缓冲；MTU 注册为 65536（`transport.c:388`），免分片。链路地址是 4 字节虚拟节点号，本机节点固定为 `A20_CLX_LOOPBACK_SELF_ADDR`（0，`transport.h:70`）。loopback 不保持链路状态：HELLO/PING/PONG/ACK/NACK/SEND 帧在分发层直接丢弃计数（`transport.c:309-312`），链路有流量即 UP（`transport.c:181-188`）。

**怎么用（虚拟节点互调）**：以内核自检 `kernel/cluster/selftest.c` 为参照，三步：

1. `cluster_set_self` 定本机 node_id（syscall 实现 `sys_native_cluster.c:89`）。
2. `cluster_route(ADD, node_b, A20_CLX_TRANSPORT_LOOPBACK, next_hop=<4B 虚拟节点号>)` 把另一个身份指到 loopback——路由层在 ADD/REPLACE 成功时同步注册虚拟节点表（`kernel/cluster/route.c:130-143` → `loopback.c:35-67`）；自检里的写法是 `next_hop = {0x02,0,0,0}`（`selftest.c:75`、`219-221`）。重复 ADD 同一节点报 `-A20_ERR_EXISTS`（`route.c:135-136`）。
3. 之后 `cluster_export` / `cluster_connect`（`sys_native_cluster.c:115`、`166`）照常使用，跨虚拟节点的 CALL 走完整帧编解码与分发路径。

**怎么跑自检**：引导参数加 `clxselftest=1`（判定在 `selftest.c:692`，引导接线在 `kernel/main.c:215-222`）。自检在单内核上带一个虚拟节点 B 跑 WA1 验收清单——A/B echo 往返、connect(self)/connect(LOCAL)、link_status 聚合、export REPLACE、CLOSE 两分支、deadline 扫描等（用例清单见 `selftest.c:5-45`），每项打印 `[CLX-TEST] <name>: PASS/FAIL`，汇总行 `clx-selftest: <n>/<total>`。

**怎么用测试钩子**（故障注入基座）：

- 构建开关 `CONFIG_CLUSTER_TEST_HOOKS`：默认关，CI 测试构建开；未定义时 `loopback.c` 不含任何钩子代码路径（`loopback.c:117`、`274`）。
- 注入走内核命令行（bootargs 惯例，同 `chtrace=`）：
  `clxhooks=drop=<pct>,delay=<ms>,dup=1,corrupt=1,part=<a>.<b>`（解析见 `loopback.c:139-183`；虚拟节点号为十进制，`part` 指定一对节点）。判定用带种子的 LCG，同一引导内确定（`loopback.c:131-137`、`263`）。
- 各钩子语义：`drop_pct` 0–100 随机丢帧（`loopback.c:197-202`）；`dup` 复制一帧（`loopback.c:281-282`）；`corrupt` 翻转载荷随机 1 bit，配合 `csum_kind=1` 测 CRC 路径（`loopback.c:204-217`）；`part=<a>.<b>` 丢弃 a↔b 间全部帧模拟分区（`loopback.c:185-195`）；`delay_ms` 在 TX 线程内睡眠后再投递——v0 形态，调用方恒为 cluster worker 线程（`loopback.c:221-231`），定时器队列版随 WA2 落地。
- 与生产门禁的关系：[06-production.md](06-production.md) §1 的 P-故障注入矩阵按"loopback 钩子先跑、UDP 复跑"组织。钩子本身已在位可用；把矩阵接成 CI 门禁属 WE 轨道（`impl-prompts.md:228`），**未实现**。

### loopback 实现期记录（WA1；2026-10-07 复核：与 `loopback.c`/`transport.c`/`route.c` 现状一致）

1. **导出命名空间每内核一份**：loopback 的"虚拟节点"是路由层身份（经
   `cluster_route` 把 node_id ↔ 4B 虚拟节点号注册进 loopback 表），不是一个
   独立的协议栈实例；本机节点与全部虚拟节点共享同一张导出表。多实例语义
   （虚拟节点拥有各自导出表）是 WA2 的 UDP 真双实例拓扑的职责（WA2 未实现，
   见 §3）。
2. **身份核验按哈希而非链路地址**：进入 loopback 的帧 src_hash/dst_hash 都必须
   解析为托管身份（self 或已注册虚拟节点；分发实现 `transport.c:268-283`）；
   链路地址不用于绑定发送方——虚拟节点的回复在本机节点（0 号）链路地址上
   发出，地址绑定会误杀合法回复（WA1 自检实机暴露）。UDP/UART 的地址钉发
   （防欺骗）由各自传输承担。
3. **测试钩子注入机制选定**：内核命令行 `clxhooks=drop=<pct>,delay=<ms>,dup=1,
   corrupt=1,part=<a>.<b>`（bootargs 惯例，同 `chtrace=`；十进制虚拟节点号，
   解析见 `loopback.c:139-183`）。debugfs/proc 方案未选：本树没有现成
   debugfs，专用测试 syscall 会扩大冻结 ABI。`CONFIG_CLUSTER_TEST_HOOKS`
   未定义时本文件不含任何钩子代码路径（已用 riscv64 编译验证两种配置）。
4. **delay 钩子的 v0 形态**：在 TX 线程内睡眠后再投递（`loopback.c:221-231`，
   调用方恒为 cluster worker 线程，绝不可能是中断上下文）；定时器队列版随
   WA2 的 UDP 工作落地（未实现）。
5. **UART 包装**：`a20_clx_transport_t` 注册表在 `kernel/cluster/transport.c`；
   UART（POLLING 档）经 `clx_uart_send_wrap`/`clx_uart_poll_wrap` 适配 WC1 的
   `a20_clx_uart_send_frame`/`a20_clx_uart_poll_all`，rx/link 上行经
   `a20_clx_uart_set_rx_handler`/`set_link_event_handler` 接入
   （`transport.c:339-405`）。UDP 传输（transport_id=1）归 WA2，注册表已留位
   （`A20_CLX_TRANSPORT_UDP_ID` 常量见 `transport.h:16`）。

## 3. UDP（transport_id=1）——**未实现（WA2 计划）**

> **这一节是规范，不是可用功能。** `kernel/cluster/udp.c` 不存在，注册表里没有 id=1 的传输；`cluster_route` 虽接受 `transport_id=1` 的参数校验（`sys_native_cluster.c:273-276`），但发出的帧会因查无传输而落 `-A20_ERR_NODE_UNREACHABLE`（`transport.c:174-178`）。跨机 UDP `channel_call` 要等 WA2 轨道（`docs/cluster/impl-prompts.md:124`）落地。本节保留原规范全文作为该轨道的实现依据，逐条不得当作现状引用。

规范（WA2 依据）：

- 链路地址：6 字节 = IPv4（4B）+ 端口（2B）。v0 只支持 IPv4（lwIP 配置现状）；IPv6 留 reserved。
- MTU：1472（1500 - IP 20 - UDP 8）。分片由 02-§5 处理（WA3 亦未实现），**禁止**依赖 IP 层分片（丢一片毁整报文，且 MCU 链路无此概念，语义要对齐）。
- 端口：默认 44020 + 实例内节点序号，实例 TOML 可配。bind 失败（占用）→ 建链失败报 LINK_DOWN。
- 实现要点：
  - 用内核内部 socket 接口（内核网络栈为 lwIP：`kernel/net/lwip_stack.c`、`kernel/net/socket_internal.h`；参考 conntrack/DHCP 等内核态使用者的既有调用方式，不要从 syscall 层绕）。
  - 每传输实例一个内核 RX 线程：recvfrom → `a20_clx_rx_frame`；TX 直接 sendto（UDP 无连接）。
  - 心跳/状态机按 02-§6：PING 1s、丢 3 SUSPECT、再丢 3 DOWN。
  - SERVER 档：收发 lane 化参照 [docs/net/net-lanes.md](../net/net-lanes.md) 的分桶思路——按 src_hash 低位分 lane，每 lane 独立锁与队列，避免单锁热点。DEFAULT 档单 lane。
- QEMU 双实例网络拓扑（WB2 建立，WA2 使用）：两实例各一个 socket 后端网卡互联或经 host UDP 转发；具体接线命令记录在 `docs/cluster/` 演示文档，必须脚本化可重跑。
  - WB2 落地（实现期核对 2026-10）：选型为 `-netdev socket` 的 **UDP 模式**——A 挂 `udp=127.0.0.1:44122,localaddr=127.0.0.1:44121`，B 端口对调，两实例处于同一条 L2 段、无启动顺序依赖（listen/connect 拓扑同样在允许集合内，本文选对称方案）。实例文件 `instances/qemu-riscv64-cluster-{a,b}.toml`（guest IP 10.0.3.2/24 与 10.0.3.3/24，独立 MAC），接线脚本 `tools/cluster-net-up.sh`（A ping B、B ping A 自证），命令与预期输出见 [02-udp-demo.md](02-udp-demo.md)。
  - 支撑上述拓扑的实例 schema 扩展（改动已按"最小化、向后兼容"落地，`make check-manifests` 证明全部既有 toml 语义不变）：`[net].mac`（NIC MAC，覆盖 QEMU 内置默认——双实例共段必须各自显式）、`[net].backend`（整段 `-netdev` 规格，替换默认 SLIRP，须带 `id=net`）、`[net].guest_ip`/`guest_netmask`/`guest_gateway`（→ `-append` 的 `a20.dhcp=0 a20.ip=... a20.netmask=... a20.gateway=...`，由 `kernel/net/net_config.c` 消费）。
  - guest 集群 UDP 端口（本节上文"默认 44020+序号，实例 TOML 可配"）**尚未**进实例 schema：其消费者 `kernel/cluster/udp.c` 与 clusterd 属于 WA2，现在加字段就是没有消费者的死配置；WA2 落地时把它加进 `[net]` 并接线，host 侧隧道端口已刻意选 44121/44122 与之错开。

**今天就能用的部分**：WB2 环境不依赖任何集群代码——`tools/cluster-net-up.sh` 现在就能把两个 QEMU 实例接上同一 L2 段并互 ping 自证（用法与预期输出见 [02-udp-demo.md](02-udp-demo.md)）。那是 ICMP 可达性验证，不是集群帧；WA2 的验收（demo-echo 四路径、金样对拍）在这个环境上跑。

## 4. UART（transport_id=2，已实现；头节点 `kernel/cluster/uart.c`，叶子 `kernel/mcu/leaf.c`）

### 帧封装（SLIP 变体）

| 字节 | 用途 |
|---|---|
| `0xC0` END | 帧定界：**帧首与帧尾各一个**（帧首 END 同时充当再同步点） |
| `0xDB` ESC | 转义前缀 |
| `0xDB 0xDC` | 数据中的 `0xC0` |
| `0xDB 0xDD` | 数据中的 `0xDB` |

- 载荷 = 完整 CL 帧（32B 头 + payload + CRC16，`csum_kind=1` 强制）。SLIP 转义发生在编码后的字节流上。常量与编码器见 `kernel/cluster/uart.h:164-174`（`A20_CLX_SLIP_END/ESC/ESC_END/ESC_ESC`）、`a20_clx_slip_encode()`（`uart.h:365-388`）。
- MTU：256（含帧头+CRC，不含 SLIP 转义开销，`uart.h:165`；最大载荷 222，`uart.h:166-167`）。115200 8N1 下满帧约 30ms——**MCU 档消息预算必须以此设计**（叶子算子载荷建议 ≤128 字节）。
- 部分帧超时：字节间静默 > 50ms 丢弃当前收半帧，回到找 END 状态（常量 `A20_CLX_UART_INTERBYTE_MS`，`uart.h:177`；判定在流式解码器 `a20_clx_slip_rx_timeout()`，`uart.h:527-536`）。
- 解转义后的半帧在 END 处结算（实现期核对 2026-10，与 `tools/cluster-ref` 金样一致；解码器 `a20_clx_slip_rx_byte()`，`uart.h:453-519`）：
  不足 32 字节 CL 头、或首两字节非 `43 4C` 的"帧"不交给 CL 层（丢弃，SLIP 层记
  truncated/bad_magic）；半帧超出 512 B 接收缓冲即刻丢弃，已收字节全部作废，
  到下一 END 再同步；坏转义（`0xDB` 后跟 `0xDC`/`0xDD` 之外）消费该字节对、不产出
  数据字节。
- 链路地址：2 字节短地址，HELLO 时头节点分配（02-§6）；`0xFFFF` = 未分配（`A20_CLX_UART_SHORT_UNASSIGNED`，`uart.h:168`；叶子上电初值，只许发 HELLO 族——落地为发送闸门，见下）。

### 头节点侧：怎么用

头节点链路机整体在 `#if CONFIG_CLUSTER_PROFILE >= 2` 内（`uart.c:57`；档推导见 `uart.h:49-59`），DEFAULT/SERVER 档编入。

1. **注册链路**：`a20_clx_uart_link_register(&cfg)`（声明 `uart.h:640`，实现 `uart.c:618-660`）。`cfg` 带本机 node_id（16B）、要分配给叶子的短地址、以及字节级驱动缝 `getc`/`putc`（`a20_clx_uart_drv_t`，`uart.h:621-625`）——`uart.c` 不直接碰串口寄存器，板级胶合在此插入。静态链路表上限 4 条（`A20_CLX_UART_MAX_LINKS`，`uart.h:198`）。
2. **传输契约接线已由核心完成**：`a20_clx_core_init()` 把 `a20_clx_uart_send_frame`/`a20_clx_uart_poll_all` 包成 POLLING 档 `a20_clx_transport_t` 并接好 rx/link 上行（`transport.c:392-405`；包装器 `transport.c:339-368`）。没注册任何链路时传输保持惰性。
3. **发数据**：先 `cluster_route(node, A20_CLX_TRANSPORT_UART, next_hop=<2B 短地址>)`，之后核心路由命中即调 `a20_clx_uart_send_frame()`（`uart.c:678-716`）：`nh_len≠2` 返回 `-EINVAL`；帧超 MTU 返回 `-EMSGSIZE`（UART 禁分片，02-§5）；`next_hop=0xFFFF` 或无 UP 链路认领该短地址返回 `-ENOENT` 并计 route miss（`uart.c:691-692`、`714`）。
4. **泵**：POLLING 档，一个泵上下文周期调 `a20_clx_uart_poll_all()`（`uart.c:753-759`），它逐链路 drain RX 字节、跑 50ms 半帧超时与 HELLO/PING 定时器（`uart.c:718-751`）。发送路径可从 cluster TX 线程并发进入；并发纪律（锁不跨驱动 IO）见 `uart.c:28-32`。
5. **观测**：`a20_clx_uart_get_status()` 回读链路状态、双方短地址、RTT 与 02-§10 计数器（`uart.c:782-816`；内部四态 DOWN/HELLO_SENT/UP/SUSPECT 在此映射为 ABI 三态，`uart.h:682-691`）；`cluster_link_status` 走统一链路表（`transport.c:417-491`）。

### 头节点侧：原理（简短）

- **头节点拨号**：链路从 DOWN 冷却后发 HELLO（载荷携带分配给叶子的短地址，`uart.c:549-563`）；HELLO_SENT 单次超时 2s、3 次未果回 DOWN 并上报 LINK_DOWN（`uart.c:566-577`；常量 `uart.h:190-191`）。HELLO 校验（`uart.c:190-246`）覆盖 ver、定长 32B 载荷、src_hash=FNV-1a(node_id)、保留/自身 id、自环 nonce、未知 caps 位、叶子不得置 RELAY、协议区间相交；任何失败回 ERROR(CLUSTER_UNSUPPORTED) 并 DOWN（`uart.c:249-262`）。HELLO_ACK 里叶子回显的短地址被采纳为对端地址（`uart.c:348-362`）。
- **心跳**：UP 后每 5s 发 PING（`A20_CLX_UART_PING_PERIOD_MS`，`uart.h:179`；发送在 `uart.c:579-607`），丢 3 个进 SUSPECT、再丢 3 个回 DOWN（`uart.c:584-596`）；PONG 逐字节回显 PING 的 8B 时间戳，头节点据此算 RTT 滑动均值（α=1/8，`uart.c:440-477`）。
- **为什么帧首也要 END**：它是接收方的再同步点——帧间噪声直接忽略，半帧作废后到下一 END 重新对齐（`uart.h:361-363`、`480-487`）。
- **为什么编解码是 header-only 纯函数**：SLIP/CRC16/FNV/LE 访问器/帧构造/校验屏全部在 `uart.h`，头节点与 MCU 叶子共用同一份，两端字节级一致且可被主机测试直接 `#include`（`uart.h:12-14`；03-§3"代码路径同源"）。

### 叶子侧（MCU）：怎么用与约束

- **构建与启动**：MCU profile 把 `kernel/mcu/leaf.c` 编进镜像（`components/trim.toml:45`）。`kernel_main` 调 `a20_mcu_leaf_init()`（`kernel/mcu/main.c:65`）；5ms 外设线程周期调 `a20_mcu_leaf_poll()`（`main.c:18-29`）。泵周期必须 ≪ 128B 控制台 RX 环的灌满时间（115200 下约 11ms；环深 `kernel/mcu/uart.c:5`，原理说明 `kernel/mcu/leaf.h:81-85`）。字节接口复用 MCU 控制台 UART 驱动 `uart_putc`/`uart_try_getc`（`kernel/mcu/uart.c:17`、`34`；叶子侧调用点 `leaf.c:122-127`、`552`）。
- **缓冲**：单接收缓冲 512B 静态数组（存**解码后**的帧）+ 单发送缓冲 320B（存转义前完整帧；最大转义膨胀 2× 场景按帧上限+余量），见 `leaf.c:71-72`。全静态分配，叶子不使用堆。
- **协议面（只应答）**：HELLO→HELLO_ACK 被动应答（`leaf.c:263-307`）、PING→PONG 载荷逐字节回显（`leaf.c:309-330`）、CALL→CALL_REPLY 单在飞（`A20_LIMIT_CLX_INFLIGHT_MCU=1`）并带 1 项 `(src_hash, txid)` 去重缓存——重投的 CALL 重发缓存 REPLY 而不重执行（`leaf.c:95-99`、`396-402`）、未知槽位回 ERROR(NOT_FOUND)（`leaf.c:405-421`）、CLOSE 使对应缓存失效、无应答（`leaf.c:445-466`）。收到 SEND/ACK/NACK/分片/广播帧：丢弃计数，不视为错误（`leaf.c:478-517`）。
- **两态状态机**：只有 DISCONNECTED/UP（`leaf.h:58-60`）；50s 无 PING 回 DISCONNECTED，短地址作废（看门狗在 `leaf.c:563-566`，常量 `uart.h:181`）。
- **`0xFFFF` 发送闸门**：未获分配地址时，叶子唯一合法的线上输出是 HELLO 族（实现上即 HELLO_ACK 应答；闸门在 `leaf.c:132-140`）。头节点侧对称：链路非 UP 时数据帧进 tx_drops。
- **内置演示算子**：u32 向量点积，槽位 `A20_MCU_LEAF_OP_DOT_SLOT=1`（0 保留；`leaf.h:56`）。CALL 载荷 `u32 n; i32 a[n]; i32 b[n]`（定长 4+8n；MTU 限定 n≤27，`leaf.c:348-350`），CALL_REPLY 载荷 `u32 status; i64 dot`（12B，`leaf.c:424-434`）。算子级失败（载荷畸形）走 REPLY 的 status 字段（值 12 = `A20_ERR_INVALID_ARGUMENT`，`leaf.c:62`；应用层空间），ERROR 帧保留给 02-§8 集群 errno 集。
- **身份**：默认演示身份为可打印串 `MCU-LEAF-DEMO-01`（`leaf.c:528`），用 `a20_mcu_leaf_set_identity()` 在首个 HELLO 到达前替换；LOCAL/BROADCAST 保留 id 会被拒（`leaf.c:539-544`）。主机测试缝 `a20_mcu_leaf_on_frame()` 可绕过 UART/SLIP 直接喂解码后帧（`leaf.c:578-581`）。
- **stm32 QEMU 探针镜像例外**：`CONFIG_STM32_QEMU` 构建且未显式 `CONFIG_MCU_CLUSTER_LEAF=1` 时 leaf.c 编译为无缓冲 no-op——该镜像（见 `kernel/mcu/main.c:82-85` 注释）处于 SRAM 边缘且无对端；实板 MCU 构建一律带完整叶子（512+320B + 状态 ≈1.1KiB SRAM，其中 832B 是本节强制的缓冲）。这是"512B/320B 必须在位"的唯一例外，开开关即恢复（`leaf.c:46-54`）。
- **叶子泵与控制台共用 UART**：当前 stm32 板仅一路控制台 UART，协议帧与 printf 共用链路。叶子稳态零 klog；帧间噪声由 END 定界 + CRC 吸收。头节点演示需第二串口或控制台静默（集成步处理）。
- **头节点侧的寻址补充**：短地址段 → uart 传输的静态路由当前由 `cluster_route` 直接注入（2B 短地址，见上"发数据"）。"短地址 ↔ 完整 node_id 映射表由 clusterd 维护"（[05-userspace.md](05-userspace.md)）是规划——**clusterd 未实现（WD1 计划，`impl-prompts.md:188`）**。同一 UART 挂多叶子（总线型）v0 不支持——一 UART 一叶子，多叶子用多 UART 或头节点间桥接；树形拓扑经头节点的 RELAY 能力转发（TTL 递减按 02-§1；RELAY 转发本身在 v0 数据面未实现，分发层对非托管目的直接丢弃，`transport.c:274` 注释）。

### 怎么对拍（自检 / 主机测试 / 金样）

- **内核自检**：`clxselftest=1` 引导参数，见 §2。
- **主机测试**：`make host-tests` 构建并运行 `tools/tests/*.c`（规则 `tools/targets-gates.mk:90-94`，主机 TU 带 `-Ikernel/include -Ikernel` 直接纳入内核实现文件）。其中：
  - `tools/tests/test_clx_uart.c` 把 codec 锚定到 `tools/cluster-ref/clframe.py` 的值（CRC 目录值 0x29B1，`test_clx_uart.c:36-39`；FNV 锚点、SLIP 转义向量同文件），并**内嵌重放**三份金样流 `slip-two-frames-01`、`slip-mal-badescape-01`、`slip-uart-budget-06`（`test_clx_uart.c:289-294`，hex 取自 `tools/cluster-ref/vectors/slip/`）；
  - `tools/tests/test_mcu_leaf.c` 用真实叶子代码跑端到端（握手/心跳/点积/去重/CLOSE/50s 看门狗/0xFFFF 闸门，场景清单见文件头注释 `test_mcu_leaf.c:1-20`）。
  - 2026-10-07 实测：`test_clx_uart`、`test_mcu_leaf`、`test_clx_frame` 三个主机测试全部通过（本机执行，见文末落地状态）。
- **金样全量对拍**：`python3 tools/cluster-ref/check_c_side.py` 按 README 约定逐向量比对解码输出（用法见 `check_c_side.py:1-30`）。2026-10-07 实测全绿：`all green (1889 checks over 91 frame + 16 slip vectors)`。金样由 `gen_vectors.py` 生成、不得手改（`vectors/INDEX.md` 头部；清单 `vectors/MANIFEST.json`）。把**内核**解码器作为 `--decoder` 接入跑全集，仍归 WA2 联调的集成步（未做）。

### 实现期记录（WC1，2026-10 按源码核对；2026-10-07 复核与 `uart.c`/`uart.h`/`leaf.c` 现状一致）

WC1 落地 `kernel/cluster/uart.{c,h}` 与 `kernel/mcu/leaf.{c,h}` 后对本节的实现期决定。冲突仲裁顺序仍是代码现实 > 子系统规范 > 顶层设计；改任何一条请先改这里。

1. **握手方向（对上文与 02-§6 歧义的裁决）**：UART 链路由**头节点拨号**——头节点发 HELLO（载荷 short_addr = 分配给叶子的短地址），叶子**被动**应答 HELLO_ACK。依据：01-abi §能力位 `A20_CLUSTER_CAP_LEAF`（"只应答，不主动建链"）与本节"HELLO/HELLO_ACK（被动应答）"；02-§6 状态机里"发 HELLO"的一侧即头节点。`0xFFFF=未分配，只许发 HELLO` 落地为**发送闸门**：叶子未获地址时唯一合法的线上输出是 HELLO 族（本实现中即 HELLO_ACK 应答，`leaf.c:132-140`）；头节点侧对称——链路非 UP 时传输只放行 HELLO/HELLO_ACK，数据帧 tx_drops 计数（`a20_clx_uart_send_frame` 对 next_hop=0xFFFF 直接返回 -ENOENT，`uart.c:691-692`）。
2. **缓冲语义**：512B 单收缓冲存**解码后**的帧（SLIP 流式解码，转义不占存储）；320B 单发缓冲存**转义前**的完整帧，转义经共享的 `a20_clx_slip_emit()` 逐字节流式输出（`uart.h:586-604`）。`slip-uart-budget-06` 金样（162B 帧全转义 312B ≤ 320B）与该语义一致。
3. **SLIP 解码语义的落点**：上文"解转义后的半帧在 END 处结算"三条规则按金样实现于 `uart.h` 的流式解码器（坏转义消费字节对且半帧存活、超缓冲即刻丢弃作废、空帧为再同步产物，`uart.h:453-519`）；"不交给 CL 层 + 记数"由两端共用的传输层校验屏 `a20_clx_screen_frame()`（magic/csum_kind/实收长度/ttl/CRC，`uart.h:555-580`）承担，计数归 02-§10 的 `rx_malformed`/`rx_drops`。与参考实现的一处**有意**差异：参考 `slip_decode` 会把 END 之前的裸噪声累积进候选帧再靠 magic 拒收；C 解码器以帧首 END 为再同步点（§4 表格原文），帧间噪声直接忽略——所有在库金样两者结果一致，共享控制台 UART 的实板环境后者更稳。
4. **代码组织**：SLIP/CRC16/FNV/LE 访问器/帧构造/校验屏为 **header-only 纯函数**，在 `kernel/cluster/uart.h`，头节点（`uart.c`）与叶子（`kernel/mcu/leaf.c`）共用同一份（03-§3"代码路径同源"）。`kernel/cluster/uart.h` 由 WC1 创建（本节登记）；`a20_clx_transport_t` 结构仍归 WA 的 `transport.c/transport.h`——uart.c 提供被包装的 `a20_clx_uart_send_frame/poll_all` 与 rx/link-event 上行 handler 注册接口，由 WA 包成 04-§1 契约（接线见 `transport.c:392-405`）。
5. **分档（03-§4 口径）**：Makefile 的 `-DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE)` 由 WA3 接线（WA3 未实现）；接线前 `uart.h` 自行推导——定义了 `CONFIG_MCU` 即 1，否则 2（`uart.h:49-59`，与 03-§4 "PROFILE=mcu 自动 CLUSTER_PROFILE=1" 同向）。`uart.c` 的头节点链路机整体在 `#if CONFIG_CLUSTER_PROFILE >= 2` 内（`uart.c:57`）；MCU 档编入 `trim.toml` 的只有 `kernel/mcu/leaf.c`。
6. **HELLO 重试参数**：02-§6 定"超时×3"未定单次超时；实现取 2s、重试 3 次（`uart.h:190-191`），量级上远大于 115200 下约 30ms 的满帧线上时长；DOWN 后重拨冷却同值，常量集中在 `uart.h` 便于集成步调整。
7. **叶子去重窗口**：02-§4"≥64 项"按 DEFAULT 档服务方口径理解；MCU 叶子单在飞（`A20_LIMIT_CLX_INFLIGHT_MCU=1`），去重缓存取 1 项 `(src_hash, txid)`（`leaf.c:95-99`），命中重发缓存 REPLY 不重执行（`leaf.c:396-402`）。内置算子为纯函数，缓存未命中重算无副作用。
8. **叶子算子与 REPLY 载荷**：演示算子 u32 向量点积，CALL 载荷 `u32 n; i32 a[n]; i32 b[n]`（定长 4+8n，MTU 限定 n≤27），CALL_REPLY 载荷 `u32 status; i64 dot`（12B）。算子级失败（载荷畸形 → `A20_ERR_INVALID_ARGUMENT`）走 REPLY 的 status 字段（应用层空间）；ERROR 帧保留给 02-§8 集群 errno 集（槽位未找到 → NOT_FOUND；已持地址时 HELLO 校验失败 → CLUSTER_UNSUPPORTED）。算子槽位 `A20_MCU_LEAF_OP_DOT_SLOT=1`（0 保留，`leaf.h:56`）。
9. **stm32 QEMU 探针镜像例外**：`CONFIG_STM32_QEMU` 构建且未显式 `CONFIG_MCU_CLUSTER_LEAF=1` 时 leaf.c 编译为无缓冲 no-op（`leaf.c:46-54`）——该镜像（见 `kernel/mcu/main.c` 注释）处于 SRAM 边缘且无对端；实板 MCU 构建一律带完整叶子（512+320B + 状态 ≈1.1KiB SRAM，其中 832B 是本节强制的缓冲）。这是"512B/320B 必须在位"的唯一例外，开开关即恢复。
10. **叶子泵与控制台共用 UART**：当前 stm32 板仅一路控制台 UART，协议帧与 printf 共用链路。叶子稳态零 klog；帧间噪声由 END 定界 + CRC 吸收。头节点演示需第二串口或控制台静默（集成步处理）。叶子泵挂 5ms 外设线程（`kernel/mcu/main.c:18-29`），周期须 ≪128B RX 环灌满时间（115200 下约 11ms）。
11. **构建接线（WA/构建所有者请过目）**：`Makefile` 非 MCU 档 `KERNEL_SRC` 增加 `$(wildcard kernel/cluster/*.c)`（否则编译门禁看不到 uart.c；`Makefile:1235`）；`components/trim.toml [profile.mcu].sources` 增加 `kernel/mcu/leaf.c`（`trim.toml:45`，trim.mk 已 regen）；`tools/targets-gates.mk` host 测试规则加 `-Ikernel`（使主机测试能纳入内核实现文件，`targets-gates.mk:93-94`）。
12. **主机侧对拍现状**：`tools/tests/test_clx_uart.c` 把 codec 锚定到 `tools/cluster-ref/clframe.py` 的值（CRC 目录值 0x29B1、FNV 锚点、SLIP 转义向量）并**内嵌重放**三份金样流（`slip-two-frames-01`、`slip-mal-badescape-01`、`slip-uart-budget-06`）；`tools/tests/test_mcu_leaf.c` 用真实叶子代码跑端到端（握手/心跳/点积/去重/CLOSE/50s 看门狗/0xFFFF 闸门）。金样全集对拍（`check_c_side.py` 约定）仍归集成步。2026-10-07 复核：参考解码器 refdec 对全集金样已全绿（1889 checks，见 §4"怎么对拍"）；待做的集成步特指把内核解码器接入 `--decoder`。

## 5. 传输对比速查

| | loopback | UDP | UART |
|---|---|---|---|
| 实现状态 | **已实现** | **未实现（WA2 计划）** | **已实现** |
| 代码 | `kernel/cluster/loopback.c` | WA2 规划文件 `kernel/cluster/udp.c` 尚未创建 | `kernel/cluster/uart.c` + `kernel/mcu/leaf.c` |
| MTU | 65536（免分片） | 1472（规范值） | 256 |
| 保序 | 是 | 否（IP 层乱序可能） | 是（单线） |
| 丢包 | 钩子控制 | 自然 | CRC 丢弃 |
| 心跳周期 | 无（常 UP） | 1s（规范值） | 5s（头→叶） |
| 分片 | 不需要 | RELIABLE 档需要（WA3 亦未实现） | 禁止 |
| 链路地址 | 4B 虚拟节点号 | 4B IP + 2B port（规范值） | 2B 短地址 |

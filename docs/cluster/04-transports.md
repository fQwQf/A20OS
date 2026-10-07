# 传输层规范（loopback / UDP / UART）

内容已按 2026-10 源码核对。帧格式见 02-wire-protocol.md，本文只规定**每种传输怎么把帧送上链路**。新传输（SPI/CAN，v1+）按 §1 契约追加到本文，不改其他文档。

## 1. 传输契约 `a20_clx_transport_t`

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

上行接口（传输 → cluster 核心，唯一入口）：

- `a20_clx_rx_frame(transport_id, next_hop_src, frame, len)`：收一帧。可在中断上下文调用（内部只入环形缓冲）。
- `a20_clx_link_event(transport_id, next_hop, UP/DOWN)`：链路级事件，进事件订阅与路由失效判定。

`next_hop` 链路地址格式随传输（见各节），长度 ≤16 字节，与 `cluster_route` 注入的值一致。

## 2. loopback（transport_id=0，三档都有）

- 用途：单内核内虚拟节点互调（WA1 验收）、故障注入测试基座、CI 无硬件回归。
- 链路地址：4 字节虚拟节点号。`cluster_set_self` 后，测试代码可经 `cluster_route` 把其他虚拟节点号指向 loopback。
- 语义：`send` = 把帧直接投递到目标虚拟节点的 `a20_clx_rx_frame`（拷贝，不共享缓冲）。零丢包、保序。
- **测试钩子**（`CONFIG_CLUSTER_TEST_HOOKS`，默认关，CI 测试构建开）：
  - `drop_pct`（0–100 随机丢帧）、`delay_ms`（入延迟队列）、`dup`（复制一帧）、`corrupt`（翻转载荷随机 1 bit，配合 csum_kind=1 测 CRC 路径）、`partition`（丢弃某虚拟节点对间全部帧，模拟分区，可恢复）。
  - 钩子参数经 debugfs/proc 或专用测试 syscall 注入（按仓库现有测试设施惯例选择，登记到本文）。
  - 故障注入矩阵（06-production.md）的全部用例先在 loopback 上跑通，再到 UDP 复跑。

## 3. UDP（transport_id=1，DEFAULT/SERVER 档）

- 链路地址：6 字节 = IPv4（4B）+ 端口（2B）。v0 只支持 IPv4（lwIP 配置现状）；IPv6 留 reserved。
- MTU：1472（1500 - IP 20 - UDP 8）。分片由 02-§5 处理，**禁止**依赖 IP 层分片（丢一片毁整报文，且 MCU 链路无此概念，语义要对齐）。
- 端口：默认 44020 + 实例内节点序号，实例 TOML 可配。bind 失败（占用）→ 建链失败报 LINK_DOWN。
- 实现要点：
  - 用内核内部 socket 接口（`kernel/net/socket.c`；参考 conntrack/DHCP 等内核态使用者的既有调用方式，不要从 syscall 层绕）。
  - 每传输实例一个内核 RX 线程：recvfrom → `a20_clx_rx_frame`；TX 直接 sendto（UDP 无连接）。
  - 心跳/状态机按 02-§6：PING 1s、丢 3 SUSPECT、再丢 3 DOWN。
  - SERVER 档：收发 lane 化参照 `docs/net/net-lanes.md` 的分桶思路——按 src_hash 低位分 lane，每 lane 独立锁与队列，避免单锁热点。DEFAULT 档单 lane。
- QEMU 双实例网络拓扑（WB2 建立，WA2 使用）：两实例各一个 socket 后端网卡互联或经 host UDP 转发；具体接线命令记录在 `docs/cluster/` 演示文档，必须脚本化可重跑。
  - WB2 落地（实现期核对 2026-10）：选型为 `-netdev socket` 的 **UDP 模式**——A 挂 `udp=127.0.0.1:44122,localaddr=127.0.0.1:44121`，B 端口对调，两实例处于同一条 L2 段、无启动顺序依赖（listen/connect 拓扑同样在允许集合内，本文选对称方案）。实例文件 `instances/qemu-riscv64-cluster-{a,b}.toml`（guest IP 10.0.3.2/24 与 10.0.3.3/24，独立 MAC），接线脚本 `tools/cluster-net-up.sh`（A ping B、B ping A 自证），命令与预期输出见 `docs/cluster/02-udp-demo.md`。
  - 支撑上述拓扑的实例 schema 扩展（改动已按"最小化、向后兼容"落地，`make check-manifests` 证明全部既有 toml 语义不变）：`[net].mac`（NIC MAC，覆盖 QEMU 内置默认——双实例共段必须各自显式）、`[net].backend`（整段 `-netdev` 规格，替换默认 SLIRP，须带 `id=net`）、`[net].guest_ip`/`guest_netmask`/`guest_gateway`（→ `-append` 的 `a20.dhcp=0 a20.ip=... a20.netmask=... a20.gateway=...`，由 `kernel/net/net_config.c` 消费）。
  - guest 集群 UDP 端口（本节上文"默认 44020+序号，实例 TOML 可配"）**尚未**进实例 schema：其消费者 `kernel/cluster/udp.c` 与 clusterd 属于 WA2，现在加字段就是没有消费者的死配置；WA2 落地时把它加进 `[net]` 并接线，host 侧隧道端口已刻意选 44121/44122 与之错开。

## 4. UART（transport_id=2，MCU 档主力；WC 轨道拥有 `uart.c`）

### 帧封装（SLIP 变体）

| 字节 | 用途 |
|---|---|
| `0xC0` END | 帧定界：**帧首与帧尾各一个**（帧首 END 同时充当再同步点） |
| `0xDB` ESC | 转义前缀 |
| `0xDB 0xDC` | 数据中的 `0xC0` |
| `0xDB 0xDD` | 数据中的 `0xDB` |

- 载荷 = 完整 CL 帧（32B 头 + payload + CRC16，`csum_kind=1` 强制）。SLIP 转义发生在编码后的字节流上。
- MTU：256（含帧头+CRC，不含 SLIP 转义开销）。115200 8N1 下满帧约 30ms——**MCU 档消息预算必须以此设计**（叶子算子载荷建议 ≤128 字节）。
- 部分帧超时：字节间静默 > 50ms 丢弃当前收半帧，回到找 END 状态。
- 解转义后的半帧在 END 处结算（实现期核对 2026-10，与 `tools/cluster-ref` 金样一致）：
  不足 32 字节 CL 头、或首两字节非 `43 4C` 的"帧"不交给 CL 层（丢弃，SLIP 层记
  truncated/bad_magic）；半帧超出 512 B 接收缓冲即刻丢弃，已收字节全部作废，
  到下一 END 再同步；坏转义（`0xDB` 后跟 `0xDC`/`0xDD` 之外）消费该字节对、不产出
  数据字节。
- 链路地址：2 字节短地址，HELLO 时头节点分配（02-§6）；`0xFFFF` = 未分配（叶子上电初值，只许发 HELLO）。

### 叶子侧实现约束（stm32f103 级）

- 编入 `kernel/mcu/` 体系：复用其 `uart.c` 驱动与 `heap.c`；**禁止**引入 lwIP 或任何 DEFAULT 档依赖。
- 单接收缓冲 512B 静态数组 + 单发送缓冲 320B（最大转义膨胀 2× 场景按帧上限 + 余量）。
- 叶子只需实现的帧类型：HELLO/HELLO_ACK（被动应答）、PING→PONG、CALL→CALL_REPLY、ERROR、CLOSE。收到 SEND/ACK/NACK/分片帧：丢弃计数，不视为错误。
- 状态机裁剪：只有 DISCONNECTED/UP 两态；50s 无 PING 回 DISCONNECTED，短地址作废。

### 头节点侧

- 静态路由：短地址段 → uart 传输；短地址 ↔ 完整 node_id 映射表在 clusterd（05-userspace.md）。
- 同一 UART 挂多叶子（总线型）v0 不支持——一 UART 一叶子，多叶子用多 UART 或头节点间桥接。树形拓扑经头节点的 RELAY 能力转发（TTL 递减按 02-§1）。

### 实现期记录（WC1，2026-10 按源码核对）

WC1 落地 `kernel/cluster/uart.{c,h}` 与 `kernel/mcu/leaf.{c,h}` 后对本节的实现期决定。冲突仲裁顺序仍是代码现实 > 子系统规范 > 顶层设计；改任何一条请先改这里。

1. **握手方向（对上文与 02-§6 歧义的裁决）**：UART 链路由**头节点拨号**——头节点发 HELLO（载荷 short_addr = 分配给叶子的短地址），叶子**被动**应答 HELLO_ACK。依据：01-abi §能力位 `A20_CLUSTER_CAP_LEAF`（"只应答，不主动建链"）与本节"HELLO/HELLO_ACK（被动应答）"；02-§6 状态机里"发 HELLO"的一侧即头节点。`0xFFFF=未分配，只许发 HELLO` 落地为**发送闸门**：叶子未获地址时唯一合法的线上输出是 HELLO 族（本实现中即 HELLO_ACK 应答）；头节点侧对称——链路非 UP 时传输只放行 HELLO/HELLO_ACK，数据帧 tx_drops 计数（`a20_clx_uart_send_frame` 对 next_hop=0xFFFF 直接返回 -ENOENT）。
2. **缓冲语义**：512B 单收缓冲存**解码后**的帧（SLIP 流式解码，转义不占存储）；320B 单发缓冲存**转义前**的完整帧，转义经共享的 `a20_clx_slip_emit()` 逐字节流式输出。`slip-uart-budget-06` 金样（162B 帧全转义 312B ≤ 320B）与该语义一致。
3. **SLIP 解码语义的落点**：上文"解转义后的半帧在 END 处结算"三条规则按金样实现于 `uart.h` 的流式解码器（坏转义消费字节对且半帧存活、超缓冲即刻丢弃作废、空帧为再同步产物）；"不交给 CL 层 + 记数"由两端共用的传输层校验屏 `a20_clx_screen_frame()`（magic/csum_kind/实收长度/ttl/CRC）承担，计数归 02-§10 的 `rx_malformed`/`rx_drops`。与参考实现的一处**有意**差异：参考 `slip_decode` 会把 END 之前的裸噪声累积进候选帧再靠 magic 拒收；C 解码器以帧首 END 为再同步点（§4 表格原文），帧间噪声直接忽略——所有在库金样两者结果一致，共享控制台 UART 的实板环境后者更稳。
4. **代码组织**：SLIP/CRC16/FNV/LE 访问器/帧构造/校验屏为 **header-only 纯函数**，在 `kernel/cluster/uart.h`，头节点（`uart.c`）与叶子（`kernel/mcu/leaf.c`）共用同一份（03-§3"代码路径同源"）。`kernel/cluster/uart.h` 由 WC1 创建（本节登记）；`a20_clx_transport_t` 结构仍归 WA 的 `transport.c/transport.h`——uart.c 提供被包装的 `a20_clx_uart_send_frame/poll_all` 与 rx/link-event 上行 handler 注册接口，由 WA 包成 04-§1 契约。
5. **分档（03-§4 口径）**：Makefile 的 `-DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE)` 由 WA3 接线；接线前 `uart.h` 自行推导——定义了 `CONFIG_MCU` 即 1，否则 2（与 03-§4 "PROFILE=mcu 自动 CLUSTER_PROFILE=1" 同向）。`uart.c` 的头节点链路机整体在 `#if CONFIG_CLUSTER_PROFILE >= 2` 内；MCU 档编入 `trim.toml` 的只有 `kernel/mcu/leaf.c`。
6. **HELLO 重试参数**：02-§6 定"超时×3"未定单次超时；实现取 2s（≈6 个满帧时长），DOWN 后重拨冷却同值，常量集中在 `uart.h` 便于集成步调整。
7. **叶子去重窗口**：02-§4"≥64 项"按 DEFAULT 档服务方口径理解；MCU 叶子单在飞（`A20_LIMIT_CLX_INFLIGHT_MCU=1`），去重缓存取 1 项 `(src_hash, txid)`，命中重发缓存 REPLY 不重执行。内置算子为纯函数，缓存未命中重算无副作用。
8. **叶子算子与 REPLY 载荷**：演示算子 u32 向量点积，CALL 载荷 `u32 n; i32 a[n]; i32 b[n]`（定长 4+8n，MTU 限定 n≤27），CALL_REPLY 载荷 `u32 status; i64 dot`（12B）。算子级失败（载荷畸形 → `A20_ERR_INVALID_ARGUMENT`）走 REPLY 的 status 字段（应用层空间）；ERROR 帧保留给 02-§8 集群 errno 集（槽位未找到 → NOT_FOUND；已持地址时 HELLO 校验失败 → CLUSTER_UNSUPPORTED）。算子槽位 `A20_MCU_LEAF_OP_DOT_SLOT=1`（0 保留）。
9. **stm32 QEMU 探针镜像例外**：`CONFIG_STM32_QEMU` 构建且未显式 `CONFIG_MCU_CLUSTER_LEAF=1` 时 leaf.c 编译为无缓冲 no-op——该镜像（见 `kernel/mcu/main.c` 注释）处于 SRAM 边缘且无对端；实板 MCU 构建一律带完整叶子（512+320B + 状态 ≈1.1KiB SRAM，其中 832B 是本节强制的缓冲）。这是"512B/320B 必须在位"的唯一例外，开开关即恢复。
10. **叶子泵与控制台共用 UART**：当前 stm32 板仅一路控制台 UART，协议帧与 printf 共用链路。叶子稳态零 klog；帧间噪声由 END 定界 + CRC 吸收。头节点演示需第二串口或控制台静默（集成步处理）。叶子泵挂 5ms 外设线程（`kernel/mcu/main.c`），周期须 ≪128B RX 环灌满时间（115200 下约 11ms）。
11. **构建接线（WA/构建所有者请过目）**：`Makefile` 非 MCU 档 `KERNEL_SRC` 增加 `$(wildcard kernel/cluster/*.c)`（否则编译门禁看不到 uart.c）；`components/trim.toml [profile.mcu].sources` 增加 `kernel/mcu/leaf.c`（trim.mk 已 regen）；`tools/targets-gates.mk` host 测试规则加 `-Ikernel`（使主机测试能纳入内核实现文件）。
12. **主机侧对拍现状**：`tools/tests/test_clx_uart.c` 把 codec 锚定到 `tools/cluster-ref/clframe.py` 的值（CRC 目录值 0x29B1、FNV 锚点、SLIP 转义向量）并**内嵌重放**三份金样流（`slip-two-frames-01`、`slip-mal-badescape-01`、`slip-uart-budget-06`）；`tools/tests/test_mcu_leaf.c` 用真实叶子代码跑端到端（握手/心跳/点积/去重/CLOSE/50s 看门狗/0xFFFF 闸门）。金样全集对拍（`check_c_side.py` 约定）仍归集成步。

## 5. 传输对比速查

| | loopback | UDP | UART |
|---|---|---|---|
| MTU | 65536（免分片） | 1472 | 256 |
| 保序 | 是 | 否（IP 层乱序可能） | 是（单线） |
| 丢包 | 钩子控制 | 自然 | CRC 丢弃 |
| 心跳周期 | 无（常 UP） | 1s | 5s（头→叶） |
| 分片 | 不需要 | RELIABLE 档需要 | 禁止 |
| 链路地址 | 4B 虚拟节点号 | 4B IP + 2B port | 2B 短地址 |


### loopback 实现期记录（WA1，2026-10 按源码核对）

1. **导出命名空间每内核一份**：loopback 的"虚拟节点"是路由层身份（经
   `cluster_route` 把 node_id ↔ 4B 虚拟节点号注册进 loopback 表），不是一个
   独立的协议栈实例；本机节点与全部虚拟节点共享同一张导出表。多实例语义
   （虚拟节点拥有各自导出表）是 WA2 的 UDP 真双实例拓扑的职责。
2. **身份核验按哈希而非链路地址**：进入 loopback 的帧 src_hash/dst_hash 都必须
   解析为托管身份（self 或已注册虚拟节点）；链路地址不用于绑定发送方——虚拟
   节点的回复在本机节点（0 号）链路地址上发出，地址绑定会误杀合法回复
   （WA1 自检实机暴露）。UDP/UART 的地址钉发（防欺骗）由各自传输承担。
3. **测试钩子注入机制选定**：内核命令行 `clxhooks=drop=<pct>,delay=<ms>,dup=1,
   corrupt=1,part=<a>.<b>`（bootargs 惯例，同 `chtrace=`；十进制虚拟节点号）。
   debugfs/proc 方案未选：本树没有现成 debugfs，专用测试 syscall 会扩大冻结
   ABI。`CONFIG_CLUSTER_TEST_HOOKS` 未定义时本文件不含任何钩子代码路径
   （已用 riscv64 编译验证两种配置）。
4. **delay 钩子的 v0 形态**：在 TX 线程内睡眠后再投递（调用方恒为 cluster
   worker 线程，绝不可能是中断上下文）；定时器队列版随 WA2 的 UDP 工作落地。
5. **UART 包装**：`a20_clx_transport_t` 注册表在 `kernel/cluster/transport.c`；
   UART（POLLING 档）经 `clx_uart_send_wrap`/`clx_uart_poll_wrap` 适配 WC1 的
   `a20_clx_uart_send_frame`/`a20_clx_uart_poll_all`，rx/link 上行经
   `a20_clx_uart_set_rx_handler`/`set_link_event_handler` 接入。UDP 传输
   （transport_id=1）归 WA2，注册表已留位。

# lwIP 分叉差异清单 (Divergence Manifest)

最后核实：2026-10-03（`644b3c76`）。

本文件记录 `kernel/external/lwip` 与上游 lwIP 的**可证明**差异。它存在的唯一目的
是让"外部依赖"这个标签不再误导读者，并让重新同步上游成为一件机械可执行的事。

## 0. 上游基线状态：未记录

**本仓库此前没有记录任何上游 SHA。** 这不是遗漏的文档，而是分叉治理缺失的第一个
症状：`kernel/external/` 的语义是"vendored third-party，不随上游风格重排"
（见仓库 README），但该目录里已经存在 A20OS 自有的协议栈概念，因此这个语义
现在是**不成立的**。

在本文件写入时，能从树内证明的上游身份只有：

| 事实 | 值 | 来源 |
|---|---|---|
| lwIP 版本宏 | `2.2.2` + `LWIP_RC_DEVELOPMENT` ⇒ 展开为 `2.2.2d` | `kernel/external/lwip/src/include/lwip/init.h:53-61` |
| 树内 git 元数据 | 无（无 `.git`、无 hash 文件、无 `CHANGELOG`、无 tag） | 全树搜索 |
| 布局 | `src/core/ipv4/`、`src/core/ipv6/`、`src/core/dns.c` | 2.2.0 之后的 master 重排布局 |

`2.2.2d` 是**开发快照**，不对应任何发布 tag，因此**不能用版本号反推唯一上游
commit**。现已显式抓取并记录基线：

| 项 | 值 |
|---|---|
| 上游 SHA | `d08f4773edd0182b7910fc8f046eed82ffcd67c9` |
| `git describe` | `d08f477`（无 tag 命中） |
| 记录位置 | `kernel/external/lwip/.upstream-base` |

### 0.1 两种"差异"必须分开看

把 vendored 树直接与上游 HEAD 对比，会把**两类改动混为一谈**：A20OS 自己写的，
和"上游前进了我们没拉"的。两者必须分别测量：

| 口径 | 测量方法 | 结果 |
|---|---|---|
| **A20OS 自己的改动** | `git diff f773b0aa HEAD -- kernel/external/lwip`（`f773b0aa` = 重新 vendoring 的提交） | **14 文件(源码),+1458 / −304** |
| **相对上游的落后程度** | vendoring 时的树 vs `d08f477` | **18 个文件不同** |
| **与上游 HEAD 的合并差异** | vendored 树 vs `d08f477` | 25 文件,+761 / −427 |

第三行是前两行的叠加，**不能**被当作"A20OS 改了多少"。本文件后续一律使用第一行。

## 1. 树的构成（树内可证明）

| 项 | 值 |
|---|---|
| 跟踪文件数 | 211 |
| `.c` 行数 | 34,964 |
| `.h` 行数 | 30,306 |
| 实际参与编译的 `.c` | **39**（由 `sources.mk` 唯一列举） |
| 编译产物（riscv64 hosted tier 2） | `.text` 389,018 + `.data` 168,592 + `.bss` 530,487 + `.rodata` 1,536 = **1,089,633 字节**，占 `kernel.elf` 的 7.1% |

### 1.1 从未参与编译的头文件（10,503 行）

`sources.mk` 只列 39 个 `.c`，因此下列头文件**一次都不被编译**，只增加树体积与
许可证审计面：

| 目录 | 行数 | 原因 |
|---|---|---|
| `src/include/lwip/apps/` | 4,865 | 35 个头（mqtt/snmp/httpd/mdns/lwiperf/smtp/sntp/tftp/netbios…），而树内**没有** `src/apps/` |
| `src/include/netif/ppp/` | 5,470 | 24+5 个头，`PPP_SUPPORT 0` |
| `src/include/compat/` | 168 | POSIX 兼容 shim，未使用 |

## 2. A20OS 自有改动

**测量口径：`git diff --numstat f773b0aa -- kernel/external/lwip/src kernel/external/lwip/sources.mk` = 13 文件，+1495 / −311**
（`f773b0aa` 是把 lwIP 重新 vendoring 进内核的提交，作为"未改动基线"）。

> 口径说明：数字只统计 `src/` 与 `sources.mk`，不含本文件自身；重跑上面那条
> `git diff` 即可复核。下文 §2.1 的清单以当前口径为准。

### 2.1 受影响的文件（13 个）

```
+397/-0   src/core/tcp_cubic.c                      【新增文件，上游无对应物】
+387/-157 src/core/tcp.c                            PCB 链表按 lane 分桶 + CUBIC RTO 分支 + wnd_limit
+181/-0   src/include/lwip/priv/tcp_cubic_priv.h    【新增文件，上游无对应物】
+178/-82  src/core/udp.c                            PCB 链表按 lane 分桶
+112/-0   src/include/lwip/priv/pcb_lane.h          【新增文件，上游无对应物】
 +98/-37  src/core/tcp_in.c                         lane 感知的输入查找 + CUBIC ACK 分派
 +61/-25  src/include/lwip/priv/tcp_priv.h         TCP_REG/TCP_RMV 改为 lane 索引
 +29/-1   src/include/lwip/tcp.h                    struct tcp_pcb 增加 lane / cong_alg / wnd_limit 字段
 +24/-6   src/core/pbuf.c                           LS2K1000 板级诊断 printf
 +18/-0   src/core/tcp_out.c                        CUBIC 快重传分支
  +6/-1   src/include/lwip/udp.h                    struct udp_pcb 增加 lane 字段
  +2/-2   src/core/timeouts.c                       定时器按 lane 分片
  +1/-0   sources.mk                                登记 tcp_cubic.c
```

引入这些改动的 A20OS 提交：

| 提交 | 说明 |
|---|---|
| `a06f4575` | 首次引入网络栈（lwIP 初次 vendoring） |
| `74578420` | ABI 文件拆分 |
| `f773b0aa` | 重新 vendoring：210 文件一次性引入（用户态 → 内核） |
| `8e466330` | LS2K1000 移植，`pbuf.c` +15 行板级诊断 |
| `7a0966c0` | lane stage B：按 lane 分桶 lwIP PCB 链表 |
| `b1bb28b5` | 按 lane 分片 TCP 快/慢定时器 |
| `16304db8` | 修 lane 桶中 TCP pcb 双重索引移除 |

### 2.2 引入的独有概念

`pcb_lane.h` 定义了上游不存在的概念：`NET_PCB_LANE_BUCKETS`、
`NET_PCB_LANE_WILDCARD`、以及把 4 元组哈希到 lane 的 `net_lane_of()`。
`struct tcp_pcb` / `struct udp_pcb` 增加了 `lane` 字段。

**这不是"补丁级"改动，而是数据结构级改动。** 这一点必须让任何读代码的人知道：
本目录下的 TCP/UDP 链表组织方式与上游不同，不可按上游文档推断行为。

### 2.3 我们主动裁掉的上游目录

以下上游目录/文件在本树中**不存在**，属有意裁剪（不是遗漏）：

```
src/api/          netconn + socket API（LWIP_NETCONN=0 / LWIP_SOCKET=0）
src/apps/         MQTT/SNMP/HTTP/mDNS/…（无实现，仅剩头文件，见 §1.1）
src/netif/ppp/    PPP_SUPPORT=0
src/netif/bridgeif*.c, lowpan6*.c, slipif.c, zepif.c
```

裁剪的后果：本树**无法**使用 netconn/socket API、MQTT、SNMP、HTTP、mDNS、
PPP、6LoWPAN、SLIP、Zephyr 网关。若要启用其中任一项，需要先把对应上游文件
重新 vendoring 回来，并在 `sources.mk` 中登记。

### 2.4 明确的非改动

`src/core/netif.c` **未被 A20OS 改动**（见 §2.1 的文件清单不含它；它与上游
HEAD 的 2 行差异属于 §0.1 的"上游漂移"，不是我们的编辑）。

因此 `struct netif` 仍是上游定义：单组 input/output/linkoutput 回调、无队列、
无 RSS、无多队列、无校验和/TSO 卸载。**多队列与卸载能力完全受限于上游 `netif`
抽象**——这是 lwIP 无法通过"小幅修改"解决的能力天花板。

### 2.5 CUBIC 拥塞控制（RFC 8312 核心）

上游 lwIP 2.2.2d **只有 Reno 一种拥塞控制算法**，且硬编码在
`tcp_in.c` / `tcp_out.c` / `tcp.c` 三处：`tcp_in.c` 的 ACK 分支里 `cwnd += 1 MSS
per cwnd acked`，`tcp_out.c` 的 `tcp_rexmit_fast()` 里 `ssthresh = MIN(cwnd,
snd_wnd)/2`，`tcp.c` 的 RTO 分支里 `cwnd = mss`。上游没有任何"按连接选择算法"的
机制，`TCP_CONGESTION` 在 lwIP 里根本不存在。

A20OS 新增：

| 文件 | 作用 |
|---|---|
| `src/core/tcp_cubic.c`（新增） | 算法本体：整数定点实现，无一个 float |
| `src/include/lwip/priv/tcp_cubic_priv.h`（新增） | `struct tcp_cubic_state`、定标常量、算法名解析 |
| `src/include/lwip/tcp.h` | `struct tcp_pcb` 增加 `cong_alg` 与 `cubic` 字段 |
| `src/core/tcp_in.c` | ACK 分支按 `cong_alg` 分派；`dupacks > 3` 的 Reno 加窗对 CUBIC 关闭 |
| `src/core/tcp_out.c` | `tcp_rexmit_fast()` 里的快重传降窗分派 |
| `src/core/tcp.c` | RTO 分支调用 `tcp_cubic_on_rto()` |
| `sources.mk` | 登记新文件 |

**这不是"补丁级"改动，而是算法级改动。** §2.2 的警告同样适用于此：
读本树 TCP 代码时不能按"lwIP = Reno"推断行为。开关是 `LWIP_TCP_CUBIC`，
由 `kernel/net/net_profile.h` 按资源档位给出（EMBEDDED 关，DEFAULT/SERVER 开）。

实现的 RFC 条款与**未**实现的条款，都逐条写在
`src/include/lwip/priv/tcp_cubic_priv.h` 的文件注释里。特别注意三条**不是**
上游行为的语义：

1. **快收敛（§4.6）是"减小" `W_max`**，用 `W_max * (1 + beta_cubic) / 2`，
   不是 `W_max * (1 + beta_c)`。名字有误导性：它让流在更长的时间里长得**更慢**，
   目的是给新加入的流让出带宽。Linux 的 fast-convergence 注释里那个
   `beta_c = 0.85` 出自更早的版本，RFC 8312 的伪代码用的是 `beta_cubic = 0.7`
   并带 `/2.0`。
2. **§4.5 的 `ssthresh = cwnd * beta_cubic`（即 ×0.7），不是 `cwnd * (1 - beta)`**
   （§4.1 描述的是 cwnd 被降到 `W_max * beta_cubic`，两处同为 ×0.7）。
3. **§4.7（RTO）与 §4.5（快重传）是两条不同的规则**：RTO 之后的第一次拥塞
   避免用 `K := 0`、`W_max := 当时的 cwnd`。因此 `tcp.c` 的 RTO 分支调
   `tcp_cubic_on_rto()` 而不是 `tcp_cubic_on_loss()`——走同一条路径会让连接停在
   一条锚定在已被超时甩掉的 `W_max` 上的曲线上。

**时间基准是秒。** `W_cubic` 的 `C = 0.4` 单位是 segment/s³，而本栈唯一的时钟是
`tcp_ticks`（每 `TCP_SLOW_INTERVAL` = 500 ms 一跳）。所以 `K` 与 epoch 经过时间都
先换算成 1/256 秒再进立方项。这一步漏掉不是精度损失而是**速率差 8 倍**。

**没有实现、因此不得假定的**（同样逐条列在头注释里）：§4.2 的 TCP-friendly
区域只以 Reno 速率近似，不是 Eq. 4 的 `W_est(t)`；HyStart / TCP-AQ / DCTCP /
Prague / ECN 与 RTT 方差耦合均无；`W_max` 不跨 pcb 生命周期持久化。

算法正确性由主机侧单元测试 `tools/test-tcp-cubic-host.sh`
（`tools/test-tcp-cubic-host.c`）覆盖，参考值全部按 RFC 原文的双精度公式独立算出，
不从实现反推。往返传输的 smoke 门禁**不能**覆盖这些：一条从不丢包的连接根本不
会离开慢启动，因此对本文件里那几类算错（立方根截断、定标错位、时间轴单位错、
快收敛方向反了）全部不敏感。

### 2.6 每 pcb 接收窗口上限（`wnd_limit`）

lwIP 的 `tcp_recved()` 每次应用层读完就把 `rcv_wnd` 直接补回 `TCP_WND_MAX(pcb)`
（`src/core/tcp.c` 的注释写的是 "restore the window"）。因此**在 lwIP 内部没有任何
一处可以挂一个比 `TCP_WND` 更小的常驻接收上限**——任何写进 `rcv_wnd` 的较小值
都会在下一次 `recv()` 时被抹掉。要让 `SO_RCVBUF` 成为一个真的约束而不是设置即丢，
必须给 pcb 一个上限字段：

| 改动 | 位置 |
|---|---|
| `tcpwnd_size_t wnd_limit;`（0 = 无上限） | `src/include/lwip/tcp.h` 的 receiver 段 |
| `tcp_recved()` 先取 `min(TCP_WND_MAX(pcb), wnd_limit)`，两者皆 0 时保持上游值 | `src/core/tcp.c` |

`struct tcp_pcb` 是 lwIP 的公开结构体，`wnd_limit` 属于**必须与上游同步的字段**，
和 §2.2 的 `lane` / §2.5 的 `cong_alg` 同类：合并上游时这三个字段要一起搬。

**语义边界（不要按 Linux 推断）**：

- 上限只约束 `rcv_wnd`（本地还愿意收多少），不约束 `rx_buf` 的 pbuf 数量，
  也不影响 `tcp_recved()` 之外的行为。
- 上限不能突破窗口缩放：线上字段是 `rcv_wnd >> rcv_scale`，16 位，所以有效天花板
  是 `min(TCP_WND_MAX(pcb), 0xFFFF << pcb->rcv_scale)`，超出的部分在
  `kernel/net/socket_inet.c` 的 `net_inet_tcp_buf_apply()` 里被夹掉。
- 下调立即生效（同时写 `rcv_wnd` 并重跑公告逻辑）；上调也要写，因为 lwIP 没有
  "还回去" 的机制。

## 3. 重新同步上游的流程

```sh
# 1. 记录一个上游 SHA 作为基线（一次性，需要网络）
tools/lwip-sync.sh --record /path/to/upstream/lwip

# 2. 随时查看当前差异规模
tools/lwip-sync.sh --stat

# 3. CI：基线缺失或漂移超阈值则失败
tools/lwip-sync.sh --check
```

合并上游时必须人工裁决的项（不可机械套用）：`pcb_lane.h` 及 `lane` 字段、
`TCP_REG`/`TCP_RMV` 的 lane 索引、`pbuf.c` 的 LS2K 诊断。

## 4. 已知上游安全通告

CISA ICS 2026 年通告（ICSA-26-265-01 / -02，影响 lwIP `>= 2.0.1` 至 `2.2.1`）：

| 通告 | CVSS | 组件 | 本树是否受影响 | 依据 |
|---|---|---|---|---|
| CVE-2026-87121 | 9.8 (AV:N) | `src/apps/mqtt.c` MQTT 客户端越界写 | **否** | `src/apps/` 在本树中不存在（§2.3），代码不可达 |
| CVE-2026-91018 | 8.8 | lwIP API double free | **未核验** | 需比对上游修复提交是否在 `d08f477` 内 |

**CVE-2026-91018 的核验方法**（不要靠版本号推断）：

```sh
# 取通告给出的修复提交，确认它是否为 d08f477 的祖先
git -C /path/to/upstream merge-base --is-ancestor <fix-sha> d08f477 && echo "fixed" || echo "vulnerable"
```

在该核验完成前，本文件**不声称**本树不受 CVE-2026-91018 影响。这是
`docs/security/hardening.md` 诚实性原则的直接应用：宁可缺数据，也不能让一个
未核验的结论看起来像结论。

## 5. 板级诊断代码（非上游，注意信息泄露面）

`src/core/pbuf.c` 的 `pbuf_free()` 中有一段 A20OS 自有诊断，编译条件为
`CONFIG_BOARD_LS2K1000 && CONFIG_COOPERATIVE_BOOT`：

```c
if (p->ref == 0) {
    printf("[LS2K-DIAG] pbuf_free p=%p caller=%p entry_sp=0x%lx ...\n", ...);
}
```

它在 pbuf 引用计数即将下溢时打印现场，用于定位 double-free（实测确实抓到过）。
两点需要记录：

1. 它用 `%p` / `%lx` 把内核地址写到串口。串口是特权出口，因此可接受，但**不得
   在量产配置下启用**。
2. 它是 `CONFIG_COOPERATIVE_BOOT` 的产物，在该板型之外是死代码（riscv64 /
   aarch64 / x86_64 等构建均不定义上述两个宏）。

这段代码是"外部目录里放着 A20OS 自有代码"的又一个实例，也是本文件存在的理由。
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
| **A20OS 自己的改动** | `git diff f773b0aa HEAD -- kernel/external/lwip`（`f773b0aa` = 重新 vendoring 的提交） | **9 文件,+790 / −302** |
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

**测量口径：`git diff f773b0aa HEAD -- kernel/external/lwip` = 9 文件，+790 / −302**
（`f773b0aa` 是把 lwIP 重新 vendoring 进内核的提交，作为"未改动基线"）。

### 2.1 受影响的文件（9 个）

```
+481/-222  src/core/tcp.c                            PCB 链表按 lane 分桶
                                            （+9）  补齐缺失的 LWIP_ASSERT_CORE_LOCKED()
+260/-178  src/core/udp.c                            PCB 链表按 lane 分桶
+105/-73   src/core/tcp_in.c                         lane 感知的输入查找
                                      （+2）        tcp_trigger_input_pcb_close() 补断言
 +86/-45   src/include/lwip/priv/tcp_priv.h          TCP_REG/TCP_RMV 改为 lane 索引
+112       src/include/lwip/priv/pcb_lane.h          【新增文件，上游无对应物】
 +30/-15   src/core/pbuf.c                           LS2K1000 板级诊断 printf
  +7/-1    src/include/lwip/tcp.h                    struct tcp_pcb 增加 lane 字段
  +7/-1    src/include/lwip/udp.h                    struct udp_pcb 增加 lane 字段
  +4/-1    src/core/timeouts.c                       定时器按 lane 分片
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
| 见 §2.5 | 把锁契约变成可执行：`LWIP_ASSERT_CORE_LOCKED()` 从空宏接到 `g_lwip_lock` 的持有者 CPU |

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

`src/core/netif.c` **未被 A20OS 改动**（见 §2.1 的 9 文件清单不含它；它与上游
HEAD 的 2 行差异属于 §0.1 的"上游漂移"，不是我们的编辑）。

因此 `struct netif` 仍是上游定义：单组 input/output/linkoutput 回调、无队列、
无 RSS、无多队列、无校验和/TSO 卸载。**多队列与卸载能力完全受限于上游 `netif`
抽象**——这是 lwIP 无法通过"小幅修改"解决的能力天花板。

### 2.5 `LWIP_ASSERT_CORE_LOCKED()` 的接线（本树独有）

上游 `src/include/lwip/opt.h:227` 把 `LWIP_ASSERT_CORE_LOCKED()` 定义成**空宏**，
除非移植层自己 `#define` 它。本树此前没有定义，于是 `tcp.c` / `tcp_in.c` / `raw.c` /
`udp.c` / `dns.c` / `ethernet.c` 里那几十处断言**全部是空操作**——
`docs/net/network-lock-contract.md` 里的锁纪律在运行时没有任何强制手段。

现在 `kernel/net/lwip_port/lwipopts.h` 在 `CONFIG_NET_LOCK_ASSERT=1` 下把它映射到
`a20_lwip_assert_core_locked()`（`kernel/net/lwip_stack.c`）：

- `a20_lwip_lock()` 记录持有者 **CPU id**（不是布尔量——布尔量在"别的 CPU 持有"时
  会误判为通过），`a20_lwip_unlock()` 清回 `A20_LWIP_LOCK_UNOWNED`；
- 宏里传的 `__builtin_return_address(0)` 在展开点求值，指向**未持锁运行的那个 lwIP
  函数**，panic 之前先记录下来；
- `a20_lwip_init()` 结束时才 **arm**。arm 之前断言是 no-op：lwIP 自己的
  `lwip_init()` / `netif_add()` / `netif_init()` / `dhcp_start()` 链在
  `a20_lwip_init()` 内部跑，那里根本没有 A20OS 的锁；早先的探针在引导期测到 23 次
  命中、8 个不同返回地址（`netif_init`、`netif_add`、`lwip_init`、
  `netif_add_ip6_address`、`a20_lwip_init`、`a20_lwip_loopif_init_cb`），
  对它们 panic 会让**每一个**配置启动即死。
- arm 之后一次违规即 **panic**（不是只计数）：计数型信号会被当成噪声忽略，而这条
  契约的价值恰恰在于"违反必须停下来"。

同时补上了此前**没有**断言的入口，把探针的盲区变成覆盖区（上游在此处也没有断言，
所以这些是纯新增行）：`tcp_new()`、`tcp_new_ip_type()`、`tcp_slowtmr()`、
`tcp_fasttmr()`、`tcp_netif_ip_addr_changed()`、`tcp_process_refused_data()`、
`tcp_trigger_input_pcb_close()`。`tcp_abort()` 在本树**已经**带断言
（`tcp.c:656`），`docs/net/net-lanes.md` 说它没有，那条记录已过时。

默认构建 `CONFIG_NET_LOCK_ASSERT=0`，宏仍为空，代价为零。

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
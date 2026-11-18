# 校验和卸载：架构调研与实现约束

本文分析在 A20OS 中为 virtio-net 协商 `VIRTIO_NET_F_CSUM` / `VIRTIO_NET_F_GUEST_CSUM` 所需的代码改造与潜在架构风险。

> **当前配置**：内核未启用硬件校验和卸载，亦未协商对应 feature。`kernel/drivers/net/virtio_net.c` 在探测路径中记录 `offered(csum=..)` 并拒绝协商（`virtio_net_init_instance()` 中的 `net->have_csum` / `net->have_guest_csum` 分支）。能力位 `NET_DEV_CAP_TX_CSUM_OFFLOAD` / `NET_DEV_CAP_RX_CSUM_OFFLOAD` 保持未置位状态。

---

## 0. lwIP 校验和机制与协商支持矩阵

lwIP 2.2.2 协议栈支持基于 netif 的校验和控制能力，相关机制与实测事实如下：

| 机制维度 | 实际情况 | 代码依据 |
|---|---|---|
| 校验和标志位 | 具备 `NETIF_CHECKSUM_GEN_*` / `NETIF_CHECKSUM_CHECK_*` 标志位、`netif->chksum_flags` 字段与 `NETIF_SET_CHECKSUM_CTRL()` 宏 | `kernel/external/lwip/src/include/lwip/netif.h:140-153`、`:340-342`、`:408-417` |
| 链路属性与校验和区分 | `NETIF_FLAG_*`（UP/BROADCAST/LINK_UP 等）管理链路层属性；校验和控制走专有的 `LWIP_CHECKSUM_CTRL_PER_NETIF` 编译开关与 `netif->chksum_flags` | `netif.h:84-107` 对比 `:140-153` |
| 内存拷贝与校验和计算 | `pbuf_take()` 仅执行内存拷贝（`MEMCPY`）；`LWIP_CHECKSUM_ON_COPY` 仅控制应用缓冲至 pbuf 拷贝时的附加累加，不影响接收侧校验逻辑（由 `CHECKSUM_CHECK_*` 在 `tcp_input()` / `udp_input()` 中执行） | `src/core/pbuf.c:1254-1295` 对比 `src/core/tcp_in.c:166-177`、`src/core/udp.c:419-450` |

**架构取舍**：虽然 lwIP 2.2.2 支持该机制，但默认未定义 `LWIP_CHECKSUM_CTRL_PER_NETIF`（`opt.h:2371-2373`，默认 0）。启用该特性需要侵入修改 lwIP TCP 发送路径（`tcp_out.c` 中校验和计算为无条件执行），且与 NAT 模块的原地校验和修正存在同步冲突。因此当前维持软件全量校验策略。

## 1. 现状：栈与驱动各自做什么

### 1.1 接收

`kernel/net/backends/lwip/lwip_stack.c:1165` 从驱动 `recv()` 拿到一块**扁平帧**，经`netfilter_input()` 后 `pbuf_alloc(PBUF_RAW, len, PBUF_POOL)` 再 `pbuf_take()`拷进 pbuf，然后交给 `netif->input()`。驱动到协议栈之间**只有一个返回长度**，没有任何 "这个帧的校验和设备已经验过了" 的通道。

随后 lwIP 无条件验三层校验和（编译期全开，见 §1.3）：

| 层 | 位置 | 失败后果 |
|---|---|---|
| IPv4 头 | `src/core/ipv4/ip4.c:531-546` | 丢包 + `ip.chkerr` |
| UDP | `src/core/udp.c:419-450` | 丢包 + 计数 |
| TCP | `src/core/tcp_in.c:166-177` | 丢包 + `tcp.chkerr` |

### 1.2 发送

`a20_lwip_linkoutput()`（`kernel/net/backends/lwip/lwip_stack.c:340`）把 pbuf 拷进`st->tx_frame`（或走 `send_sg` 快路径），经 `netfilter_output()` 后交给`ops->send()` / `ops->send_sg()`。驱动侧 `virtio_net_send_iov()` 把`virtio_net_hdr` 区域**清零**后填帧（`virtio_net.c:803-810`），`hdr.flags = 0`，即从不声明 `VIRTIO_NET_HDR_F_NEEDS_CSUM`。

lwIP 这边校验和已经**全部算完**并且写进了帧里：

| 层 | 位置 | 是否受 per-netif 开关控制 |
|---|---|---|
| IPv4 头 | `src/core/ipv4/ip4.c:983-999` | **是**，`IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_IP)` |
| UDP | `src/core/udp.c:942-960` | **是**，`IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_UDP)` |
| TCP | `src/core/tcp_out.c:1587-1596` | **否。** 最后的 `seg->tcphdr->chksum = ~FOLD_U32T(acc)` 无条件执行 |

TCP 这一行是本次调查最要紧的发现：**打开 `LWIP_CHECKSUM_CTRL_PER_NETIF` 并给 netif 置上 `NETIF_CHECKSUM_GEN_TCP`，TCP 的校验和仍然会被完整算出来**，因为 lwIP 的 TCP 发送路径没有把这个位接进去。要真的卸载，必须改 lwIP 的 TCP 输出，而不只是打开一个开关。

### 1.3 编译期现状

`kernel/net/backends/lwip/port/lwipopts.h` 里**没有任何 `CHECKSUM*` 定义**，全部取上游默认：

| 选项 | 默认 | 位置 |
|---|---|---|
| `CHECKSUM_GEN_IP` / `_UDP` / `_TCP` / `_ICMP` | 1 | `opt.h:2379` / `:2386` / `:2393` / `:2400` |
| `CHECKSUM_CHECK_UDP` / `_TCP` / `_ICMP` / `_ICMP6` | 1 | `opt.h:2421` / `:2428` / `:2435` / `:2442` |
| `LWIP_CHECKSUM_CTRL_PER_NETIF` | **0** | `opt.h:2371-2373` |
| `LWIP_CHECKSUM_ON_COPY` | 0 | `opt.h:2449-2450` |

## 2. 要真正启用，各需要什么

### 2.1 发送侧（`VIRTIO_NET_F_CSUM`）

设备侧是通的：QEMU 收到 `VIRTIO_NET_HDR_F_NEEDS_CSUM` 的 TX 头会在主机上补算校验和（外部 Debian QEMU 10.0.13+ds 源树 `qemu-10.0.13+ds/hw/net/net_tx_pkt.c:833-838`；来源与解包定位见[外部 QEMU 源码定位](../dev/external-dependencies.md)）。缺的是**栈 → 驱动**的那一段：

| 需要的改动 | 位置 | 性质 |
|---|---|---|
| 打开 per-netif 校验和控制 | `opt.h:2371-2373` 或 `lwipopts.h` 定义 `LWIP_CHECKSUM_CTRL_PER_NETIF=1` | **改上游文件**（opt.h 是上游的；也可以只在本树 `lwipopts.h` 里定义，不动上游，但那样就不是"分歧"而是"配置"——见 §3） |
| lwIP 在 `netif->linkoutput` 之前把 L4 校验和降级成伪首部部分和 | `src/core/tcp_out.c:1587-1596`（TCP）、`src/core/udp.c:942-960`（UDP） | **改上游文件**：TCP 那处没有任何 `IF__NETIF_CHECKSUM_ENABLED` 包裹，必须新增 |
| 一个把 `csum_start` / `csum_offset` 从栈传给驱动的通道 | `net_dev_ops_t::send` / `send_sg`（`driver_class.h:178`、`:201`）目前只有 `(dev, pkt, len)` | 新增带逐帧元数据的独立原生 TX 协议可保留旧表；若改变 `net_iovec_t` 或 primary net 表布局，仍须升模块 ABI |
| 驱动据此填 `hdr.flags = VIRTIO_NET_HDR_F_NEEDS_CSUM`、`gso_type`、`csum_start`、`csum_offset` | `kernel/drivers/net/virtio_net.c:803-810` | 本树内 |
| netif 上置 `NETIF_CHECKSUM_GEN_IP/UDP/TCP` | `kernel/net/backends/lwip/lwip_stack.c` 建 netif 处 | 本树内 |

### 2.2 接收侧（`VIRTIO_NET_F_CSUM`）

设备侧**在本机可用的 QEMU 上不可用**，这是比栈侧更硬的一道墙：

- `VIRTIO_NET_HDR_F_DATA_VALID`（"校验和有效"）这个位在外部源树**只有定义、virtio-net 从不使用**：`qemu-10.0.13+ds/include/standard-headers/linux/virtio_net.h:132` 定义了它，`qemu-10.0.13+ds/hw/net/virtio-net.c` 全文**没有出现过**（`hw/net/e1000e_core.c:1187`、`hw/net/igb_core.c:1379`、`hw/net/vmxnet3.c:884` 才在用）。也就是说 QEMU 10.0 的 virtio-net 即使协商了 CSUM，也不会逐帧告诉驱动"这帧验过了"。来源与解包定位见[外部 QEMU 源码定位](../dev/external-dependencies.md)。
- lwIP 侧的表达是**每 netif 一个位**（`netif->chksum_flags`），不是每 pbuf 一个位。打开 `NETIF_CHECKSUM_CHECK_TCP` 就等于"这个 netif 的每一帧都别验了"。在 virtio 上这个假设只有在驱动能逐帧判断时才成立，而驱动判断不了——RX 头里唯一的逐帧信息（`flags`）当前驱动**根本没读**（`virtio_net_recv()` 只读 `num_buffers`）。

要在 QEMU 上验证接收卸载，最小的诚实做法是：驱动读 RX 头的 `flags`，把"设备说有效"逐帧传下去；栈侧则需要**逐帧**的入口（本树没有这个字段，需要在`struct pbuf` 或 `struct netif` 之外新增一条 per-frame 通道，或在 lwIP 里加一个`PBUF_FLAG_NOCHECKSUM` 之类的位——**这一项无论如何都是改上游 `pbuf.h`**，因为 `PBUF_FLAG_*` 的语义由 lwIP 的 `pbuf_free()` / `pbuf_take()` 使用者共同定义）。

## 3. 风险清单（按严重度排序）

1. **静默损坏（最严重）。** 只要协商了 CSUM 却不逐帧传递验证结果，lwIP 就会去验一个设备没算的校验和：**每一个包**都被丢，表现为"网络完全不通"，而计数器只显示`rx_drops`，没有任何 "checksum feature misused" 的线索。这正是两个能力位一直不置位的理由。
2. **NAT 与发送卸载互斥。** `netfilter_rewrite.c` 的 `netfilter_set_addr` / `netfilter_set_port` 对 L4 校验和做**增量修正**（改地址/端口后按 one's complement 折叠）：同文件中的 `netfilter_csum_delta()`计算差量，`netfilter_fix_l4()` 修正 L4 地址校验和，`netfilter_set_port()` 修正端口校验和。若校验和字段此时是"部分和"而不是完整校验和，增量算出来的值没有意义——NAT 表里的连接全部发出去就是错包。启用发送卸载必须同时决定：NAT 之后由谁补完校验和（设备？栈？还是禁止带 NAT 的 netif 卸载？）。
3. **TCP 路径不会被开关关住。** 见 §1.2：`tcp_out.c:1587-1596` 无条件写完整校验和。只打开 `LWIP_CHECKSUM_CTRL_PER_NETIF` 会得到"UDP 卸载了、TCP 没卸载"的半吊子状态，比全不卸载更难排查。
4. **`CHECKSUM_CHECK_IP` 的语义会变。** `NETIF_CHECKSUM_CHECK_IP` 关掉后，`ip4.c:531-546` 不再验 IPv4 头校验和。IPv4 头校验和错误在真实网络上通常是丢包，但它同时是**分片重组与地址错误的第一道过滤**，关掉它等于把这条信息全部推给上层。
5. **接口与 ABI。** 独立原生协议按 ID/version/实际大小发布，可新增逐帧 TX/RX 元数据而保留旧 primary 表；它仍要求 stack 与 provider 共同实现，不能只设置一个能力位。若直接改变 `net_iovec_t` 或既有 net 表布局，须提升模块 ABI 并重建对应包；当前 `A20_DRIVER_ABI` 为 12（`kernel/include/drivers/driver_descriptor.h:47`）。协议机制见[核心模型](../drivers/guide/core-model.md#原生协议组合能力而非扩大全局-vtable)。

## 4. 结论

- **不启用，两半都不启用。** 决定不变。
- 理由从"lwIP 没有这个 flag"更正为：**flag 有，但（a）TCP 的发送路径没有接入 per-netif 控制，需要改上游 `tcp_out.c`；（b）QEMU 10.0 的 virtio-net 不逐帧上报`DATA_VALID`，接收侧在本机根本无法验证；（c）启用会与 NAT 的原地校验和增量修正直接冲突。**
- 落地顺序（若将来要做）：先发送侧 + 关闭 NAT 组合 → 再接收侧，前提是换一台会设`DATA_VALID` 的设备或换一个 netdev。
- 登记的改动、动机与风险同时记录于 `kernel/external/lwip/DIVERGENCE.md` §2.11，作为**意向**而非已实施的差异。

## 附：证据索引

| 断言 | 证据 |
|---|---|
| per-netif 校验和位存在于 vendored lwIP 2.2.2 | `kernel/external/lwip/src/include/lwip/netif.h:140-153` |
| 该机制被编译开关关着 | `opt.h:2371-2373`（`LWIP_CHECKSUM_CTRL_PER_NETIF 0`）；`netif.h:408-417` 展开为空语句 |
| 本树未覆盖任何 CHECKSUM 默认值 | `kernel/net/backends/lwip/port/lwipopts.h` 全文无 `CHECKSUM` 字样 |
| `pbuf_take()` 与校验和无关 | `src/core/pbuf.c:1254-1295` |
| 接收侧三层校验和 | `src/core/ipv4/ip4.c:531-546`、`src/core/udp.c:419-450`、`src/core/tcp_in.c:166-177` |
| 发送侧 TCP 校验和不受开关控制 | `src/core/tcp_out.c:1587-1596` |
| 驱动清零 vnet 头，从不声明 NEEDS_CSUM | `kernel/drivers/net/virtio_net.c:803-810` |
| 驱动 RX 只读 num_buffers，不读 flags | `kernel/drivers/net/virtio_net.c:929-947` |
| QEMU virtio-net 不用 `DATA_VALID` | 外部 Debian QEMU 10.0.13+ds：`hw/net/virtio-net.c` 全文无该宏；定义在 `include/standard-headers/linux/virtio_net.h:132`，来源见[外部 QEMU 源码定位](../dev/external-dependencies.md) |
| QEMU 会补算 TX 校验和 | 外部 Debian QEMU 10.0.13+ds：`hw/net/net_tx_pkt.c:833-838`，来源见[外部 QEMU 源码定位](../dev/external-dependencies.md) |
| NAT 增量修正 L4 校验和 | `kernel/net/filter/netfilter_rewrite.c`（`netfilter_set_addr` / `netfilter_set_port`） |
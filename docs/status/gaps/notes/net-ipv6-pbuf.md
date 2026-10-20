# x86_64 桌面：lwIP IPv6 收包路径 pbuf 引用计数被破坏

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[NET-09](../net.md)。

> **当前状态（先读这一段；下面是调查过程，含已被推翻的中间结论）**
>
> 1. 找到并修掉了一个真的双重释放，位置在 raw socket 接收回调
>    （`kernel/net/socket_inet.c` 的 `raw_recv`）：它先 `pbuf_free(p)`，然后返回 0。
>    lwIP 把这个返回值读作"这个包已被吃掉"的标志，非零才表示回调接管了所有权；
>    返回 0 等于宣称自己没动，于是调用方又释放了一次。`raw_recv` 同时注册在 AF_INET 与
>    AF_INET6 的 `SOCK_RAW` 上。这是 send 侧那个已修双重释放的接收侧镜像。
> 2. 但它是否是本 panic 的触发原因，未证明。修掉它不等于解释了这个 panic。
> 3. IPv6 专属这个相关性仍未解释。唯一能对得上的方向是：同一条路径上
>    引导期 IPv6 流量（RS/NS/NA/DAD、MLD）的量远高于 IPv4，足以把一个
>    潜伏的竞争暴露出来，而 IPv4 负载把它盖住了。**这是解释，不是证明。**
> 4. "真正的重复释放 vs 相邻缓冲区越界写"这对假设，已经用测量判开了，
>    结论与此前记录的方向相反：见下方"已定案"小节。**成因是 pool 越界写，不是重复释放。**
> 5. ~~下一步：用 `CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1` 构建以抓第一个失衡。~~
>    已做，并且它一次性给出了判据（下方）。该选项仍默认关闭。
>
> **已定案（`CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1`，x86_64 + KVM 桌面回归，
> `a96134c2` 之后）**
>
> 开 canary 后同一场景不再报 `pbuf_free: p->ref > 0`，而是报：
>
> ```
> ========== KERNEL PANIC ==========
> lwIP assertion failed: detected mem underflow in pool PBUF_POOL
> [PANIC] backtrace:
>   [0] mem_overflow_check_raw
>   [1] do_memp_malloc_pool_fn
>   [2] pbuf_alloc
>   [3] a20_lwip_process_netif_rx_tx_locked
>   [4] ethernet_output ... [IRQ] ... idle_loop
> ```
>
> 三件事因此确定：
>
> 1. **是 pool 越界写，不是重复释放。** 重复释放只会触发 `p->ref > 0`；
>    canary 完好时它就该那样报。它报了 underflow，说明在断言触发之前，
>    某个 pool 元素前面的受限区已经被改写。
> 2. **是 underflow（越界写到元素之前），不是 overflow（之后）。** pool 是
>    `struct pbuf` 的连续升址数组，所以"写到 N 号元素之前"落进的是
>    N-1 号元素的尾部。即：破坏源缓冲区写过了它的低端，其越界末端
>    正好落在前一个 pbuf 上。
> 3. **发现在分配路径上。** 崩溃点是 `pbuf_alloc` 取新元素时检查到上一个
>    元素已被污染，说明破坏发生在这次分配之前，与 IPv6 解析无关。
>    IPv6 只是流量形态；它把这条路径的包量和包长分布推到了会踩中的区间。
>
> **首要嫌疑（机制已查清，尚未证实触发）**：`ip6_frag.c` 的 IPv6 重组助手。

`IPV6_FRAG_COPYHEADER 1` 是 A20OS 在 `lwipopts.h` 里唯一一处偏离 lwIP 默认值的 IPv6 配置，
其自带注释就写明「64-bit targets cannot fit lwIP's IPv6 reassembly
helper into IP6_FRAG_HLEN」。开启后：

- `IPV6_FRAG_REQROOM = sizeof(struct ip6_reass_helper) - IP6_FRAG_HLEN`
  = **12 − 8 = 4**（64 位下 `struct pbuf *` 占 8 字节）；
- `ip6_frag.c:415` 用 `pbuf_header_force(p, 4)` 把 payload 指针往回挪 4 字节，
  就地覆盖片外扩展头；
- 而 `pbuf_add_header_impl()` 的越界检查只对连续型 pbuf 存在
  （`payload < p + SIZEOF_STRUCT_PBUF`）。非连续型 pbuf 走 `force` 分支时
  完全不做检查，直接 `payload - 4`；
- 注释声称「This cannot fail since we already checked when receiving this fragment」，
  但那个「already checked」是 `ip6_frag.c:289` 的
  `p->len >= sizeof(struct ip6_frag_hdr)`：它只验证片头往后放得下，
  完全没有验证前面有没有 4 字节可借。

若该片落在非连续 pbuf（例如 PBUF_POOL 链上的元素）上，这 4 字节就直接写进
前一个元素的 payload 尾部 / `struct memp` 空闲链表指针，与 canary 报出的
「underflow in pool PBUF_POOL」完全吻合（underflow 与前一个元素的 overflow 是
同一处物理写坏，只是被哪一侧的检查先发现）。

这条假设能同时解释全部四个观察：仅 IPv6（`ip6_frag` 是 IPv6 专属路径）、
写坏 pool、underflow 而非 overflow（往 payload 之前写）、
在分配路径被发现。

**已证伪（保留记录）**：上面的 `ip6_frag` 假设经实测不成立。以
`LWIP_IPV6_FRAG=0`（其余配置不变、canary 保持开启）重建并运行 xfce 桌面，
panic 原样复现：

```
lwIP assertion failed: detected mem underflow in pool PBUF_POOL
[3] a20_lwip_process_netif_rx_tx_locked
[2] pbuf_alloc
[1] do_memp_malloc_pool_fn
[0] mem_overflow_check_raw
```

与开启分片时的栈完全一致，因此写坏内存的不是 IPv6 分片重组。
附带一条观测：该次 panic 出现在启动期（elogind 刚起来，桌面未起），
说明损坏在首次网络活动时就已经存在，而不是桌面负载才触发。

**canary 语义的精确含义**（`memp.c:130` 传入 `payload = element + MEMP_SIZE`，
即 `struct memp` 之后的 8 字节对齐偏移）：

- BEFORE 保护区 = `[payload - 16, payload)`，其中 8 字节落在前一个元素的
  payload 尾部，另外 8 字节是本元素的 `struct memp` 空闲链表指针；
- 保护区在 free 时填 `0xcd`、alloc 时校验，所以 alloc 期报错意味着
  「该元素被 free 之后，仍有代码往它的 payload 起始处回写」。

**二分结果（canary 全程开启，均为真负）**：

| 配置 | 结果 |
| --- | --- |
| `LWIP_IPV6=0` | **无 panic**，桌面正常启动并跑满 400s 超时 |
| `LWIP_IPV6_FRAG=0` | panic 复现，栈不变 |
| `LWIP_ND6=0` | panic 复现 |
| `LWIP_IPV6_DHCP6=0` | panic 复现 |
| QEMU 不挂 `-device virtio-net-pci` | **无 panic**，桌面跑满 300s 超时 |

所以损坏必须有 IPv6 才发生（IPv6 是必要条件），但与分片重组无关，
也与邻居发现无关。`LWIP_IPV6=0` 这一档同时给出了修复方向的判据：任何
最终修法都应当能在保留 IPv6 的前提下成立，而不是关掉 IPv6 绕过。

**配置二分已彻底用完**：剩下的候选无法再用编译期开关排除。

| 尝试 | 结果 |
| --- | --- |
| `LWIP_ICMP6=0` | 编译失败：`ip6.c:780` 无条件调用 `icmp6_param_problem()` |
| `LWIP_RAW=0` | 编译失败：`lwip_stack.c:470` 使用 `MEMP_RAW_PCB` |

**当前最强嫌疑：`ip6_input:1054` 忽略了 `pbuf_add_header_force()` 的返回值。**

```c
#if LWIP_RAW
  pbuf_add_header_force(p, hlen_tot);   /* 返回值被丢弃 */
  raw_status = raw_input(p, inp);
```

它同时满足此前所有观察：位于 `ip6_input`（仅 IPv6）；对每个 IPv6 包都会
执行（因此「首次网络活动即损坏」，不需要桌面负载）；且在 RX 得到的
`PBUF_POOL` pbuf 上，这次 force 必然静默失败。pool 的 payload 紧贴
`struct pbuf` 尾部，回退 1 字节就越过
`payload < p + SIZEOF_STRUCT_PBUF` 这道检查，于是 force 什么都不做，
而代码却当作成功继续执行；此时 `ip_data.current_ip_header_tot_len` 已被写成
`hlen_tot`，`raw_input()` 拿到的是一个 payload 布局与 `hlen_tot` 不符的 pbuf。

A20OS 侧确实存在 raw socket（`kernel/net/socket_packet.c`），且同一区域此前
修过一次双重释放，与「pool 元素被写坏」的症状方向一致。

**进一步推论（重要）**：`raw_input()` 在没有任何匹配的 raw pcb 时会提前返回，
什么都不做。也就是说，仅靠 `ip6_input` 这一行不足以致害，必须同时存在一个
绑定了该协议的 raw socket。这正好把嫌疑引向 A20OS 自己的代码：
`net_packet_rx_defer()` 会在每个收到的帧上喂一次 raw socket 通道，
与「首次网络活动即损坏」的现象吻合。若该推断成立，真正的越界写发生在
`kernel/net/socket_packet.c` 消费 raw pbuf 时按（被 force 失败弄脏的）
IP 头去算长度的那一段，而不在 lwIP 内。

因此下一步 instrument 应当同时覆盖两处：`ip6_input:1054` 的 force 失败分支，
以及 `socket_packet.c` 里 raw 接收的长度计算。只看 lwIP 侧可能看不到越界写。

**已证伪**：`ip6_input:1054` 这条线索经实测不成立。按其机制改写
（仅在 force 成功时才执行 undo）并以 canary 重建后，panic 原样复现，
栈帧 0~3 完全一致（仅 `[3]` 偏移因代码布局变化从 `+0x15c` 变为 `+0x16b`）：

```
lwIP assertion failed: detected mem underflow in pool PBUF_POOL
  [0] mem_overflow_check_raw   [1] do_memp_malloc_pool_fn
  [2] pbuf_alloc               [3] a20_lwip_process_netif_rx_tx_locked+0x16b
```

该改动已回退，vendored lwIP 保持未打补丁状态。

顺带更正上一条提交里的一个错误推论：`raw_input()` 在没有匹配 pcb 时确实会
提前返回，但它返回的是「未吃掉」，因此 `pbuf_remove_header` 仍会执行。
也就是说这个缺陷不需要存在活动的 raw socket 就会触发，先前「必须同时有
raw socket」的推断是错的。

**一条尚未解释的观测**：栈帧 `[4]` 是 `ethernet_output+0x13f2ae827`。
偏移量约 5.4×10⁹，不可能是任何函数的合法偏移，说明符号化落到了最近的
前驱符号上，即该帧的返回地址无法解析。若这是真实的栈损坏而非 unwinder
的缺陷，则越界写的目标可能不止 pool，还波及到了内核栈。这与「1~8 字节溢出」
的判断并不矛盾，但目前无法区分。

**仍未定案**：到底是哪一个缓冲区越界写了。下一步应在该处加
instrumentation，检查 force 的返回值，在失败分支打印 `p->payload` 实际地址
与 `hlen_tot`，直接确认 `raw_input` 是否被喂了错位的 pbuf。

### 本轮（`feat/graphics-3d-completion`）新证据：篡改规模被实测推翻，且拿到一个可用的规避

**1. 重组假设两半都已排除。** 上文只验证过 `LWIP_IPV6_FRAG=0`（**发送**方向
分片）。本轮补测 `LWIP_IPV6_REASS=0`（**接收**方向重组），panic 依旧；`ip6_frag.c`
的重组助手这条线不再是候选，写坏内存的是别的 IPv6 路径。

**2. 篡改规模比上文推断的大得多，而且写进去的是指针。** 临时给 `mem.c` 的
`mem_overflow_check_raw()` 加了转储（已回退），实测越界写入的内容是：

```
guard[16]  = e07d40000080ffff 48b42f010080ffff
           = 0xffff800000407de0  0xffff8000012fb448   ← 两个内核虚拟地址
pbuf[0..15] = 0600000000000000 98d281000080ffff
```

两处修正上文：

- 写坏的**不止 1~8 字节**：整个 16 字节 guard 区**全被覆盖**，首个被查出的偏移是
  `-16`，即紧邻元素那一端的最后一个字节。
- 覆盖进去的是**两个相邻的内核虚拟地址**，不是零散包数据。这排除了「拷贝多了
  几个字节」这一类解释，指向**某个含指针字段的结构体被写在偏移错误的位置**，
  或某个 pbuf 的 `payload` 指针算错后经它写入。

**3. 排除项。** `memcpy`（`core/string.c`）只会前进且严格写 `n` 字节，无法写到
目标之前；`pbuf_add_header_impl()` 对 `PBUF_POOL`（该类型带
`PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS`，payload 紧贴 `struct pbuf`）的越界检查是
**有效**的，不是先前推测的「空检查」。

**4. 可用的规避（已实测；是规避，不是修复）。** `LWIP_IPV6=0` 时，带
`virtio-net-pci` 的 xfce-x86_64 桌面**连续运行 9 分钟无 panic**，IPv4 正常
（`netif en2 ... ip=10.0.2.15 gw=10.0.2.2`），帧持续 present，桌面可用。代价是
**整个 IPv6 协议栈关闭**（无链路本地地址、无 RA/RS、无 MLD/DHCPv6）。这是产品
取舍，**本轮未擅自提交**，留待决策。

**5. 下一步为什么需要新工具。** 本轮每次桌面启动约 4 分钟，且 panic 处栈回溯不可信
（帧 `[4]`、帧 `[8]`~`[11]` 符号落在 `ethernet_output` 上但偏移是 `0x7f9xxxxx`
量级的垃圾值），靠增加启动次数做二分已不可行。要定位那个写坏内存的指针，需要
**确定性的用户态复现器**（guest 内构造 IPv6 报文直接打 `ip6_input`），或在
`MEMP_PBUF_POOL` 上加硬件 watchpoint——本内核目前**没有** watchpoint 设施
（`x86_64` 只实现了 ptrace 的 regset，`trap.S` 未处理 `#DB`）。

尚未排除的 IPv6 专属面：

- `ip6.c` 输入路径与扩展头处理（`pbuf_remove_header` / `pbuf_unchain` 链式搬移）；
- ICMPv6 中非 echo 的路径（echo 应答已确认走 `PBUF_RAM`，故非 echo 类）；
- IPv6 的 `netif` / 地址层。

完全不挂 virtio-net 也不复现，说明确实必须有一条活的 RX 数据面在喂包，
排除「与网络无关的启动期内存踩踏」。

分片重组、邻居发现、DHCPv6 三条「IPv6 专属且常驻」的后台路径已全部排除，
剩下的多半就在 `ip6_input` 自身的扩展头解析/搬移里。逐个再关子系统
的收益开始下降，建议改为直接在 `ip6_input` 的扩展头循环里对
`pbuf_remove_header` / `pbuf_unchain` 加定位 instrumentation，
用 canary 命中时的 `p->payload` 地址反推真正的越界写点。

结论不变但更精确：存在一处 **1~8 字节的溢出**，写穿某个 1536 字节
`PBUF_POOL` payload 的尾部，落在下一个元素的头部。

**仍未定案**：到底是哪一个缓冲区越界写了。canary 只给出方向（低端越界、
落在 PBUF_POOL 相邻元素），不给身份。下一步应从 pool 元素尺寸与
`PBUF_POOL_BUFSIZE`（1536）的边界关系入手，找哪个子系统按 1536 以上的
步长写入 pool 附近内存；`MEMP_OVERFLOW_CHECK` 无法回答这个问题，
需要在 canary 命中的那对元素上打印其地址/尺寸/相邻关系。
>
> **仍未被推翻的既有事实**：lwIP 除一处有板级保护的 printf 外未打补丁；
> RX 接缝每帧新分配一个单元素 pool pbuf，且 virtio_net 同时夹紧 `pkt_len` 与
> `used_len`，因此接缝处一帧的 pbuf 不可能被双重释放；出问题的那次释放
> 发生在更早的包上。

- 症状：x86_64 桌面起来后约 1 分钟不确定性地打死内核，报
  `lwIP assertion failed: pbuf_free: p->ref > 0`；调用链为
  `ip6_input` → `ethernet_input` → `a20_lwip_process_netif_rx_tx_locked`。
  桌面会话因此活不过约 55s。这条是既有问题，rebase 到当前 main 后依旧复现。
- 已修（调用侧，真实缺陷）：`kernel/net/lwip_stack.c` 的 RX 循环在 `netif->input()`
  返回错误时又 `pbuf_free(p)`，但 lwIP 的 `ethernet_input` 在自己的错误路径上
  「先释放再返回 `ERR_OK`」（源码注释：so the caller doesn't have to free it again），
  这是对已移交所有权的 pbuf 二次释放。已修（`687f8271`）。
- 已修（可二分性）：`kernel/net/socket_inet.c` 的 UDP/raw 收包回调原先无条件调用
  `ip6_current_dest_addr()` / `IP6H_HOPLIM()` 等，导致 `LWIP_IPV6=0` 根本编不过，
  IPv6 路径无法单独二分。已加 `#if LWIP_IPV6` 保护（`0154a7fc`）。
- 已定位到 IPv6 收包路径：`LWIP_IPV6=0` 时桌面稳定存活 250s+、零 panic。
- **关键坑（务必注意）**：想「只关一个开关」时，我把 `LWIP_IPV6_DHCP6` 与
  `LWIP_IPV6_AUTOCONFIG` 关掉、其余 IPv6 全留，也不复现，但那是因为
  guest 根本没拿到 IPv6 地址，IPv6 收包路径几乎没被触发（日志里仅 1 处 IPv6、
  无任何地址行）。所以这条只是混杂变量的阴性结果，不能据此判定
  DHCPv6/SLAAC 无罪。真正结论是：**panic 需要真实 IPv6 收包流量才会发生。**
- 已排除（逐一验证）：
  - `g_lwip_lock` 确实在 IRQ 路径（`kernel/drivers/net/virtio_net.c:554`）与 poll 路径都持有，RX 循环本身已串行化；
  - `net_packet_rx_defer()` 在自旋锁下 `memcpy` 拷贝帧，不会保留共享 `rx_frame` 指针；
  - vendored lwIP 的重组路径已审：`ip6_reass_free_complete_datagram()` 与
    `ip6_reass()` 完成时的 `pbuf_cat` 链接逻辑均与上游所有权约定一致，未见缺陷。
- 曾判定为"真正的重复释放，不是内存踩坏"，这条判定不成立，见下方"更正"。
  当时的做法是在 `pbuf.c` 里临时记录已释放地址、重新分配时抹掉记录，并在断言前
  比对，从而试图把两种成因分开（`pbuf_free: p->ref > 0` 只能证明 `ref == 0`，
  单看断言无法区分）。x86_64 `NR_CPUS=4` + XFCE 镜像实测输出：
  ```
  pbufdiag: DOUBLE-FREE 0xffff80000147cba8 type=0 len=0 tot=2 next=0x0
  ```
  该地址当时看起来确实被释放过。更正：这个判据在 memp 的空闲链表
  语义下是无效的；空闲链表本身就是穿过已释放块的指针链，一次双重释放之后
  同一块内存会被交给两个活着的 pbuf，于是"这块地址被释放过"对每个 pbuf 都
  成立，`ref` 也失去意义。**所以这一段插桩没有排除"相邻缓冲区越界写"，也没有
  证实"真正的重复释放"。当时的结论应当作废。** 当时被排除的"重复释放"候选
  （`pbuf_remove_header` 内部释放、`ip6_input` 返回非 `ERR_OK`）仍然是有效的
  排除，它们是逐个读实现否掉的，不依赖上述插桩。
- 关键线索是被释放的 pbuf 形态：**`len=0` 而 `tot_len=2`**（`type=0 next=0x0`）。
  `len` 归零的来源已确认：vendored lwIP 的 `pbuf_remove_header()` 不释放任何东西，
  它只把 `payload` 前移并原地减小 `len`/`tot_len`，因此当头部正好等于首个 pool pbuf
  的长度时就会留下 `len=0`、`next=0x0`、而剩余字节挂在 `tot_len` 里的形态。
- **已排除的猜测（不要重查）**：「`pbuf_remove_header` 内部释放了 pbuf，导致上层再
  释放一次」。读实现即可否掉，它只做指针/长度算术。而且该函数只处理链首
  （不遍历 `p->next`），所以链首被吃空后仍有一条 `tot_len=2` 的尾巴，形态自洽。
- **第二个猜测也已排除（不要重查）**：怀疑是「`ip6_input` 释放 `p` 后返回非 `ERR_OK`，
  再被 `ethernet_input()` 的 `if (err != ERR_OK) pbuf_free(p);` 释放一次」。实测
  `ip6_input` 只有 5 个返回点（`ip6.c:535/541/561/581/1119`），全部是 `ERR_OK`，
  根本不存在 `ERR_MEM` 出口；因此 `ethernet_input` 那条错误路径对 IPv6 永不触发。
- 目前两条最自然的 IPv6 内部路径都被排除（见上），重复释放的实际双方还没找到。
  已确认的事实：`ip6_input` 自身不返回错误；链首 pbuf 可被 `pbuf_remove_header`
  吃成 `len=0/next=0x0`；断言触发时 `ref==0`。"该地址确实曾被释放"这一条不再
  可用作证据（见上面的更正）。IPv6 专属这一点仍是最强线索；同一条
  `ethernet_input` 路径的 IPv4 分支从不触发，说明差异在 `ip6_input` 内部而非入口。
- **已做过一次 `ip6_input` 全量打点（22 处 `pbuf_free`）的实验，结果与预期不同**：
  在 x86_64 `NR_CPUS=4` + XFCE 镜像上捕获到
  ```
  ip6diag: REPEAT-FREE 0xffff80000139aa00 already in recent frees
  ip6diag:   ==> 0xffff80000139aa00
  ip6diag:       0xffff80000139aa70
  ```
  但同一轮 `pbuf_free: p->ref > 0` 断言并没有触发（0 次）。也就是说这次捕获到的
  「重复释放」不是本 bug：该 pbuf 当时 `ref` 仍 > 0（很可能是被 `pbuf_ref` 过、
  有两个所有者），两次 `pbuf_free` 本身合法。
  **教训：判重必须同时打印 `p->ref`**，只看地址重复会把正常引用计数误判成缺陷；
  任何后续检测都不要漏掉这一点，否则会得出假结论。
- 另一个观察：两个地址相差 `0x70 = 112` 字节，正好等于 `struct pbuf` 的大小，
  即它们是 pbuf pool 里相邻的两个元素。这与「越界写踩到邻居」的形态吻合，
  但仅凭地址相邻不能下结论（pool 本来就是连续分配），仍需 `p->ref` 与写入点证据。
- 下一步建议（未做）：把上面的 22 处打点保留，但记录项从「地址」扩展为
  `{地址, p->ref, 释放点行号}`，并且只在断言真的会触发的那一轮 dump 序列；
  本次实验说明不崩溃的轮次里重复信息噪声极大。复现需保持 SLAAC 可用。
- **但这套「按地址判重」的路子整体上已经不可用**，原因见顶部第 4 条（memp 的
  空闲链表穿过已释放块，一次双重释放之后地址判重与 `ref` 都会失真）。
  正确方向是 `CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1` 抓第一个失衡点，而不是继续
  在触发断言的那一轮里做模式匹配。

# 双实例 UDP 演示环境（WB2 交付）

内容已按 2026-10 源码核对。本文是 WA2（UDP 传输 + 跨机 channel_call）的**验收环境**：不依赖任何 `kernel/cluster/` 代码，只把两个 A20OS QEMU 实例接到同一条 L2 链路上并证明双向可达。对应轨道定义见 [impl-prompts.md](impl-prompts.md) 的 WB2 节；拓扑契约来自 [04-transports.md](04-transports.md) §3（"两实例各一个 socket 后端网卡互联或经 host UDP 转发，必须脚本化可重跑"）。

## 接线拓扑

```
        宿主 (127.0.0.1)
   ┌─────────────────────────────────────────────┐
   │                                             │
   │   UDP 44122                    UDP 44121    │
   │  ┌────────┐   L2 帧走 host UDP 隧道  ┌──────┐ │
   │  │ 发往 → │◄─────────────────────────│◄ 发往│ │
   │  │        │                          │      │ │
   │  ▼        │                          │      ▼ │
   │ ┌──────────────────┐        ┌──────────────────┐
   │ │  QEMU 实例 A      │        │  QEMU 实例 B      │
   │ │  virtio-net      │        │  virtio-net      │
   │ │  MAC 52:54:00:   │        │  MAC 52:54:00:   │
   │ │      12:34:01    │        │      12:34:02    │
   │ │  IP 10.0.3.2/24  │        │  IP 10.0.3.3/24  │
   │ └──────────────────┘        └──────────────────┘
   └─────────────────────────────────────────────┘
```

- **链路**：QEMU `-netdev socket` 的 UDP 模式（非 SLIRP、非 listen/connect）。每个实例 `localaddr` 绑定自己的宿主 UDP 端口、`udp=` 指向对端端口，以太网帧原样经宿主回环转发。两侧对称：无启动顺序依赖、重启不需要先拆对端。
- **为什么不是 SLIRP**：两个各自挂 SLIRP 的实例各有一个互不相通的 10.0.2.0/24，guest 之间既不可 ARP 也不可 ICMP。
- **为什么不是 listen/connect**：也可行，但 listen 方必须先起、connect 方依赖对端在听；UDP 模式完全对称，脚本幂等性更好。这是本文选型，不是对 04-§3 的偏离（两种都在规范允许集合内）。
- **为什么不是 hostfwd**：`[net].hostfwd` 只转发 TCP/UDP 端口，ICMP 不过 SLIRP，"A ping B" 这条验收命令根本到不了对端。
- **MAC 必须显式且不同**：QEMU 所有网卡的内置默认 MAC 都是 `52:54:00:12:34:56`，两个实例同段共存会 ARP 撞车。这是本次 schema 扩展加 `[net].mac` 的直接原因。
- **网段选 10.0.3.0/24**：刻意避开 SLIRP 默认网段 10.0.2.0/24，肉眼可分集群流量与 user-mode 网络流量。

## 一键复现

```bash
tools/cluster-net-up.sh                 # 构建（如缺产物）+ 起双实例 + A ping B + B ping A
tools/cluster-net-up.sh --skip-build    # 产物已就绪时跳过构建
tools/cluster-net-up.sh --keep          # 验证后保留两个 guest 供手动检查
```

脚本幂等：重跑会先杀掉上次留下的 `a20os-cluster-{a,b}` 标记进程、复查两个宿主 UDP 端口可绑定，然后从头再验。任何一步失败都以非零码退出并打印对应 guest 日志的尾部。

预期输出（2026-10 实测，本机 QEMU 10.2.1 + TCG；时间数字每次会变，行格式是稳定的）：

```
[cluster-net-up] started guests: pid 73360 (a20os-cluster-a), pid 73361 (a20os-cluster-b)
[cluster-net-up] guest pid 73360 booted, netif at 10.0.3.2 (waited 1s)
[cluster-net-up] guest pid 73361 booted, netif at 10.0.3.3 (waited 0s)
[cluster-net-up] A(10.0.3.2) -> B(10.0.3.3): ping 10.0.3.3 3 (attempt 1)
A(10.0.3.2) -> B(10.0.3.3): 3 packets transmitted, 3 received
[cluster-net-up] B(10.0.3.3) -> A(10.0.3.2): ping 10.0.3.2 3 (attempt 1)
B(10.0.3.3) -> A(10.0.3.2): 3 packets transmitted, 3 received
cluster-net-up: PASS
date: 2026-10-06T18:55:10Z
instances: qemu-riscv64-cluster-a (10.0.3.2, MAC 52:54:00:12:34:01, host UDP 127.0.0.1:44121)
           qemu-riscv64-cluster-b (10.0.3.3, MAC 52:54:00:12:34:02, host UDP 127.0.0.1:44122)
A -> B ping: 3 packets transmitted, 3 received
B -> A ping: 3 packets transmitted, 3 received
launch parameters: .kernel-build/cluster/qemu-riscv64-cluster-a.cmdline, .kernel-build/cluster/qemu-riscv64-cluster-b.cmdline
guest transcripts: .kernel-build/cluster/a20os-cluster-a.log, .kernel-build/cluster/a20os-cluster-b.log
```

guest 串口里的逐包行（A 侧，即 `.kernel-build/cluster/a20os-cluster-a.log`）：

```
PING 10.0.3.3
64 bytes from 10.0.3.3: icmp_seq=1 time=31 ms
64 bytes from 10.0.3.3: icmp_seq=2 time=13 ms
64 bytes from 10.0.3.3: icmp_seq=3 time=10 ms
--- 10.0.3.3 ping statistics ---
3 packets transmitted, 3 received
```

B 侧镜像对称（`bytes from 10.0.3.2`，time=15/11/11 ms）。RTT 的量级（10–30 ms）是 TCG 客户端的处理开销，不是隧道本身——隧道走宿主回环。

启动参数记录在 `.kernel-build/cluster/qemu-riscv64-cluster-{a,b}.cmdline`（make 推导的完整 QEMU 命令行），两个 guest 的串口全文在 `.kernel-build/cluster/a20os-cluster-{a,b}.log`，汇总裁决在 `.kernel-build/cluster/summary.txt`。

## 手工接线（不用脚本时）

```bash
# 1. 构建一次（两个实例的构建变量相同，共享同一 BUILD_DIR）
tools/a20 build qemu-riscv64-cluster-a

# 2. 看 make 为每个实例推导的 QEMU 命令行
make -C . $(tools/a20 show-vars qemu-riscv64-cluster-a) _qemu_argv
make -C . $(tools/a20 show-vars qemu-riscv64-cluster-b) _qemu_argv

# 3. 各开一个终端直接跑这两条命令行，串口交互注入：
#    guest A:  ping 10.0.3.3 3
#    guest B:  ping 10.0.3.2 3
```

在 guest 内可用的核对命令（都在镜像里）：

| 命令 | 期望 |
|---|---|
| `ping 10.0.3.3 3`（A 上） | `3 packets transmitted, 3 received`，逐包 `bytes from 10.0.3.3: icmp_seq=N time=X ms` |
| `ping 10.0.3.2 3`（B 上） | 同上，对端地址对调 |
| `netctl` | `NETCTL: PASS`（接口枚举与 /proc/net/* 投影自检） |

内核侧证据：启动日志中每个实例有一行

```
[LWIP] netif en0 attached to <dev> ip=10.0.3.2 gw=0.0.0.0 dns_count=0
```

它来自 `kernel/net/lwip_stack.c` 的 `a20_lwip_register_netifs()`；静态地址是 `-append 'a20.dhcp=0 a20.ip=... a20.netmask=...'` 注入、由 `kernel/net/net_config.c` 解析的。脚本 `wait_boot` 就盯这一行。

## 实例配置（schema 扩展）

两个实例文件（`instances/qemu-riscv64-cluster-{a,b}.toml`）用到的字段：

```toml
[net]
mac = "52:54:00:12:34:01"                                             # NIC MAC（覆盖 QEMU 默认）
backend = "socket,id=net,udp=127.0.0.1:44122,localaddr=127.0.0.1:44121"  # 完整 -netdev 规格，替换默认 SLIRP
guest_ip = "10.0.3.2"          # → a20.ip（内核 bootargs）
guest_netmask = "255.255.255.0" # → a20.netmask
```

这些字段是本次 WB2 对实例 schema 的最小扩展（`tools/a20_instance.py` 的 `NetCfg`）：

| 字段 | make 变量 | 落点 |
|---|---|---|
| `[net].mac` | `NET_MAC` | `-device virtio-net-device,...,mac=<NET_MAC>`（Makefile net 块） |
| `[net].backend` | `NET_BACKEND` | 替换默认 `-netdev user,id=net[,hostfwd...]`；必须带 `id=net` |
| `[net].guest_ip` / `guest_netmask` / `guest_gateway` | `NET_GUEST_IP` / `NET_GUEST_NETMASK` / `NET_GUEST_GATEWAY` | `-append 'a20.dhcp=0 a20.ip=... a20.netmask=... a20.gateway=...'` |

向后兼容：全部字段可选、不写就不产生任何变量，`make check-manifests` 下全部既有 toml 语义不变。`[net].backend` 与 `[net].hostfwd` 互斥（hostfwd 只存在于被替换掉的 user 后端），`a20 check` 会拒绝组合。宿主端口预检（`a20 ports` / `a20 run` 启动前）现在同时识别 `backend` 里的 `listen=`（TCP）与 `localaddr=`（UDP）端口声明。

## 端口表

| 端口 | 归属 | 用途 |
|---|---|---|
| host UDP 44121 | 实例 A `localaddr` | A 收 L2 隧道帧 |
| host UDP 44122 | 实例 B `localaddr` | B 收 L2 隧道帧 |
| guest UDP 44020 | 实例 A（WA2 接入后） | 集群帧端口，04-§3 默认 `44020 + 实例序号` |
| guest UDP 44021 | 实例 B（WA2 接入后） | 同上 |

宿主隧道端口刻意取 44121/44122（= 44020+101/44020+102），与规范默认的 guest 集群端口错开，肉眼可分。guest 集群端口目前**没有**进实例 schema：`kernel/cluster/udp.c` 还不存在（WA2 交付物），现在加字段就是没有消费者的死配置；WA2 实现时按 04-§3"实例 TOML 可配"把它加进 `[net]` 并由 clusterd/内核消费。

## 已知的 QEMU 网络坑（WB2 实测记录）

1. **两个实例不能共享同一个根镜像**：两实例构建变量相同 → 同一 BUILD_DIR → make 的 argv 让两边都挂同一个 `fat32.img`，而 QEMU 对 raw 镜像持有**写锁**——第二个实例在引导前就死于 `Failed to get "write" lock`。`tools/cluster-net-up.sh` 因此先把镜像克隆成 `.kernel-build/cluster/fat32-{a,b}.img` 再分别挂载（构建产物本身不被污染）。任何双实例方案都要处理这一点，`a20 run` 起两个同名镜像实例会以同样方式失败。
2. **默认 MAC 撞车**：QEMU 所有网卡的内置默认 MAC 都是 `52:54:00:12:34:56`。两实例同段共存必须各自显式 `mac=`，否则 ARP 表互相覆盖，症状是 ping 时通时断。
3. **`-append` 值里的空格与 `_qemu_argv` 的单引号**：`_qemu_argv` 原来用 `printf '%s\n' '$(QEMU) $(QEMU_FLAGS) ...'` 单引号展开，`-append 'a20.dhcp=0 a20.ip=...'` 值内的单引号会提前终止 shell 引用，命令行被拆成多行，shlex 消费方（`tools/qemu.py`、`tools/a20 test`）会**静默丢掉 guest 参数**。已改为双引号展开（`tools/run-targets.mk`），不变量是"flag 值不同时含单引号与双引号"。
4. **静态 IP 必须显式关 DHCP**：UDP 隧道链路上没有 DHCP 服务器。`a20.dhcp=0` 由 Makefile 在设置 `NET_GUEST_IP` 时自动带上——不关的话 lwIP 会在静态地址之上再跑一个注定超时的 DHCP 客户端（不致命，但启动日志会混入 DHCP 噪声，且 `configured` 判定路径不同）。
5. **riscv64 的 bootargs 通路**：`-append` 写进 QEMU 生成的 DTB `/chosen/bootargs`，由 `kernel/arch/riscv64/platform/fdt.c` 的 `arch_bootargs_get()` 读出——所以 `a20.ip=` 在 riscv64 上生效，不需要 bootloader 参与。实测 guest 启动日志出现 `[FDT] bootargs='a20.dhcp=0 a20.ip=10.0.3.3 a20.netmask=255.255.255.0'` 与 `[LWIP] netif en2 attached to virtio-net4 ip=10.0.3.3 gw=0.0.0.0`。
6. **TCG 冷启动时间**：双实例同机全串口日志时，从 QEMU 起到 shell 可注入命令，实测 1–30 s 不等（首跑要建镜像缓存时更久）。脚本默认等 240 s（`A20_CLUSTER_BOOT_TIMEOUT` 可调），并对 ping 注入做最多 3 次重试——太早的命令会落在 shell 就绪前被丢弃。
7. **串口注入时机**：脚本以 `[LWIP] netif ... ip=<guest_ip>` 行为引导完成标记（`kernel/net/lwip_stack.c` 的 `a20_lwip_register_netifs()` 打印），再等 3 s 让 shell 到提示符。直接手工注入时不要按 smoke 的 8 s `input_delay` 猜——等标记行比猜时间可靠。

## 给 WA2 的接手说明

- 本环境对集群传输零假设：WA2 规划中的 UDP 传输文件 `kernel/cluster/udp.c` 尚未创建；实现后帧将走 guest 内 `socket(AF_INET, SOCK_DGRAM)` 到对端 `10.0.3.x:44020`，链路 MTU 1472 以内不分片（04-§3）。
- 演示结果（demo-echo 四路径，05-§5）按 00-design 的约定回填到本文档末尾的"验收记录"一节。
- 故障注入（丢包/分区）在 loopback 钩子上做（04-§2），不在本隧道上做；本隧道只证明"链路通"。
- `--keep` 留下的两个 guest 可直接用于 clusterd 联调；宿主端口 44121/44122 只承载 L2 隧道帧，与 guest 集群端口无冲突。

## 验收记录

（WA2/WA3 验收后回填：demo-echo 四路径结果、金样对拍结论、异常路径输出摘录。）

- **2026-10（WB2 交付）**：`tools/cluster-net-up.sh` 在本机（QEMU 10.2.1，TCG，无 KVM 参与）实跑通过——A(10.0.3.2) → B(10.0.3.3) 与 B → A 双向各 `3 packets transmitted, 3 received`（逐包行见上文）。两次发现并修掉接线缺陷：双实例写锁冲突（坑 1）与 `_qemu_argv` 单引号展开（坑 3）。集群协议面（HELLO/PING 帧）本阶段不存在也不需要——本环境只证明链路层可达。

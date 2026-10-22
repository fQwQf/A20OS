# NET 缺口

对应 V1-NET。现状见 [current/net-status.md](../current/net-status.md)、
[current/server-readiness.md](../current/server-readiness.md) §八。

### NET-01 lwIP 全局锁的最终拆分

- 现状：socket 表已分桶，阶段 C/D/E 落了 lane 上下文与按 lane 分发。
- 缺口：`g_lwip_lock` 本身仍是全局锁；`tcp_active`/`tcp_bound_pcbs`/`udp_pcbs` 的 per-lane 所有权与冷路径死锁边界未定。
- 关闭条件：热路径不再取全局 lwIP 锁；per-lane 所有权有静态/运行期断言；死锁边界有设计记录与门禁。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P0。

### NET-02 多 lane 的 2% connect flake

- 现状：797 次执行 0 失败，仍未复现、未定位、未声称已修。
- 缺口：失败样本（12404/12405）的构建号未核对，无法判断是否仍存在。
- 关闭条件：核对历史失败样本的构建号；要么定位并修复，要么给出"当前树不存在该路径"的依据。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P0。

### NET-03 IPv6 写入侧

- 现状：入站 TCP、`/proc/net/tcp6`、rtnetlink 地址写入已有。
- 缺口：地址只有加没有删（`RTM_DELADDR` 返回 `-EOPNOTSUPP`）、不做 DAD、ND6 学到的地址不发通知。
- 关闭条件：删除地址生效；DAD 行为有明确契约；ND6 通知缺失被记录为已知边界或有实现。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P1。

### NET-04 接收缓冲自动调优

- 现状：`SO_RCVBUF`/`SO_SNDBUF` 不再是 no-op，窗口缩放已解除协议上限。
- 缺口：没有 `tcp_wmem`/`tcp_rmem`、没有内存压力反馈、不从实测吞吐调整。
- 关闭条件：调优策略有实现与上限约束；依赖自动增长的调用方能拿到收益。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P1。

### NET-05 嵌入式档预算对账

- 现状：DEFAULT 档四项求和断言钉住 44 KiB；tier 2/3 的池上限仍远大于各自 `MEM_SIZE`。
- 缺口：三条预算断言只有 tier 1 有；嵌入式档仍装不进 20 KiB（已诚实声明 44 KiB）。
- 关闭条件：每个档位的池与 `MEM_SIZE` 都有求和断言；嵌入式档的声明值与实际一致。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P1。

### NET-06 pbuf 池与零拷贝收包

- 现状：窗口缩放解决了协议上限，实际瓶颈是 384 KiB pbuf 池。
- 缺口：没有扩大池容量，也没有零拷贝收包路径。
- 关闭条件：大窗口下吞吐受协议而非内存限制；或明确记录为设计边界。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P1。

### NET-07 conntrack/NAT 的剩余形态

- 现状：五元组表、NEW/ESTABLISHED、SNAT/DNAT、ICMP echo 跟踪、RFC 1624 校验和已有。
- 缺口：无 ALG；不跟踪 ICMP 差错报文（路径 MTU 探测不可用）；不做分片 NAT；端到端门禁只覆盖 DNAT。
- 关闭条件：每种形态要么实现并有门禁，要么作为设计边界记录；SNAT/MASQUERADE 有端到端场景。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八、[net/conntrack-nat.md](../../net/conntrack-nat.md)。

### NET-08 真实 netif 数据面

- 现状：socket 队列压力与本地 fast path 有覆盖；真实 lwIP/netif 与外部对端未完整验证。
- 缺口：双向 TCP/UDP、NAT 回程、外部 hostfwd 对端与耗尽/关闭竞态没有完整矩阵。
- 关闭条件：真实接口上的双向数据与外部 peer 用例通过，保留自然退出与结论。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) 优先级表。

### NET-09 IPv6 收包路径的 pbuf 引用计数破坏

- 现状：x86_64 桌面上 lwIP IPv6 收包会破坏 pbuf 引用计数；`LWIP_IPV6_FRAG=0` 方向的规避有实测依据，原先的"篡改规模"假设已被推翻。
- 缺口：根因未定位；规避不等于修复。
- 关闭条件：IPv6 收包长跑无引用计数下溢或 panic；或以明确的 IPv6 分片契约限制范围并有对应测试。
- 诊断：[notes/net-ipv6-pbuf.md](notes/net-ipv6-pbuf.md)。

### NET-10 lwIP accept 阶段的接线断言为红

- 现状：`make check-honesty-policy` 的 `lwip-concurrency-wiring` 报 `accept stage is missing listener guard, head publication, unlock, or BH schedule`。
- 缺口：这是一条 fail-closed 的反回归断言，它现在为红，意味着 accept 阶段的接线完整性无法被证明仍然成立；在候选上该 job 直接失败。
- 关闭条件：断言在候选上通过。
- 来源：DELIVERY 的 CI `toolchain-gates` job 本地复现（[current/delivery-baseline.md](../current/delivery-baseline.md) §6）。

### NET-11 TCP 发送背压用例与源码脱节

- 现状：`check-a20-tests` 的 `test_tcp_send_backpressure.test_accepted_bytes_and_retry` 在源码里找不到自己绑定的标记，直接 `ValueError: substring not found`。
- 缺口：TCP 发送背压的宿主侧行为断言当前无法区分"实现回退"与"夹具漂移"。
- 关闭条件：该用例在候选上通过，或按当前契约改写（保留行为断言与负控）后通过。
- 来源：DELIVERY 的 `check-a20-tests` 基线（[current/delivery-baseline.md](../current/delivery-baseline.md) §5）。

### NET-12 net 的 32 位内核构建不通过

- 现状：`make -k check-riscv32-bringup` 在 `kernel/net/netfilter.c:284`、
  `kernel/net/netfilter_nat.c`（6 处 `uint32_t *`/`int32_t *` 与 `unsigned int *`/`int *`
  不匹配）、`kernel/net/socket_control.c:664-666`、
  `kernel/net/netfilter_filter.rs:30`、`kernel/net/netfilter_nat.rs:33`、
  `kernel/net/socket_netlink.rs:583-584`（32 位目标没有 `AtomicU64`）报错。
- 缺口：net 的 32 位内核构建不通过。除了与 CORE-11 相同的 `int32_t`/`int` 宽度别名问题，
  `AtomicU64` 在 `max-atomic-width=32` 的目标上不可用，需要按 32 位可用的原子原语改写。
- 关闭条件：riscv32 内核构建不再有 `kernel/net/` 条目。
- 来源：MM-05 的 32 位复现（[mm.md](mm.md)），本轮 `make -k check-riscv32-bringup` 输出。

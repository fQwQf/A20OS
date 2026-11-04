# 生态纵深：历史评估与当前工程入口

本文档对系统远期演进方向与技术储备进行评估。当前工程子系统的完整交付要求见 [1.0 计划](../releases/v1.0.md)，详细缺口见 [工程清单](a20os-improvement-todo.md)。下文早期测量保留历史含义，候选验证需执行 [功能/性能对照](../releases/v1.0-validation.md)。

## 1. netfilter / 防火墙（第一切片已落地）

- 现状：已实现可用的 IPv4 包过滤（`kernel/net/filter/netfilter.c`）。hook 挂在`a20_lwip_process_netif_rx_tx_locked`（进）与 `a20_lwip_linkoutput`（出），即 `next-horizon.md` 当初评估的两个数据面点位。规则表 32 条，按方向、协议、源/目的地址、源/目的端口匹配，每条独立可通配；accept/drop 判决；每规则与全局计数器。
- 控制面：`/proc/a20/netfilter`。读为策略+计数器+规则表，写接受`add <rule>` / `del <n>` / `reset`。格式非法的规则被拒绝而非静默接受，避免一次拼写错误悄悄放宽或收紧策略。
- 启动时无规则、默认策略 accept，因此**未配置的机器行为与引入前完全一致**，不会让既有部署回归。
- 验证：`make smoke-netfilter`（`user/cmds/net/netfilter_test.c`）。除控制面外，断言匹配 drop 规则的 UDP 发送确实让 `out_dropped` 增长。实测`dropped=3 out_packets=3`，证明 hook 在真实数据面执行而非空过。
- 后续已实现 IPv4 conntrack 与 NAT，旧稿“仍缺、不能转发/转换”的结论失效。当前支持边界、回程与分片/ICMP 限制见 [conntrack/NAT](../net/conntrack-nat.md)。完成现有网络子系统还需真实双向流量、耗尽/回收、错误路径与性能验收；不能仅凭首个无状态过滤 smoke 宣布完成，也不能把未实现的网络目标直接删掉。

## 2. Native 完整 libc 与动态链接

Linux ABI 下 Alpine 动态程序和 Native 完整 libc 是不同验收范围。Native 的动态链接、线程、信号与 exec 继承按 [运行时状态](../status/current/native-abi-runtime-status.md) 和[启动协议](../native-abi/07-startup.md) 收口，纳入 V1-RUNTIME。最小 hello 只证明加载链路，不能代替完整运行时和真实应用；迁移期间同步验证既有 musl 生态不回退。

## 3. 文件系统 journal、持久化与元数据

ext4 可写 journal 和 ordered 提交/回放已有实现及 `smoke-ext4-journal` 入口，旧稿“只读回放、写 journal 尚未立项”的结论失效。当前剩余特性、格式兼容与数据保护边界见[工程清单](a20os-improvement-todo.md) 和 [一致性模型](../fs/fs-consistency-model.md)。已开展的文件系统工作纳入 V1-FS，继续完成其详细目标、失败回滚与断电恢复；VFS 的 xattr 接口存在不证明每个后端都已持久化，元数据校验也不等于文件内容校验。各项分别验收，不沿用旧稿的一两天/一周工期估计。

本页既有的 ext4 xattr 落盘与 ACL 开发目标继续保留：分别验证持久化、权限检查、复制/继承及重启后行为，先完成 xattr 存储契约再验收依赖它的 ACL，不能在改写状态时删项。

## 4. 电源与板级能力

x86_64 已有 ACPI 接入，不能再笼统写“无 ACPI”；其真实固件、IRQ 和关机边界见[x86_64 平台](../platforms/x86_64-pc.md)。这不自动证明 cpufreq、系统 suspend/resume 或 CPU 热插拔完成。已有板级工程按 V1-DELIVERY 逐板收口；新的电源管理候选先明确需求、硬件与验收再立项，不用历史评估给出当前未核实的能力或工期结论。

## 原评估中未立项或暂缓的扩展

原评估列出以下候选边界；它们不取消其他文档中已经立项的工程要求。

- 新增文件系统（原评估举 exFAT、btrfs 为例）：没有单独立项决定时不因本页自动加入 1.0。
- KASLR：评估结论见 docs/security/hardening.md（需 PAGE_OFFSET 运行时化前置重构，暂缓）。

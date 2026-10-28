# 网络分层与接收进展改造记录

分支 `codex/net-backends-router`，参照为 `4c6e6ab1`。本页记录实际执行结果，
不代表 FreeBSD 后端或软路由整体验收。

## 已执行

- 网络源码按 socket、control、filter、crypto、backends/lwip 分层，显式源码清单。
  x86_64、2 vCPU、`-O3`、UBSAN=0 内核编译通过。
- 16组socket/lwIP宿主状态机测试通过。netlink测试补齐既有路由符号测试桩，dump用例
  使用NLM_F_DUMP，短请求的拒绝用例改为真正缺失family字节，而非误拒合法短查询。
- lane队列生产代码的并发测试通过：120000帧、held-slot生命周期和CPU-local上下文，
  新增满队列信用检查；没有增加4槽默认容量。
- `check-io-progress-model` 和 `check_lwip_concurrency_wiring.py` 通过。门禁检查接收前
  容量、filter顺序、IRQ预算和主动连接重分桶后的owner发布。
- FreeBSD上游由submodule固定为 `95719d8a895bf8170851e9844d9b4092b1d2b6fe`。
  只获取源码，未链接、未作性能排名、未改变默认后端。

## 修复的实际机制

1. IRQ及轮询在取走设备帧前检查所有lane信用，满队列时留在设备中，保持pending。
2. IRQ使用有界预算；调度依次推进已有协议工作、设备输入、新协议工作和socket通知。
3. idle在仍有网络工作时不进入休眠；后台推进通过已注册netif ops，不再依赖动态
   virtio模块无法满足的静态弱链接poll函数。
4. 主动TCP/UDP connect在冷屏障内发布PCB的新owner lane。旧代码在socket归属与PCB
   不同时反复选择旧socket lane与新PCB lane，导致数据路径锁无法取得稳定归属。
5. 核心调度和启动通过协议无关的 `net/stack.h` 执行契约调用后端，不包含lwIP类型。
   当前只绑定初始网络域的lwIP执行器，INET socket私有字段的分离尚未完成。

## KVM证据

相同冻结Linux ELF、2 vCPU/2GiB、相同virtio与SLIRP、CPU4,6。3组1秒出站TCP64B、
TCP16KiB、UDP64B完成全部payload校验。此为功能预检，不是严格性能非劣性试验。

在执行器抽取前冻结的修复内核上，进一步完成3次独立启动的单端口入站：每client
至少2秒，单连接16KiB echo为12.18/13.30/13.40MiB/s，4连接父进程计时合计为
13.23/11.21/10.56MiB/s。三轮结束时目标lane累计处理75996/90584/87744帧，lane drop
均为0。保留了正常4槽容量，不是此前加32槽的诊断配置。

这修复了先前约0.008MiB/s及队列丢包的严重问题，但未证明多lane比单lane更快；单端口
仍集中在一个lane，收发复制、并发锁和协议栈本体尚需后续对照。

本机证据：`build/evidence/net-refactor-20261028/`，含路径映射、宿主日志、冻结ELF、
QEMU参数、结果manifest和完整串口。早期不完整候选的超时/接收失败原样保留，没有
作为成功样本或从最终矩阵中悄悄剔除。正式BSD/lwIP/Linux后端矩阵尚未执行。

## IPv4 FIB 与 Linux 控制工具（10月29日）

路由状态移至独立 Rust 模块，修正小端前缀掩码和 RTA_GATEWAY 常量，保存下一跳、
表号及 metric，执行源/目标策略规则。正常/嵌入式预算各4项生产FIB单元测试及23项
netlink状态机测试通过；x86_64/2CPU/4lane内核编译通过。

`control-complete-kvm` 使用冻结的 `kernel-route-control.elf` 和实际 Alpine `ip` 完成
路由/规则的新增、dump、策略查询、删除，串口有 `NET_CONTROL_KVM_PASS`。此前3个
失败候选保留，分别暴露write路径、规则删除、路由删除的ABI差异。此为控制面验证，
不代表双网卡转发、IPv6路由或NAT的验收。使用和限制见 `routing.md`。

## bottom half 通知与引用生命周期

新增确定性用例在最终空队列观察后模拟一次并发发布，验证 pending 必须保留到下一轮；
14项 socket INET bottom-half 宿主测试通过。消费通知提前到 drain 前，避免抹掉并发
发布；全表 drain 的 walk 引用延后到所有 wait_queue 唤醒之后释放。

含本修复及正在验收的NAT事务改造的 `kernel-napt-bh.elf`，在实际改端口SNAT下完成
TCP16KiB与UDP64B全部payload校验（`napt-bh-kvm`）。此前修复时钟/校验和之后仍存在
的长流停滞样本保留；这个集成样本不是隔离的性能A/B实验。

## NAT 绑定、计时与校验和

NAT 状态事务改为子系统自己的 IRQ-safe CT 锁，移除对 lwIP CT shared-domain 和 procfs
全 lane barrier 的依赖；Rust 表项读写使用值副本，不再生成静态可变别名。先预留完整
回复元组再改包；动态端口处理客户端冲突，固定端口冲突/表满返回错误并由过滤器丢弃，
不驱逐活跃绑定。支持 ingress 未转换条目在 egress 附加 SNAT 并重建回复索引。

修复真实 KVM 暴露的两个旧缺陷：超时混用高频原始tick/32位截断，导致活跃NAT过早
过期；重写把合法TCP零校验和当成禁用校验，改端口后偶发坏包。时间统一为回绕毫秒，
TCP零值正常更新，UDP计算结果零编码为0xffff。ICMP echo回复索引也已修复。

默认/嵌入式/2GHz时钟三种宿主构建通过生产状态机断言、4线程竞争和转录比较；
filter生产hook验证NAT失败会DROP；C与Rust重写实现各通过47项独立重算检查。
新的pcap校验器确实检出旧失败样本中的3个TCP坏包，并通过修复后的抓包。

`napt-release-kvm` 为3次独立启动、每case至少1秒的改端口SNAT：TCP16KiB echo
10.26/11.76/11.51MiB/s，UDP64B全部payload校验通过；三份完整pcap的IPv4/TCP/UDP
校验均无错误。CPU口径为全QEMU进程，请勿解释为guest-only周期计数。此处不作严格
性能非劣性判断，也不以SLIRP结果宣称已完成LAN→WAN转发或成熟软路由。

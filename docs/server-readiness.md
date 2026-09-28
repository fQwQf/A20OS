# A20OS 服务器就绪度评估

最后核实：2026-09（`server-hardening` 分支）。本文记录 A20OS 面向服务器
部署时的**当前能力边界**、已知的结构性限制，以及按严重度排序的阻塞项。

本文只写已在源码中核实的事实。每条都给出文件位置，便于自行复核。
运行类结论以当期提交为准；历史记录见 [archive/](archive/)。

## 一句话结论

A20OS 已经是一个认真的内核，但**当前形态是「QEMU 上的桌面/研究内核」**，
不是「服务器内核」。差距不在功能数量，而在三个结构性问题：网络数据面
被单一全局锁串行化、容器隔离的前置件（PID/userns/pivot_root）缺失、
真机 PCIe 可用性受硬编码 QEMU 假设限制。

在下面的阻塞项收敛之前，把数据库或不受信任的工作负载放上去是不安全的。

## 一、存储持久性

### 已达成

`fsync()` 现在真正到达稳定介质。`block_dev_t` 有可选 `flush` 原语，
`bcache_sync_common()` 在写完数据后、缓存锁之外调用它。实现覆盖
virtio-blk（`VIRTIO_BLK_T_FLUSH`）、loop（转发 backing file 的 fsync）、
AHCI（`FLUSH CACHE EXT`）。

验证：`make smoke-fsync-durability`，断言 `block_flushes` 计数器确实增长。

### 仍缺

- **ext4 不是日志文件系统。** `kernel/fs/diskfs/ext4_journal.c` 明确只做
  挂载时回放，然后把日志标记为空并清除 `RECOVER`。RW 挂载后没有 journal
  提交、没有 ordered 模式语义，写回途中崩溃可留下 journal 本可避免的
  元数据/数据不一致。对需要崩溃一致性的数据库，这是硬阻塞。
- **AHCI 路径仅编译验证。** `ahci.c` 位于 `CONFIG_AHCI` 之后，树内没有任何
  实例挂载 AHCI 控制器。补一个挂 `ich9-ahci` 的门禁是缺失的一环。
- **无断电/崩溃注入测试基础设施**，因此上述 journal 改造无法被验证。
- **无 RAID、无数据校验和、无快照/CoW、无 fs-verity。**
  `crc32c` 只用于校验 JBD2 回放日志，不覆盖常规文件数据。

## 二、网络

### 已达成

可用的 IPv4 无状态包过滤（`kernel/net/netfilter.c`），控制面在
`/proc/a20/netfilter`。启动时无规则、默认 accept，未配置机器行为不变。
验证：`make smoke-netfilter`。

### 结构性限制（按严重度）

1. **整个 TCP/IP 数据面被一把全局自旋锁串行化。**
   `g_lwip_lock` 保护全部 lwIP 核心状态，每次 raw lwIP 调用都必须持有
   （`docs/net/network-lock-contract.md`）。**多核服务器最核心的收益在这里
   直接归零**——这不是性能调优能解决的，需要重构 lwIP 集成。
2. **无连接跟踪与 NAT。** 因此不能做端口转发、地址转换，也无法实现
   有状态的防火墙规则。
3. **窗口缩放缺失 → BDP 硬上限约 47 KB。** `LWIP_TCP_SACK_OUT` 为 0，
   `TCP_WND` 固定 32×MSS = 46720 字节。**任何 BDP 超过该值的链路
   （跨城、跨洲）吞吐被硬顶死**，与调优无关。
4. 无 SACK、无 ECN、无 SYN cookie、无 `MSG_ZEROCOPY`。
5. 多队列/RSS/RPS/XPS/XDP 全部缺失；virtio-net 只有一对硬编码队列
   （RX=0/TX=1），无 MSI-X，无任何卸载（CSUM/TSO/GSO/GRO）。
6. `SO_BINDTODEVICE`、`IP_TRANSPARENT`、`TCP_CORK`、`TCP_USER_TIMEOUT`
   等返回 `-EOPNOTSUPP`（本轮修复：此前它们返回成功并被静默丢弃）。
7. accept 队列固定 128 且溢出静默丢弃；无 socket 内存压力控制，也无
   `/proc/sys/net/*`。

## 三、隔离与多租户

### 已达成

cgroup v1/v2 是真的，且**在热路径上强制**：`cg_mem_charge()` 在缺页路径有
十四个计费点，`cg_cpu_account()` 在每次上下文切换记账。`cpuset` 通过掩码
求交生效（`sched_task_cpu_mask()`）。这比很多「有 cgroup 目录」的项目实在。

### 仍缺

1. **8 个 namespace 只有 mount 是真的**，其余 7 个由 `unshare()` 显式返回
   `-EINVAL`。这一点做得诚实（`sys_namespace.c:4-7` 明确写了边界）。
2. **`pivot_root` 返回 `-EPERM`**，因此即使有容器运行时也无法换根。
3. 无 userns、无 `nsproxy`、无完整 capabilities。
4. **无容器运行时**（lxc/runc/nspawn/crun/podman 均无），`packages/world/`
   里没有 server world。
5. cgroup 缺 `pids` / `io` / `freeze` 控制器；`cpu.shares` 存了但调度器
   从不读取。
6. **全局 OOM killer 的评分只看 `oom_score_adj`，不看 RSS**，因此不会可靠地
   选中最大占用者（cgroup 局部路径倒是用了 RSS）。

结论：**当前形态无法承载多租户**。PID ns + userns + `pivot_root` 是绕不过
去的三件套。

## 四、进程与调度

- **无内核抢占**（无 `CONFIG_PREEMPT`，只有 `need_resched` 标志在安全点消费），
  也无 IRQ 线程化。`SCHED_FIFO` 存在，但过不了长内核路径的 deadline。
  `SCHED_RR` 本轮已修成真正轮转，但只能在调度器下次运行时让出 peers，
  **无法按时间片强制抢占**——这依赖上面那条抢占缺口。
- 无 RT 限流（`sched_rt_runtime_us`）、无 `RLIMIT_RTPRIO`：`SCHED_FIFO`
  任务可以独占 100% CPU，无预算、无计量。
- **`proc_lock` 仍是压倒性热点**：8 核压测 33335 次竞争 / 16191822 自旋，
  多轮优化后仍 12–20K。根因是整个任务表只有一把全局自旋锁。
- `pid_max` 默认 32768，**与 Linux 默认值一致，且可通过
  `/proc/sys/kernel/pid_max` 运行时调整**（`procfs.c` 的 sysctl 写路径
  已接线）。因此它不是缺陷；确有需要的部署自行调高即可。
  （本文件早期版本把默认值列为待修项，是错的：默认值本身与 Linux 相同，
  上调反而会造成与 Linux 的行为差异。）
- `MAX_PROCS`（consts.h，MCU 32 / 托管构建 4096）是**死常量**：除自身
  `#define` 外全树零引用，任务结构是 `kcalloc` 动态分配的。它会误导读者
  以为存在 4096 的进程上限，应删除。
- **`RLIMIT_AS` / `RLIMIT_NPROC` 本轮已实现并强制**（此前完全缺失，
  单进程可耗尽宿主机内存）。
- 所有 `smoke-*` 门禁硬编码 `-smp 1`（`perf-overhaul.md:127` 明确警告），
  **单核通过不证明 SMP 正确性**。对一个服务器 OS 的 CI 这是结构性缺陷。
- 全部性能数据来自 QEMU TCG 模拟器，无真机基准。

## 五、可观测性

本轮把「静默说谎」的观测面改成了真实测量（详见
[../security/hardening.md](../security/hardening.md) 的诚实性原则）：
`/proc/<pid>/io`、`/proc/loadavg`、`/proc/pressure`、`getrusage`、
`TCP_CONGESTION` 现均为真实值。

仍然缺失或不可用：

- **无硬件 PMU**：`perf_event_open` 只有 software event，无 mmap ring
  buffer（只能 read），无 group leader → `perf top` 类采样不可用。
- **无 ftrace / kprobes / tracepoints**。`bpf(2)` 是真的 eBPF 解释器，
  但只有 1 个扩展点、所有 map 命令返回 `-EOPNOTSUPP`、无 kprobe/perf 挂载
  类型，**不能用于可观测性**。
- 无 audit 子系统、无 syscall 审计、无文件完整性度量、无安全启动/dm-verity。
- 无日志外发（syslog/journald 集成）。
- seccomp 机制可用但**树内无任何服务安装 filter**。

## 六、可靠性

- **panic 是关机不是重启**（`panic.c:117` 调 `firmware_shutdown()`），
  失败则 `arch_halt()` 死循环。
- **无跨 CPU stop IPI**（`panic.c:20-22` 自述）：其他核继续跑到自己 panic，
  输出被丢弃。
- **panic 文本不进 klog 环**（直接调 `printf`/`uart_puts`）→ 控制台卡死时
  dump 全丢。
- kexec 只有 staging，**无执行后端**（`sys_proc.c:760-764` 返回 `-ENOSYS`）
  → 无 kdump。
- **无 watchdog**（仅 STM32 MCU 的 IWDG），无 softlockup/hung-task 检测
  → 内核挂死即永久挂死。
- **无 A/B 分区、无 dm-verity、无回滚**。升级 = 重刷。
- 无节点级自愈：没有 fencing、quorum，也没有监管 `svcmgr` 自身的东西
  ——它挂了所有服务变孤儿。

## 七、真机与虚拟化

- **PCI 不遍历 bridge**：`pci_scan_current()` 线性扫预配置 bus 范围，从不读
  header type 1 的 secondary/subordinate bus 寄存器。riscv64 上范围是
  `(0,1)`——**只有 bus 0**。挂在 PCIe switch 后面的设备根本看不见。
- **无 MSI/MSI-X** → 只有 INTx。
- **INTx 路由硬编码 QEMU q35**：`x86_64/trap/irqchip.c:251-274` 只认
  host bridge `0x29c08086`，否则 `return -1`。代码注释自述需要
  ACPI `_PRT` 与 PIRQ link 编程。
- **ECAM 基址是编译期常量**（仅 virtualbox-aarch64 从 MCFG 读）。
- **ACPI 基本没有**：只有 RSDP + MADT + HPET + TPM2。**无 DSDT/AML 解释器**
  → 电源管理在架构上就不可能。
- **无真 RTC**：wall clock 从编译期常量 `A20_BUILD_UNIX_TIME` 起步。
- **无 paravirt clock**：KVM 检测只用于决定是否信任 TSC，无 kvm-clock
  兜底 → KVM 下 guest 存在时钟漂移风险。
- 无 virtio-fs/DAX（**服务器存储共享路径完全缺失**）、virtio-rng、
  virtio-console、virtio-balloon。
- 好的一面：RISC-V IOMMU 是 755 行真实现（fail-closed）；x86_64 TSC 校准
  完整（CPUID 0x15/0x16 + PIT + invariant-TSC）；idle 路径是真实架构停机
  （`sti;hlt` / `wfi`）而非忙等。

## 八、阻塞项排序

| 级别 | 阻塞项 | 理由 |
|---|---|---|
| P0 | lwIP 全局锁分片 | 决定多核网络收益能否兑现，其余网络工作都在它之下 |
| P0 | PID ns + userns + `pivot_root` | 多租户的前置件，缺一不可 |
| P0 | ext4 可写 journal + 崩溃注入测试 | 数据库一致性的硬前提 |
| P1 | conntrack + NAT | 容器网络与服务暴露的依赖 |
| P1 | 窗口缩放 | 消除 47KB BDP 硬上限，跨地域链路的必要条件 |
| P1 | PCI bridge 遍历 + MSI-X + ACPI `_PRT` | 真机服务器的准入条件 |
| P1 | kdump 执行后端 + panic 改为重启 | 故障后能否自动恢复 |
| P1 | 内核抢占 + RT 限流 | 实时性与尾延迟保证 |
| P2 | 硬件 watchdog + A/B 分区 + dm-verity | 无人值守与安全更新 |
| P2 | 硬件 PMU + ftrace/tracepoints | 生产环境可诊断性 |
| P2 | `proc_lock` 按等待对象分锁 | 进程数上去后的扩展性 |
| P2 | virtio-fs/DAX | 共享存储 |
| P2 | 真 RTC + paravirt clock | 真机时间正确性 |
| P3 | NUMA、热管理、C-states | 规模与能效 |

## 九、推荐的第一批动作

按「改动小、风险低、避免真实事故」排序：

1. 补一个挂 `ich9-ahci` 的门禁，让 AHCI flush 获得运行验证。
2. 把 `/proc/pressure` 改为 Linux 的 `cpu`/`memory`/`io` 子目录布局——
   systemd 等工具按目录读，当前单文件布局不兼容。
3. 崩溃注入测试基础设施（QEMU 可用 `-device qemu-x-test` 或直接 kill -9 +
   重放镜像比对），这是 ext4 journal 改造的前提。
4. 删除 `MAX_PROCS` 死常量（纯清理，无行为变化）。
5. 引入真机基准入口；当前所有性能结论都来自 TCG 模拟器。

其中第 2 项是纯兼容性修复，第 4 项是一行常量，二者都不需要设计决策。

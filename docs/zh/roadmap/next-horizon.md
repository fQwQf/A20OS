# 生态纵深：历史评估与当前工程入口

本文档对系统远期演进方向与技术储备进行评估。当前工程子系统的完整交付要求见 [1.0 计划](../releases/v1.0.md)，详细缺口见 [工程清单](a20os-improvement-todo.md)。下文早期测量保留历史含义，候选验证需执行 [功能/性能对照](../releases/v1.0-validation.md)。

## 后续版本功能强化

状态更新：2026-11-16，按 [1.0 范围决定](../releases/v1.0.md#24-功能强化移出-10) 移出发布目标。下列事项尚未全部完成，后续按设备、应用和具体版本独立立项；既有实现和测试继续保留。

| 方向 | 当前状态 | 后续范围 |
| --- | --- | --- |
| Native Linux 人格层（原 N06） | 已有 pipe 字节流及 fd/mmap/socketpair/futex/EventQ facade | fd/epoll 语义深化、更多兼容测例与必要性能对照；见 [人格层设计](../hybrid-kernel/05-idl-and-personality.md) |
| PCI/MSI-X（原 N10/N11/N12） | 已有 x86 路由、部分回退及 QEMU 网卡真实流量记录 | per-function/queue 亲和性、其他架构扩展与向量计数专项认证；不取消现有驱动错误/回收修复 |
| 文件系统元数据 | ext4 user xattr 与 ACL 属性名可存储读回，ACL 不参与权限判定 | ACL 权限/继承、更多 xattr 命名空间及其他后端持久化；见 [FS 现状](../status/current/fs-status.md) |
| 笔记本实机能力 | 第 19 轮记录已确认启动、键盘、触控板及 WLAN 关联/DHCP，仍有输入/应用阻塞与慢写回记录 | 新设备能力与完整实机认证；核心写回/资源安全缺陷继续按现有功能修复；见 [交接记录](../platforms/x86_64-laptop-handoff.md) |
| DRM/GPU | 已有通用 request/queue、部分 VirtIO GPU 接入、每文件 GEM/FB owner 及部分映射寿命 | 完整 master/auth、共享访问、多 GPU、厂商 BO/GPUVM/context/队列与复位恢复；现有映射/DMA 提前释放风险仍须修复 |

### GPU 与 AMD-Vi 后续实现前置

下面保留已有实现及后续开发的细项，状态不因移出 1.0 而变为完成。通用停止失败与资源回收的现有缺陷继续列在 [1.0 驱动台账](../status/gaps/drivers.md)。

| 待完成范围 | 关闭条件 |
|---|---|
| AMD-Vi 接管前资源完整性 | 已有被动 PCI 唯一绑定、IVRS 保留、完整 aperture checked claim，以及 probe 失败/解绑/重绑的主机与 QEMU 证据。已通过独立存储保留对象导入受管 RAM 与 ACPI 表页。已有 PCI 桥路径/AMD Range 查询，以及组合 IVRS requester/alias、PCI 路径和控制器上下文租约的保留路由对象。已有完整固件声明集合查询、逐成员发布/路径/归属/BME 检查与原子占用，成员失效后继续保留。仍须补齐 PFA 以外 RAM、其他固件/DT 保留来源，通过各所属驱动停止全部 alias peer 并协调归属变更，落实实际 AMD domain 的硬件权限/Unity/Exclusion 及停止/失败保留（已有覆盖全部声明成员的独立 IVMD 约束和通用映射入口检查），并补物理平台证据；被动绑定和只读快照均不授予硬件启用权限 |
| AMD-Vi 翻译与故障处理 | 已有 IVMD → 独立 policy 与 managed 映射入口预检；仍需为实际 requester/alias 在硬件中落实权限、Unity 和排除范围，验证页表、失效完成、真实 DMA 读写与非法 IOVA fault；停止/失效超时继续保留硬件可访问页面与上下文，并有可重试关闭证据。现有 RISC-V 测试不能关闭此项 |
| GPU 消费者与映射退役 | CPU 物理映射安装/失败回滚已共用 MM/Rust 事务，权限 ceiling 与 Device 属性保留已接入；该物理视图仍借用 backing；有 get_scanout 的 RAM fbdev 分支已由连续 VMO 与普通 VMA 保留存储，支持 fork/split，模块 ABI 12（owning scanout 引入于 11）。fbdev 单次调用与 sysfs GPU 快照已有同绑定 class access，默认选择保留元数据且旧 publication 不跟随重绑；DRM 已固定文件 publication，隔离 GEM/CRTC/EDID/事件并在 scoped access 内调用，未知 resource/context 清理保留到成功或旧绑定停止，导出 pin/VMA 保留 CPU 存储；每文件 GEM/FB owner、正常 close、显式导入及失败输出回滚已实现；仍须补完整 master/auth、共享访问权、厂商 BO/context/请求/fence 与旧 get_fb 持久物理 VMA 的所有权和退休协议。virtio-gpu 内部暂存/IRQ 已要求 reset 读回确认，失败 probe 和退出保留对象，每绑定已持有独立原生 DMA 实例/队列，失败仅保留该实例，双设备 peer 刷新/逻辑注销/重绑已有 QEMU 证据；仍需厂商执行队列、外部 GEM backing 的请求/完成寿命、多 GPU 用户节点和跨 GPU attach。一次 class active call 不保留调用返回的存储，也不能证明 GPU 已停止；默认选择已按显式原生角色区分 headless 与固件回退；角色查询不提供通用渲染提交、格式/队列限制或硬件健康证明 |
| GPU 内存与请求模型 | 已有固定 resident RAM VMO：预分配/清零、固定 extent/slot，原始 cgroup charge 随 VMO 存活，DRM 已采用；这不等于完整 BO 预算/迁移/驻留策略。独立 DMA 页 pin/map_vmo 不保留该预算 owner；新增 map_owned_vmo 可显式为固定 kind 4/5 在成功与失败清理期间保留整个 VMO/charge，确认失效后锁外交还 owner。设备只映射选定视图；不等于 BO 驻留/迁移策略，也不授予共享访问或 GPU 执行权限。已有独立 DMA buffer 及显式 domain 映射，失效失败保留整块分配；原生 VMO 已通过原子 canonical 页快照接入 mapper，源 resize/销毁不提前回收所映射帧，但不转移 charge 或代替 extent/驻留/访问租约；RISC-V native provider 借用该所有权；尚不代替 BO/GPUVM/驻留；仍须建立每设备 BO、GPUVM、context、提交与 fence 引用，验证进程退出、并发提交、超时/复位时请求完成和资源生命周期；通用 fence 已有可撤销的一次性通知，支持完成上下文调度 work，并在 callback 在途时保留 owner；通用 request 已保留 native backing，queue 已覆盖已接受请求、代码/context 及 stop/retire 重试；VirtIO GPU 已接入。厂商 BO/GPUVM、硬件队列调度、独立 abort、复位恢复与 DRM 同步 UAPI 仍需实现 |
| Phoenix 原生执行与呈现 | 固定上游/固件版本后，依次取得真实 SDMA copy/fill 回读、Mesa 离屏渲染及显示接管证据；分别记录平台、设备身份与失败行为，不以 GOP、virtio-gpu 或软件渲染结果替代 |

## 1. netfilter / 防火墙（第一切片已落地）

- 现状：已实现可用的 IPv4 包过滤（`kernel/net/filter/netfilter.c`）。hook 挂在`a20_lwip_process_netif_rx_tx_locked`（进）与 `a20_lwip_linkoutput`（出），即 `next-horizon.md` 当初评估的两个数据面点位。规则表 32 条，按方向、协议、源/目的地址、源/目的端口匹配，每条独立可通配；accept/drop 判决；每规则与全局计数器。
- 控制面：`/proc/a20/netfilter`。读为策略+计数器+规则表，写接受`add <rule>` / `del <n>` / `reset`。格式非法的规则被拒绝而非静默接受，避免一次拼写错误悄悄放宽或收紧策略。
- 启动时无规则、默认策略 accept，因此**未配置的机器行为与引入前完全一致**，不会让既有部署回归。
- 验证：`make smoke-netfilter`（`user/cmds/net/netfilter_test.c`）。除控制面外，断言匹配 drop 规则的 UDP 发送确实让 `out_dropped` 增长。实测`dropped=3 out_packets=3`，证明 hook 在真实数据面执行而非空过。
- 后续已实现 IPv4 conntrack 与 NAT，旧稿“仍缺、不能转发/转换”的结论失效。当前支持边界、回程与分片/ICMP 限制见 [conntrack/NAT](../net/conntrack-nat.md)。完成现有网络子系统还需真实双向流量、耗尽/回收、错误路径与性能验收；不能仅凭首个无状态过滤 smoke 宣布完成，也不能把未实现的网络目标直接删掉。

## 2. Native 完整 libc 与动态链接

Linux ABI 下 Alpine 动态程序和 Native 完整 libc 是不同验收范围。Native 的动态链接、线程、检查点通知与 exec 继承按 [运行时状态](../status/current/native-abi-runtime-status.md) 和[启动协议](../native-abi/07-startup.md) 收口，纳入 V1-RUNTIME。Native 通知遵守[设计边界](../native-abi/checkpoint-signals.md)，完整异步处理器既不是 1.0 必须项，也不是默认下一版本待办。最小 hello 只证明加载链路，不能代替完整运行时和真实应用；迁移期间同步验证既有 musl 生态不回退。

## 3. 文件系统 journal、持久化与元数据

ext4 可写 journal 和 ordered 提交/回放已有实现及 `smoke-ext4-journal` 入口，旧稿“只读回放、写 journal 尚未立项”的结论失效。当前特性、格式兼容与数据保护边界见[工程清单](a20os-improvement-todo.md) 和 [一致性模型](../fs/fs-consistency-model.md)。原 N21 ext4 专项已按[发布范围 §2.6](../releases/v1.0.md#26-ext4-专项任务移除)从 1.0 队列移除，既有文件系统行为按支持矩阵回归；VFS 的 xattr 接口存在不证明每个后端都已持久化，元数据校验也不等于文件内容校验。各项分别验收，不沿用旧稿的一两天/一周工期估计。

新增 xattr 持久化与 ACL 权限/继承目标已移到后续版本；现有 ext4 user xattr 存储继续回归，已有支持行为按正式候选矩阵回归，不重新引入 N21 专项。

## 4. 电源与板级能力

x86_64 已有 ACPI 接入，不能再笼统写“无 ACPI”；其真实固件、IRQ 和关机边界见[x86_64 平台](../platforms/x86_64-pc.md)。这不自动证明 cpufreq、系统 suspend/resume 或 CPU 热插拔完成。已有正式支持配置按 V1-DELIVERY 收口；新增笔记本能力与完整实机认证属于上表后续功能；新的电源管理候选先明确需求、硬件与验收再立项，不用历史评估给出当前未核实的能力或工期结论。

## 原评估中未立项或暂缓的扩展

原评估列出以下候选边界；它们不取消其他文档中已经立项的工程要求。

- 新增文件系统（原评估举 exFAT、btrfs 为例）：没有单独立项决定时不因本页自动加入 1.0。
- KASLR：评估结论见 docs/zh/security/hardening.md（需 PAGE_OFFSET 运行时化前置重构，暂缓）。

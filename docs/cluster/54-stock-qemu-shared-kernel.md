# 系统自带 QEMU 的跨实例共享内核验证

无需 QEMU 源码、补丁或重新编译模拟器。`QEMU_SHARED_SMP=1` 为 A20OS 增加一个
可选的 x86_64 验证平台：两个独立的系统 QEMU/KVM 进程，各有一个 vCPU，映射
同一个 RAM 文件。一个进程完成引导，另一个从 A20OS 的次级启动 ROM 进入同一个
内核的次级 CPU 入口。共享分配器、任务表、页表和调度器只有一份。

该平台复用已有的 `smp_ops` 和任务接口；QEMU 特有的启动和中断投递适配留在
平台层，不增加应用 syscall 或集群线上格式。MCU 的共享总线/仲裁后端仍见
[25](25-native-shared-kernel.md)，跨架构执行域与可执行镜像约束见
[27](27-portable-domains.md)。本测试的两个执行端都是 x86_64，不能由此推断
异构 ISA 可以直接执行同一份机器码或共享栈。

## 复现

要求本机 x86_64、可读写的 `/dev/kvm`、系统 `qemu-system-x86_64`、Python 3、
项目 x86_64 工具链以及 GNU `as`/`ld`。RAM 临时目录至少留出 600 MiB；构建输出
位于独立 `.kernel-build` 变体目录。若需要把 RAM 放在其他**可写**分区，使用
`TMPDIR=/path/to/temp`；脚本不修改挂载方式。

```sh
python3 tools/check-multi-qemu-kernel.py \
  --out .kernel-build/cluster/multi-qemu/shared-run

# 同一个真实 Native 用户程序跨实例创建线程和分配计算任务。
python3 tools/check-multi-qemu-kernel.py --workload native \
  --out .kernel-build/cluster/multi-qemu/native-run

# 用户程序对照也可组合 --workload native --mode smp。
# 对照：普通单 QEMU、双 vCPU，关闭新增配置。
python3 tools/check-multi-qemu-kernel.py --mode smp \
  --out .kernel-build/cluster/multi-qemu/ordinary-smp-run
```

输出目录必须不存在，避免覆盖失败证据。脚本自行构建内核和 64 KiB 的 A20OS
次级 ROM，冻结启动输入，再启动系统 QEMU；不下载任何依赖。
`--timeout` 控制 guest 验收的最长等待时间，默认 90 秒；`--jobs` 控制构建并行度。
脚本只停止自己创建的进程，在它们退出之后删除自己的临时 RAM 和 QMP socket，
保留内核、ROM、串口日志、构建日志、源码差异/哈希和 `report.json`。

报告通过 QMP 记录每个进程的 vCPU 和 KVM 状态，并读取 `/proc/<pid>/maps` 验证
两个进程使用同一 inode 的可写共享映射。内核模式的 guest 必须报告：

```text
[SMP-TEST] PASS cpus=2 mask=0x3 ... migrated=1->0 ... heap=cross-cpu timer-irqs=all-online ipi=acked exit=0
[SMP-TEST] CLEANUP PASS ...
```

这是现有生产调度路径执行的内核任务：在 CPU 1 分配堆和保存栈，在同一共享
调度器中阻塞、修改亲和性、唤醒后到 CPU 0 继续执行，校验 PID/栈/堆、定时睡眠、
两个 CPU 的定时中断、跨 CPU 内存屏障确认、退出和任务/PID 资源回收。
宿主只启动进程、读取证据和结束测试，不写入 guest 任务状态或通过标志。
迁移由测试显式改变亲和性触发；这本身不是自动负载均衡的验证。

用户程序模式启用 `RAMFS_USER=1 RAMFS_NATIVE_TESTS=1`，将既有 Native futex
测试 ELF、spawn 子程序和确定性输入文件放入 RAMFS。它仍使用正常 Linux init、
mksh、exec 与 Native SDK/syscall，宿主只向串口 shell 输入测试命令。RAMFS 配置
独立于 QEMU 后端，普通构建不包含这些测试资源；要求 BOTH ABI，以同时运行
Linux shell 和 Native 程序。新增 x86_64/AArch64 的 RAMFS objcopy 映射，前者
已有运行验证；后者仅补齐构建表项，不据此声称新的运行结果。

用户程序检查根身份/对象目录、用户线程创建、跨 CPU 唤醒和取消、共享地址空间
分配/保护/解除映射、4100 次文件打开关闭、并发读取同一输入、两次真实 ELF 子
程序启动及退出。两个工作线程通过既有 `a20_work_range_take` 动态领取 32 个
计算片段，检查无遗漏、无重复、总和为 4359168、执行 CPU 掩码为 3；该模式
验证了实际用户计算分布到两个实例，不代表等量分配或 CPU 利用率最优。
通过条件包括 `NATIVE_FUTEX: PASS` 和 shell 获取的真实退出码 0。

当前 UART 输出仍可从任一 CPU 的本地虚拟串口发出。检查器分别保留两个日志，
在检查用户程序标志时汇总两端输出；没有实现统一输出终端。共享 UART RX 环
允许 shell 读取主端输入，这不等于统一了任意设备的访问。

## 平台实现

- 主端使用正常 `-kernel` 引导。次端使用 `tools/boot/shared-qemu-reset.S` 生成的
  ROM；它不再次初始化共享 RAM，等待主端在物理 `0x6f00` 发布启动标志，然后
  进入 `0x7000` 的既有 AP trampoline，使用共享页表和独立的次级栈。
- 两个虚拟机的本地 APIC ID 都为 0。次级 trampoline 提前设置已有的 GDT 逻辑
  CPU 标签；之后继续使用原来的 `arch_current_cpu_id`。平台枚举的 0/1 是共享
  内存端点标识，不宣称是可跨虚拟机寻址的 APIC ID。
- QEMU 的 LAPIC 不能跨进程发送 IPI。平台将重调度、TLB 和向量配置请求发布到
  共享原子位图，由各端真实 LAPIC 定时中断轮询，再调用原来的 handler。
  为唤醒空闲 CPU，此配置将定时间隔上限设为 1 ms；只在显式启用时生效。
- 两个 KVM 的 TSC epoch 并不一致。此配置由 CPU 0 发布共享单调时间，其他
  CPU 读取同一时间，不叠加本地 TSC；本地 LAPIC 标定仍使用本地计数器，以免
  次级上线前主端关中断等待造成标定死锁。主端定时 IRQ 更新时钟，空闲时的
  编程周期上限为 1 ms，实际更新也受宿主调度和关中断区间影响。主端停滞会
  停止全域时间，尚无时钟 owner 故障接管。此配置关闭直接读取 TSC 的 vDSO，
  用户时间 API 使用正常 syscall；普通平台的 vDSO 和时钟路径不变。
- 共享内存原子性使用 KVM 下宿主 CPU 的一致性保证。脚本不自动退回 TCG；
  多 TCG 进程对共享内存原子操作的完整语义尚未验证。

`QEMU_SHARED_SMP` 默认为 0，独立 build variant 避免混用对象文件。当前只接纳
`ARCH=x86_64 BOARD=qemu-virt-x86_64 NR_CPUS=2 BRINGUP=1`。关闭配置后，普通
ACPI CPU 发现、INIT/SIPI、APIC IPI 和 tickless 定时路径保持原样。

## 当前证据与尚未完成的部分

初次运行 `multi-qemu/direct-01` 已通过上述迁移和清理断言，QEMU 版本为系统
Debian 10.0.13，两进程 PID 为 267740 / 267739；这两个测试进程已经结束。
随后 `multi-qemu/shared-02` 通过严格脚本，QMP 确认 PID 278907 / 278906
各有一个 vCPU，实际 RAM 映射的 inode 一致且为 `rw-s`。普通 SMP 对照
`multi-qemu/baseline-01` 同样通过，PID 286313 的单一 QEMU 拥有两个 vCPU。
后续 `clock-before-01` 捕获了真实时钟错误：主端前采样 31725437707，远端
31726417545，主端后采样 31726396812，远端值超出因果顺序允许的区间，内核
断言失败。修复后 `clock-after-02` 通过时间顺序握手、主端生成/远端等待的
绝对截止时间和原有迁移/回收断言。`native-01` 的两个 QEMU PID 为
298748 / 298747，实际用户线程与动态工作分配检查通过；`native-baseline-01`
在普通单实例 SMP 上运行相同用户程序也通过。

新增的严格时间顺序检查在普通 SMP `clock-baseline-01` 中也观测到 689 ns 的
远端领先（31683245694 大于主端后采样 31683245005）。普通平台原本只对每 CPU
读数做单调钳制，没有全域发布顺序。本次保留该生产路径和性能；严格顺序断言
只对新增的共享时钟后端成立。普通 SMP 继续记录三次采样，并检验远端睡眠、
任务迁移和回收，输出 `clock=sampled`，不把它当成严格全域时钟的通过证据。
最终 `clock-final-01`（双实例）和 `clock-baseline-02`（普通 SMP）均通过各自
上述门槛。`execution-evidence.json` 汇总成功/失败运行并核对保存的内核和
串口日志哈希；失败记录未被覆盖。
这些测试进程均已结束；详细命令、输入哈希、映射和日志哈希保存在各次
`report.json`。四项非法配置拒绝检查及文档一致性检查也通过。

该配置仍限制在 bringup 阶段：各 QEMU 的块设备、图形、UART、APIC 和时钟设备
是独立的虚拟设备。共享 RAM 不会自动把这些设备变成统一设备。目前没有完整
设备所有权与远程访问路径，仍未验证高压力下的用户态 TLB 一致性、
跨实例 Linux/JVM 线程或单端故障后的安全继续运行。应整组启动和停止，使用
新建且清零的 RAM 文件，不支持复用旧 RAM 或单端重启。

因此，本项证明了**单一原生内核中的实际任务可以跨两个 QEMU 进程继续执行**，
尚未证明单个 Minecraft 跨进程运行。已有真实 Minecraft 客户端菜单/输入证据
属于 [52](52-main-minecraft-integration.md) 和 [53](53-linux-application-execution.md)
中的单执行域。当前已经验证共享时间和 Native 用户线程/工作分配；下一步需要
统一设备访问，验证 Linux/JVM 线程，再将真实 Java 客户端接到这套平台上；
MCU 端还需对应架构运行时与
硬件后端的验收。

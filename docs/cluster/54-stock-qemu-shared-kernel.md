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
两个进程使用同一 inode 的可写共享映射。guest 必须报告：

```text
[SMP-TEST] PASS cpus=2 mask=0x3 ... migrated=1->0 ... heap=cross-cpu timer-irqs=all-online ipi=acked exit=0
[SMP-TEST] CLEANUP PASS ...
```

这是现有生产调度路径执行的内核任务：在 CPU 1 分配堆和保存栈，在同一共享
调度器中阻塞、修改亲和性、唤醒后到 CPU 0 继续执行，校验 PID/栈/堆、定时睡眠、
两个 CPU 的定时中断、跨 CPU 内存屏障确认、退出和任务/PID 资源回收。
宿主只启动进程、读取证据和结束测试，不写入 guest 任务状态或通过标志。
迁移由测试显式改变亲和性触发；这本身不是自动负载均衡的验证。

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
这些测试进程均已结束；详细命令、输入哈希、映射和日志哈希保存在各次
`report.json`。四项非法配置拒绝检查及文档一致性检查也通过。

该配置仍限制在 bringup 阶段：各 QEMU 的块设备、图形、UART、APIC 和时钟设备
是独立的虚拟设备。共享 RAM 不会自动把这些设备变成统一设备。目前没有完整
设备所有权与远程访问路径，也没有验证两端时钟偏差、用户态 TLB 高压力、
跨实例 Linux/JVM 线程或单端故障后的安全继续运行。应整组启动和停止，使用
新建且清零的 RAM 文件，不支持复用旧 RAM 或单端重启。

因此，本项证明了**单一原生内核中的实际任务可以跨两个 QEMU 进程继续执行**，
尚未证明单个 Minecraft 跨进程运行。已有真实 Minecraft 客户端菜单/输入证据
属于 [52](52-main-minecraft-integration.md) 和 [53](53-linux-application-execution.md)
中的单执行域。下一步需要统一设备访问和时钟，验证用户线程、共享地址空间与
负载分配，再将真实 Java 客户端接到这套平台上；MCU 端还需对应架构运行时与
硬件后端的验收。

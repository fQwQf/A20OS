# 统一运行实例与执行域

## 设计目标

A20 集群应向应用呈现一个有明确生命周期、对象命名和执行状态的运行实例。同一个 Native 接口覆盖 MCU、开发板与服务器，平台差别留在硬件适配和能力中。普通单机是一个执行域的运行实例；启用集群后增加资源，不要求应用为每块板复制业务控制器。不能用多个独立游戏进程或服务器算子冒充一个客户端实例。

| 对象 | 语义 | 当前实现边界 |
|---|---|---|
| 运行实例 | 统一生命周期、对象目录、服务/作业归属和状态 | 已有绑定、成员准入、带租约任务目录；统一健康快照待实现 |
| 原生执行域 | 一个内核的任务表、PID、调度器、共享内核对象 | 一致性 SMP 和共享 RAM MCU 复用公共实现；新增公共状态查询 |
| 应用实例 | 一次应用启动及其线程、文件、设备和内存 | 已有真实 ELF 启动、ISA 变体选择和按活动数跨域分配；完整跨域资源管理尚缺 |
| 架构镜像 | 针对 ISA/ABI 的同一应用构建产物 | execution_info 初筛已实现；完整清单与自动部署尚缺 |

跨架构意味着同一机制和接口在不同 ISA 上实现。原生寄存器、栈返回地址和机器码仍有 ISA/ABI 约束；同一原生线程直接迁移的执行域必须相容。异构域可以参与一个服务/作业运行实例，不能凭网络成员身份加入本内核的 CPU mask。 Java 字节码可以使用同一应用资源，解释器和本地库仍须为目标 ISA 实现。

## 已实现的执行域状态接口

Native `execution_domain_info`（0x0003）仅查询调用者当前原生执行域，固定 64 B、version=1；不修改已有 execution_info 的结构、编号或特征位。它与板型、总线和网络传输无关，读取公共 SMP 拓扑与一次 online mask 快照。在线数量由该快照计算，不能分别读取后形成计数/位图不一致的记录。

| 偏移 | 字段 |
|---|---|
| 0、4 | u32 size=64、version=1 |
| 8、12 | u32 scope=CURRENT_KERNEL(1)、state=READY(1)/DEGRADED(2) |
| 16 | u32 flags，SHARED_KERNEL=1 表示配置了多 CPU 的同一内核 |
| 20、24、28 | u32 cpu_capacity、cpu_present、cpu_configured |
| 32、36 | u32 cpu_online、online_mask |
| 40、44 | u32 memory_model、atomic_model，枚举沿用 execution_info |
| 48–63 | 四个 u32 保留字，必须为零 |

READY 表示配置的 CPU 均已在线；DEGRADED 包括未全部上线的启动阶段或启动失败状态。它不是成员服务的健康状态，也不证明用户态依赖、JVM 或图形已就绪。 capacity 是编译容量，present 是公共 SMP 实际发现并描述的数量，configured 是本内核采用的数量。它们都不代表通过网络发现的服务器数或物理整机全部 CPU。本阶段 CPU 上线后不会因读一次查询而探测死亡，不能把 online 位当活性租约。

SDK 使用 `a20_execution_domain_info(&info)`；旧内核返回不支持即可保留旧行为。新接口按需调用，不注册周期线程，不向调度、普通内存访问、文件操作或网络收发插入透明远程请求。默认精简 MCU 不包含此查询的实际代码，须继续保持与前一阶段相同的固件哈希。服务器增加的是查询代码和派发表入口，没有新增常驻集群状态线程；本阶段不宣称已完成全量性能评测。

## 向完整统一运行实例推进

1. 显式实例身份、域身份、启动代次和任务对象名已接入，见 [34](34-runtime-namespace.md)。下一步为持久代次提供者、对象目录与资源租约，重启后不能复用旧对象身份。
2. 应用启动清单与 ISA 变体选择，将实例唯一控制点和执行域分离；失败只改变归属状态，不暗中重新执行有副作用的启动请求。
3. 文件/显示/输入设备的明确所有者与代理语义。本地对象保持本地调用路径；远程对象才使用传输，代理失败须能观察，不能自动把裸指针变成 RPC 地址。
4. 多 MCU 一个原生内核继续扩充线程创建、装载、文件与运行时。最终在真实 MCU 上原生运行 Java 解释器和真实 Minecraft 客户端，显示主菜单并响应输入。

本文件不以只读状态接口代替这些未完成项。Minecraft 的完整验收仍见[24-single-system-minecraft.md](24-single-system-minecraft.md)，内存与同步基础见[30-flat-user-memory.md](30-flat-user-memory.md)。

## 验证与尚未覆盖的状态

```sh
make check-execution
python3 tools/check-native-sync.py --output .kernel-build/cluster/native-domain-sync
python3 tools/check-native-sync.py --arches riscv64 --cpus 1 --output .kernel-build/cluster/native-domain-single
make check-mcu-program-rtl
```

宿主契约测试在 x86、AArch64、RV64 指令环境执行相同生产头，覆盖 1–32 CPU 容量、位图/计数不一致、READY/DEGRADED 记录与保留字段；x86 另运行 ASan/UBSan。 RV64/AArch64/x86 双核与 RV64 单核内核执行同一 Native 应用，检查空指针、短容量、执行信息一致性和实际在线 CPU 数，报告记录内核/磁盘哈希。 MCU 原生应用通过 SVC 在 CPU1 迁移至 CPU0 前后查询相同域，报告实际位图=3，并继续覆盖堆、等待、原子操作和退出回收。RTL 联合测试仍不是物理板证据。 RV32 同一用户测试仅编译链接；其其他 SDK 头的既有指针转换警告保留为后续项。

当前真实内核测试覆盖完全上线的 READY；DEGRADED 的结构约束有宿主证据，尚未通过物理 CPU 启动失败验证。查询不提供全局实例 UUID、热插拔或跨域对象迁移，不能把这些未完成项当作该接口的隐含功能。

普通精简 MCU 的二进制重建哈希继续为`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。这项证据只适用于该构建配置；其他配置以实际回归为准，不推断全平台性能。

用户线程与主线程先退出的真实应用生命周期扩展见[32-native-threads.md](32-native-threads.md)。它提供同一原生执行域内的共享应用线程，不替代全局运行实例身份、对象目录或跨域应用管理。

本内核内的 TASK/THREAD/DEBUG 稳定引用与 PID 复用隔离已实现，见[33-task-object-lifetime.md](33-task-object-lifetime.md)。这些对象地址只供内核内部引用，不能作为跨域 ID、持久 ID 或重启代次发送到线上。

公共运行实例绑定与可编码的任务对象名见 [34-runtime-namespace.md](34-runtime-namespace.md)。它补充 0x0003 的执行域拓扑查询；两者都不承诺全局健康或 Minecraft 就绪。

实际应用启动与按活动程序数分配已接入，复现流程及边界见 [37](37-application-execution.md)。

# 跨 MCU、开发板与服务器的公共机制

2026-10-12，按用户要求修正实现中的 STM32 偏向。A20 集群的接口不以 MCU、
树莓派或服务器划分；板型、ISA、内存连接和资源容量是独立维度。
STM32 是一个硬件适配与验证样本，不是公共集群规范。

## 统一契约

| 层次 | 所有平台的接口/语义 | 允许不同的实现 |
|---|---|---|
| 应用服务/作业 | node_id、slot、CL 会话/事务、算子与进程任务能力 | 静态或动态预算，支持的执行类型 |
| 一个原生执行域 | 同一任务表/PID/运行队列，Park/Wake、亲和性、任务退出 | 一致性 RAM 或硬件仲裁的共享 RAM |
| CPU 上线与通知 | `smp_platform_ops_t`：discover/start/send_ipi/secondary_init | SBI、PSCI、APIC、设备寄存器、fabric RELEASE |
| 非一致性外部共享域 | `a20_fabric_t` / `a20_fabric_io_t` | FSMC、其他并行总线、MMIO 桥等 |
| 架构上下文 | 同一调度器调用架构上下文切换/异常接口 | ARM、RISC-V、x86 各自寄存器与机器指令 |
| 设备与传输 | board/driver、`a20_clx_transport_t` | UART、以太网及其他链路驱动 |

普通服务器的硬件一致性 SMP 已经通过同一个 `smp_platform_ops_t` 接入。
不要求服务器采用 STM32 的寄存器或每次原子操作都绕过自己的硬件一致性。
共享内核要求共享对象、互斥与内存可见性成立；网络连接自身不能提供这些条件。

同 ISA、相同执行 ABI、相容虚拟地址/内存布局的 CPU 才能直接恢复同一份原生
线程上下文。ARM 与 x86 可属于同一个服务/作业集群，接口和业务协议相同；
当前实现不能把一个 ARM 返回地址直接作为 x86 返回地址执行。
异构节点执行各自的算子/本机程序，进程任务能力也不等于可执行任意远程 ELF。
异构单线程透明迁移需要额外的可移植执行层；目前未实现。

## 已抽取的公共 fabric 驱动

公共头文件 `kernel/include/core/shared_fabric.h` 和实现
`kernel/core/shared_fabric.c` 不依赖 STM32、ARM 寄存器、MCU 宏、堆或调度器。
所有架构使用同一份代码完成：

- MAGIC/静态拓扑验证、端口一次性 ATTACHED 与主核 CLAIM；
- 从核等待/释放、共享数据在放行前可见；
- 硬件门的竞争与 IRQ token 保存/恢复；
- 通知设置、消费及故障围栏。

总线适配只提供 host-endian read16/read32/write16、完整设备/内存屏障、
relax、IRQ save/restore。token 使用 `uintptr_t`，不能暗设为 ARM 的 PRIMASK
或 32 位。绑定及其上下文在整个调用期间有效；早期启动使用不可变或本 CPU
私有存储，不能依赖尚未初始化的共享 BSS、堆、软件锁、编译器原子函数。
STM32 早期 reset 函数禁用栈保护，因为其 canary 也位于未初始化的共享 BSS。

共享 RAM 在每个参与 CPU 上必须为不可缓存映射，或具有真实的硬件缓存一致性。
屏障负责顺序与设备写完成，不能单靠屏障让私有缓存保持一致。带 cache/MMU
的 RISC-V/ARM/x86 板必须落实这个映射契约，普通 C 指针访问才具有共同含义。

STM32 的 `arch/armv7m/platform/shared.c` 现在调用这份公共驱动；地址映射、
DSB/PRIMASK、Flash 链接布局、UART 和 CPU 异常入口留在适配层。
非一致性域的 GCC 原子 libcall 适配目前仍为 ARMv7-M 实现；新 ISA 必须提供
正确的编译器 ABI 与屏障适配，不能只改板名。公共 fabric ABI v1 当前最多
16 个端口；服务器 SMP 的 32 CPU 调度掩码限制是另一个资源边界。

`MCU_SHARED_KERNEL` 和 `MCU_NATIVE_PROGRAM` 是现有实验端口的构建选择器，
不是新的全平台应用接口，也不表示任意板型已具备 fabric 硬件。

## 证据与平台状态

```sh
make check-shared-fabric-portability
```

默认要求 x86-64/AArch64/RV64 Linux 交叉工具链及对应 QEMU user，另要求
ARM bare-metal 和 RV32 bare-metal 编译器。测试直接编译公共生产驱动，
在 x86-64 宿主进程和 AArch64/RV64 指令模拟进程中执行相同的 2–16 端口测试。
覆盖加入、启动、通知一次消费、竞争、完整宽度 IRQ token、复位拒绝、故障后
拒绝及非法目标。ARMv7-M/RV32 另做 freestanding 编译。报告逐项区分
“执行通过”和“仅编译”，在 `.kernel-build/cluster/fabric-portability/`。
fake bus 验证公共代码语义，不构成相应板型开机证据。ARMv7-M 完整固件与
真实 RTL 联合测试另见 [25](25-native-shared-kernel.md)、[26](26-native-programs.md)。

| 平台 | 当前边界 |
|---|---|
| STM32F103 多 MCU | 原生内核/应用与 fabric RTL 联合仿真；没有物理板证据 |
| RISC-V 开发板 | 仓库已有 VF2、Milk-V Duo 等板级入口；本阶段不声明新物理联调 |
| RISC-V MCU | 公共 fabric 驱动通过 RV32 freestanding 编译；多芯片板级适配尚缺 |
| AArch64 / x86 / RV64 服务器 | 既有公共 SMP/集群接口；本阶段分别记录构建/开机回归结果 |
| 树莓派 | 仓库目前没有树莓派专用 board.c，不能以 AArch64 编译代替树莓派支持 |

## 后续实现约束

树莓派与其他新板应增加板级启动、时钟、中断、CPU 启动、设备/链路适配，
沿用上述公共机制；不能复制一套成员、会话、作业或调度协议。
跨板共享内核先声明真实的共享 RAM、缓存维护、原子性、地址布局与复位契约，
再接公共执行域。独立地址空间的服务器集群使用已有服务/作业接口。

需要继续补统一的执行能力查询与镜像 ABI 匹配、非 ARMv7-M fabric 适配、
树莓派板级支持、设备所有权与物理测试矩阵。未完成项保留为未完成；不能
通过把功能名称改成“通用”来声明所有平台可用。功能优先，性能不作首轮门槛。

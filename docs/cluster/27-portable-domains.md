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

同 ISA、端序、指针宽度、执行 ABI 与相容虚拟地址/内存布局的 CPU 才能直接恢复同一份原生
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
GCC 原子 libcall 现也由公共 `core/fabric_atomics.c` 实现。新 ISA 必须提供
正确的编译器 ABI 与屏障/总线适配，不能只改板名。公共 fabric ABI v1 当前最多
16 个端口；服务器 SMP 的 32 CPU 调度掩码限制是另一个资源边界。

`MCU_SHARED_KERNEL` 和 `MCU_NATIVE_PROGRAM` 是现有实验端口的构建选择器，
不是新的全平台应用接口，也不表示任意板型已具备 fabric 硬件。

## 证据与平台状态

```sh
make check-shared-fabric-portability
```

默认要求 x86-64/AArch64/RV64 Linux 交叉工具链及对应 QEMU user，另要求
ARM/RV32 bare-metal 编译器及 qemu-riscv32。测试直接编译公共生产驱动与原子库，
在 x86-64 宿主进程和 AArch64/RV64/RV32 指令模拟进程中执行相同的 2–16 端口测试。
覆盖加入、启动、通知一次消费、竞争、完整宽度 IRQ token、复位拒绝、故障后
拒绝及非法目标。ARMv7-M/RV32 另做 freestanding 编译。报告逐项区分
“执行通过”和“仅编译”，在 `.kernel-build/cluster/fabric-portability/`。
fake bus 验证公共代码语义，不构成相应板型开机证据。ARMv7-M 完整固件与
真实 RTL 联合测试另见 [25](25-native-shared-kernel.md)、[26](26-native-programs.md)。

| 平台 | 当前边界 |
|---|---|
| STM32F103 多 MCU | 原生内核/应用与 fabric RTL 联合仿真；没有物理板证据 |
| RISC-V 开发板 | 仓库已有 VF2、Milk-V Duo 等板级入口；本阶段不声明新物理联调 |
| RISC-V MCU | 公共 fabric/原子库在 RV32 指令环境执行并通过 freestanding 编译；多芯片板级适配尚缺 |
| AArch64 / x86 / RV64 服务器 | 既有公共 SMP/集群接口；本阶段分别记录构建/开机回归结果 |
| 树莓派 4 | 新增 BCM2711 board；QEMU raspi4b 四核/乱序/稀疏核上的真实内核线程迁移、timer/IPI、退出回收通过。设备联网、完整用户态和实物未验收 |

## 后续实现约束

树莓派后续设备/链路适配和其他新板的启动、时钟、中断、CPU 启动适配，
均应沿用上述公共机制；不能复制一套成员、会话、作业或调度协议。
跨板共享内核先声明真实的共享 RAM、缓存维护、原子性、地址布局与复位契约，
再接公共执行域。独立地址空间的服务器集群使用已有服务/作业接口。

需要继续补统一的执行能力查询与镜像 ABI 匹配、非 ARMv7-M fabric 适配、
树莓派设备/网络与完整运行时、设备所有权与物理测试矩阵。未完成项保留为未完成；不能
通过把功能名称改成“通用”来声明所有平台可用。功能优先，性能不作首轮门槛。

## 公共原子运行库接入

`CONFIG_SHARED_FABRIC_ATOMICS` 选择同一份 GCC size-specific runtime，提供
1/2/4/8 字节 load/store/exchange/CAS 和 add/sub/and/or/xor 的 fetch 操作。
所有编译单元必须使用 `-fno-inline-atomics`，禁止在同一共享对象上混用本地
LDREX/STREX、LR/SC 或本地 IRQ 屏蔽替代跨端口原子性。每次操作持有公共
atomic gate，使用完整屏障；较弱 memory_order 也按此更强顺序实现。
它不是 lock-free 实现，当前没有提供完整 GNU atomic runtime 的所有扩展。

平台提供 `arch_shared_fabric()` 选择当前 CPU 的有效绑定，以及不返回的
`arch_shared_fabric_stop()`。前者不得依赖堆、软件锁或编译器原子；后者在域
故障时停止访问。IRQ token 在公共层全程使用 `uintptr_t`。普通硬件一致性
服务器继续使用原有编译器原子，不选择这个非一致性域适配。

跨架构测试使用 GCC 实际发出的 libcall，不通过手工调用替代编译器 ABI。
每个端口核对 46 次操作恰好产生 138 次门屏障，覆盖失败 CAS 更新 expected、
弱 CAS、各宽度回绕、64 位跨 32 位进位与完整 IRQ token 恢复。额外故障进程
对未映射地址发起 atomic load，要求先检测 fabric FAULT、恢复 IRQ token，
再进入 stop hook，不能访问该地址。四 ISA 均通过。

RV32 进程使用测试专用 Linux write/exit 汇编 shim，在 qemu-riscv32 中执行；
这不构成 RV32 MCU 内核或物理板证据。ARMv7-M 完整固件已改用同一公共原子
运行库，内核线程与原生应用/RTL 回归通过。这个运行库属于特权内核，当前
非特权 MCU 应用尚不能直接使用它作为线程同步 API；后续需接统一 Native
线程同步系统调用，不能让应用直接调用内核的 IRQ 屏蔽适配。


## 树莓派与服务器的共同功能验收

新增树莓派 4 的 board 适配，沿用现有 `smp_platform_ops_t`。AArch64 启动入口
接受可选布局/EL 参数，逻辑 CPU ID 使用固定于 CPU 的 `TPIDRRO_EL0`，不再暗设
MPIDR 的 Aff0 等于逻辑号。GICv2 device adapter 由 QEMU virt 与树莓派复用；
运行时 `CNTFRQ_EL0` 支持不同计时频率。公共 `fdt_view` 负责有界固件表遍历，
板级才解释 BCM2711 的寄存器和 spin-table。

`make check-raspberrypi4` 在 QEMU 的真实 BCM2711 模型上执行同一公共
`core/smp_selftest.c`；`make check-smp-native` 在 x86-64、AArch64、RV64
服务器双核配置执行它。测试覆盖同 PID/栈迁移、跨 CPU 堆对象、Park/Wake、
sleep、所有在线 CPU 的 timer IRQ、远端 IPI 应答与退出回收。它是功能验收，
不以吞吐量或速度作为通过条件。默认构建不运行该自检。

本阶段三种服务器测试均得到功能和回收 PASS；AArch64/RV64 自然退出，x86
打印关机后由夹具 SIGTERM 结束，即使其返回码为 0 也不能称为自然电源关闭。
树莓派没有电源控制实现，由测试程序
在终态输出后停止模拟器。树莓派仍缺少网卡、存储、完整用户态与物理证据，
不能将芯片内 SMP 等同于已经完成跨板共享内核。详见
[树莓派端口](../platforms/raspberrypi4.md)。

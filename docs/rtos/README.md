# 实时执行与 RTOS 子系统

本目录收纳 A20OS 的实时执行机制、第三方 RTOS ABI 人格层（CMSIS-RTOS2）以及嵌入式 MCU 支持的相关技术规范与使用指南。

A20OS 在 NOMMU / MCU 配置下以通用微内核机制提供确定性调度保障：CMSIS 线程映射为内核 `SCHED_FIFO` 实时任务，提供固定优先级即时抢占、互斥量优先级继承（PI）和每任务调度器锁（`osKernelLock`）。时钟精度以内核 tick 为基准（Hi3861 100 Hz / 10 ms，QEMU MCU 1 kHz / 1 ms）。

完整设计哲学与系统分层参见 [A20OS 实时执行模型与系统架构](overview.md)。

## 子系统文档矩阵

| 文档 | 核心议题 | 适用受众 |
|---|---|---|
| [overview.md](overview.md) | **系统定位、微内核分层哲学、调度机制总览、资源约束与验证体系** | 架构师、内核开发者 |
| [cmsis-rtos2.md](cmsis-rtos2.md) | 第三方 ABI 兼容层架构、全量 API 覆盖度矩阵、基于 Manifest 的静态裁剪 | 系统开发者、中间件维护者 |
| [execution-model.md](execution-model.md) | 线程映射、优先级分桶映射表、原子前置配置、双安全点抢占与调度锁实现 | 内核调度开发者 |
| [synchronization.md](synchronization.md) | 同步原语矩阵、`wait_owner` 依赖图追溯、内核 PI 捐赠与快照恢复状态机 | 并发与驱动开发者 |
| [interrupts.md](interrupts.md) | NOMMU 特权级约束、M 态 trap 到 U 态线程的 futex 分发器、临界区软延迟 | 板级支持与驱动开发者 |
| [porting-guide.md](porting-guide.md) | 最小应用实现、构建配置接入、tick 同步原则、内存与栈量化、LiteOS API 对照 | 嵌入式应用移植开发者 |
| [experiments.md](experiments.md) | 4.1–4.5 实验套件、QEMU 与 Hi3861 运行验证、4.5 六大实时性子项与负向对照 | 测试与质量验收工程师 |
| [latency.md](latency.md) | 调度时延理论推导、tick 量化限制、基于自由运行计数器的 4.6 测量体系规划 | 性能评估与系统优化工程师 |

## 外部与底层参考

- **[内核调度契约](../kernel/process-scheduler.md)**：内核调度器核心实现（RT 优先级分桶、EEVDF 调度器、双安全点抢占、`proc_sched_rt_donate/restore`、`sched_lock_depth`）。
- **[Hi3861 平台支持页](../platforms/hi3861.md)**：物理板硬件参数、Flash 分区、UART 调试与实板固件烧录。
- **[CMSIS 人格库源码说明](../../user/cmsis_rtos2/README.md)**：库目录布局、后端契约（`os_impl.h`）、裁剪配置与宿主单元测试。
- **[LiteOS 实验套件说明](../../user/tests/liteos-demos/README.md)**：RV32 M 态 flat program 实验源码与启动约定。

## 阅读指引

- **初次了解系统能力**：从 [overview.md](overview.md) 了解整体定位与机制分工，随后阅读 [execution-model.md](execution-model.md)。
- **移植或编写嵌入式应用**：直接查阅 [porting-guide.md](porting-guide.md) 进行构建接入，遇到同步需求查阅 [synchronization.md](synchronization.md)。
- **排查或扩展内核调度机制**：阅读 [execution-model.md](execution-model.md) 与 [synchronization.md](synchronization.md)，并结合 [内核进程调度契约](../kernel/process-scheduler.md)。
- **评估实时指标与复现实验**：阅读 [experiments.md](experiments.md) 并在 QEMU / 物理板运行门禁，阅读 [latency.md](latency.md) 查看测量协议与基准规划。

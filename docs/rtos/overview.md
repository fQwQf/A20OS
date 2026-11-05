# A20OS 实时执行模型与系统架构

本文档阐述 A20OS 在轻量级嵌入式与 MCU 环境下的实时支持模型、微内核分层哲学与关键机制设计。具体执行细节、同步原语、中断处理与移植实践见各专题文档。

## 1. 实时支持定位与保障边界

A20OS 在 Native ABI 的 NOMMU / MCU 配置下支持以微内核原语承载实时任务负载。上层通过用户态人格库接入 Arm CMSIS-RTOS2 标准，现有实时应用无需修改源码即可直接链接运行。

内核为实时执行提供以下机制与保障：

1. **固定优先级单级抢占**：CMSIS 线程直接映射为内核 `SCHED_FIFO` 调度类，优先级 1..55 一对一映射至独立就绪队列分桶。
2. **双安全点即时抢占**：高优先级任务就绪后，在系统调用返回（U 态陷入退出）与硬件中断处理完成（IRQ 返回）两个安全点立即触发调度，无需等待下一个时钟 tick。
3. **互斥量优先级继承（PI）**：支持单级与多级依赖链捐赠；内核保存基准调度快照并在释放最后一个关联互斥量时恢复，消除无界优先级反转。
4. **任务级调度器锁**：`osKernelLock` 映射为内核每任务独立的 `sched_lock_depth` 计数，锁定期间保留硬件中断响应并推迟抢占，锁释放归零时立即执行安全点切换。
5. **跨线程挂起/恢复**：`osThreadSuspend/Resume` 通过 `suspend_pending` 和 syscall/IRQ 返回安全点把目标发布为 `PROC_STOPPED`；阻塞中的目标在唤醒路径直接发布 STOPPED，恢复后再入 RT 队列。
6. **单核 tickless 低功耗**：`osKernelSuspend` 在 MCU 单核上以调度器锁 + one-shot 定时器 + `WFI` 实现，返回实际睡眠 tick；SMP 和 hosted 后端返回 0，不假装暂停其他 CPU/宿主。
7. **PMP 栈保护**：当前 flat task 的栈底安装 8 字节无权限 PMP 区域，U-mode 越界访问转为 SIGSEGV 任务退出；这不是完整任务隔离，代码/全局仍共享。

系统的工程边界如下：

- **时间分辨率受内核 tick 约束**：时间基准由架构定时器中断驱动（Hi3861 默认为 100 Hz / 10 ms，QEMU MCU 目标为 1 kHz / 1 ms）。`osDelay`、超时判定与软件定时器均按 tick 粒度对齐，不提供 sub-tick 硬截止期。
- **无形式化 WCET 上界证明**：系统通过机制消除死锁和无界阻塞，但不提供芯片级指令最坏执行时间（WCET）的形式化证明。
- **单核验证基准**：优先级继承、依赖链遍历、调度器锁与 tickless 的运行证据建立在单核 MCU（Hi3861）与单核 QEMU 模拟器之上；AArch64 SMP2 已跑通 RT runqueue 迁移、IPI 和 `SCHED_DEADLINE` 的 `sched_stress`，但跨核 RT PI 与 CMSIS `osKernelLock` 的全局语义仍未验证。
- **PMP 保护范围有限**：8 字节栈 guard 只捕获第一次越界写入，不提供 MPU 多区域隔离；堆、代码和全局仍共享。
- **无 WCET 证明**：4.8 只提供 tick jitter/syscall latency 实测，不提供芯片级最坏执行时间证明。
- **POSIX 宿主后端不具备实时保证**：`user/cmsis_rtos2/` 在 hosted 环境下提供的 POSIX 后端将线程映射为宿主 pthread，调度策略完全取决于宿主操作系统。

## 2. 机制在内核，语义在人格层

A20OS 坚持“微内核只提供通用调度机制，具体 API 语义由用户态人格库实现”的设计哲学：

```text
应用层（标准 CMSIS-RTOS2 源码）
   │  osThreadNew / osMutexAcquire / osDelay / ...
   ▼
用户态人格层（user/cmsis_rtos2/）
   │  维护对象状态机、递归深度、wait_owner 等待图、软件定时器服务线程
   │  通过后端契约（os_impl.h）调用底层 ABI
   ▼
通用 Native ABI 系统调用表面
   │  thread_create / task_set_sched / sched_lock / task_self / futex / clock_get
   ▼
A20OS 内核核心机制
      SCHED_FIFO 队列分桶 / proc_sched_rt_donate / sched_lock_depth / 定时器
```

CMSIS-RTOS2 属于独立的第三方 source-level ABI，不构成 Native ABI 的子集，亦非独立的第三种 syscall ABI。内核不维护 CMSIS 专有调用号，CMSIS 结构体亦不进入内核空间。

实现该人格层所需的 Native ABI 调度表面仅包含 5 项通用原语：

| 系统调用 / 扩展项 | 接口属性 | 职责说明 |
|---|---|---|
| `thread_create` 调度追加字段 | 既有 syscall 结构体扩展 | 在任务发布前原子写入初始调度策略与优先级 |
| `task_set_sched`（`DONATE` / `UNDONATE`） | 既有 syscall 标志位扩展 | 触发内核执行 RT 优先级继承提升与快照恢复 |
| `task_get_sched` | 既有 syscall | 查询任务当前的有效实时优先级（含继承捐赠值） |
| `sched_lock`（`0x0219`） | 新增通用 syscall | 维护当前任务的 `sched_lock_depth`，实现 `osKernelLock` |
| `task_self`（`0x021A`） | 新增通用 syscall | 为初始主线程在其句柄表中登记自引用句柄，使其可作为 PI 捐赠目标 |

这些原语完全通用，任何其他固定优先级实时人格层（如 FreeRTOS 或 RT-Thread）均可直接复用该接口集。

## 3. 确定性保障与关键调度机制

### 原子发布（Pre-publish Priority Assignment）

传统线程创建若先以默认属性启动再通过后续系统调用调整优先级，会在发布与调整之间留下低优先级运行窗口。A20OS 的 `thread_create` 支持调度属性随创建参数传入：内核在 `proc_publish_deferred_task()` 将任务插入全局就绪队列之前，已通过 `proc_sched_set()` 将其配置为目标 `SCHED_FIFO` 优先级。新线程自首次参与调度起即具备正确的实时优先级。

### 双安全点抢占

为兼顾内核执行路径的一致性与低中断开销，高优先级就绪任务在两个明确的安全点消费 `need_resched` 标志：

1. **系统调用返回点**：`riscv_mcu_syscall()` 在即将通过 `mret` 返回 U 态前，调用 `proc_sched_safe_point()`。
2. **中断退出点**：`kernel_preempt_at_irq_return()` 在硬件中断处理完成即将恢复任务上下文时触发，只要当前上下文非中断嵌套、当前任务已完成首次进入且调度锁深度为 0，即刻完成任务切换。

### 调度器锁与延迟抢占

`osKernelLock()` 在内核中体现为当前任务的 `sched_lock_depth`。当深度大于 0 时，两个安全点均保留 `need_resched` 挂起状态而不执行上下文切换；当嵌套锁深度递减恢复至 0 时，释放路径立即消费挂起的重调度请求。调度器锁仅推迟线程级调度，不屏蔽外部硬件中断。在持锁期间主动调用阻塞型 API（如 `osDelay` 或信号量等待）属于应用编程错误。

### MCU 专用无限流 RT 带宽策略

在 hosted 多任务桌面/服务器环境下，内核默认对 RT 调度类施加每秒 95% 运行预算限制，保留 5% CPU 带宽以防止低优先级系统服务彻底饥饿。但在 MCU 专用固件中，系统不存在分时普通工作负载。因此在 `CONFIG_MCU` 构建中，RT 调度器直接走 `a20_rt_take_best()`，完全旁路带宽限制器，消除了 RT 任务每秒被迫让出约 50 ms 的非预期停顿。

### 有界阻塞与广播唤醒

同步原语的底层等待均基于 futex park 机制，杜绝轮询忙等；超时时间由内核按 tick 向上取整转换。互斥量与信号量释放采用广播唤醒（`osb_bq_wake_all`），所有阻塞等待者一次性移入内核 RT 就绪队列，由内核调度器严格根据优先级拣选最高者执行；未竞得锁的低优先级任务在重新检查条件后再次挂起。该方案避免了在内核中为 futex 维护复杂优先级排队链表的开销，调度权威完全由单一调度器保证。

## 4. 资源受限环境下的轻量化纪律

针对 Hi3861 等典型 352 KiB SRAM 的资源受限目标，实时子系统实行严密的资源约束控制：

1. **静态内存供给**：CMSIS 控制块（`cb_mem`/`cb_size`）与线程栈（`stack_mem`/`stack_size`）均支持调用方静态分配，避免动态堆（`a20_malloc`）碎片化导致内存耗尽。
2. **栈尺寸精简**：默认 16 KiB 栈在 MCU 上会迅速耗尽内存，移植应用需按任务类型显式分配（典型传感器与采样任务建议 1–2 KiB）。
3. **基于 Manifest 的静态裁剪**：以 `user/cmsis_rtos2/cmsis_rtos2.toml` 为唯一事实源，`tools/cmsis_trim.py` 为不同场景生成精简定义：
   - `mcu` profile：保留线程、互斥量、信号量、定时器、事件标志、消息队列、suspend/resume 与低功耗 idle，仅包含 18 个 Native syscall；
   - `mcu-min` profile：仅保留线程、信号量与定时器，压缩至 9 个 Native syscall。未启用的对象族在人格层保留符号并返回 `osErrorResource`，确保链接兼容；内核构建时通过 `-DA20_NATIVE_SYSCALL_TABLE` 覆盖分发表，未声明的系统调用直接被分发器拒绝，不把死代码编入内核 ELF。

## 5. 验证体系与模块导航

实时子系统的正确性由两层互补的验证套件保证：

- **对象语义验证（宿主回归）**：`make -C user/cmsis_rtos2 test` 在 POSIX 与 Native 宿主模拟平台上验证所有 CMSIS 对象的创建、销毁、递归、队列吞吐与内存池双重释放防护；同时覆盖 `msg_prio` 排序、`osThreadGetAffinity` 和栈高水位查询。
- **调度语义验证（QEMU 与硬件靶板）**：RV32 M 态镜像运行 4.1–4.8。4.5 对创建优先级、`SetPriority`、`osKernelLock`、单级 PI、链式 PI 与主线程 handle 捐赠做断言和负向对照；4.6 验证跨线程挂起/恢复；4.7 验证 tickless WFI；4.8 用周期计数器记录 tick jitter 与 syscall 时延。
- **SMP 验证**：AArch64 SMP2 的 `sched_stress` 覆盖 RT runqueue 迁移、跨核 IPI、RT 带宽与 `SCHED_DEADLINE` 的 API/准入。

各领域详细设计请参阅对应专题：

- **[执行模型](execution-model.md)**：任务状态机、优先级映射表、抢占点与调度器锁实现细节。
- **[同步与优先级继承](synchronization.md)**：互斥量、等待链图遍历、内核快照与恢复边界。
- **[中断与 ISR 模型](interrupts.md)**：NOMMU 硬件特权级限制、厂商 futex 分发器与延迟投递。
- **[CMSIS-RTOS2 人格层与裁剪](cmsis-rtos2.md)**：完整 API 支持矩阵、TOML 规范与静态链接校验。
- **[移植与配置指南](porting-guide.md)**：构建接入步骤、tick 同步原则、栈内存量化与 LiteOS API 对照表。
- **[实验 4.1–4.8](experiments.md)**：实验矩阵、QEMU/Hi3861 运行命令、时延基线与负向对照分析。
- **[时延测量体系](latency.md)**：时延理论上界、自由运行计数器 4.8 基线与 WCET 边界。

# CMSIS-RTOS2 任务与执行模型

本文档说明 CMSIS-RTOS2 线程在 Native/MCU 后端上的底层执行机制：包括线程到内核任务的映射、实时优先级分桶、原子前置配置、双安全点抢占、tick 量化与调度器锁实现。

## 1. 线程到内核任务的原子映射

在 Native/MCU 后端下，`osThreadNew()` 最终通过 `thread_create` 系统调用在内核创建 `task_t` 实体，其默认调度类为 `SCHED_FIFO`。CMSIS 控制块仅作为用户态人格层元数据；任务的状态流转、阻塞挂起、分桶唤醒与硬件上下文切换全部由微内核统一管理。

```text
osThreadNew()
  └─ thread_create(sched_flags=POLICY|PRIORITY, policy=FIFO, priority=osPriority)
       └─ proc_create_thread_deferred()
            ├─ proc_sched_set()              ← 在发布入队前完成属性绑定
            └─ proc_publish_deferred_task()  ← 插入 RT 就绪队列并触发调度评估
```

调度属性随创建参数传入并在 `proc_publish_deferred_task()` 之前通过 `proc_sched_set()` 生效。这保证了新线程从首次获得执行权起即处于正确的实时优先级，消除了“以创建者优先级试运行后再通过后续系统调用调整”的调度抖动窗口。

## 2. 优先级分桶映射

CMSIS 定义的 `osPriority_t`（数值 1..55）直接作为内核 RT 优先级数值（1..99），不执行压缩映射：

| CMSIS 优先级常量 | 对应内核 RT 优先级 | 推荐系统角色与场景 |
|---|---|---|
| `osPriorityIdle` (1) | 1 | 最低实时优先级，仍严格先于所有普通非实时任务 |
| `osPriorityLow` (8) | 8 | 后台非紧急周期的管家任务 |
| `osPriorityBelowNormal` (16) | 16 | 低于常规优先级的辅助任务 |
| `osPriorityNormal` (24) | 24 | `osThreadNew` 缺省优先级 |
| `osPriorityAboveNormal` (32) | 32 | CMSIS 定时器服务线程专用优先级 |
| `osPriorityHigh` (40) | 40 | 高频控制环路、传感器数据采样 |
| `osPriorityRealtime` (48) | 48 | 关键硬实时快速响应微循环 |
| `osPriorityRealtime7` (55) | 55 | CMSIS 规范允许的最高普通线程优先级 |

在 `CONFIG_MCU` 构建中，内核将实时优先级分桶数量配置为 `PROC_RT_PRI_LEVELS = 64`，为每一个 CMSIS 优先级提供独立的 FIFO 就绪队列链表（hosted 构建为 100 桶）。

`osThreadNew()` 严格校验输入，拒绝 `osPriorityNone` 与 `osPriorityISR`（中断上下文不能作为线程优先级）；若创建属性省略优先级，则缺省配置为 `osPriorityNormal`。

## 3. 双安全点抢占机制

当异步事件或系统调用将更高优先级的任务置为就绪态后，内核并不等待下一次时钟 tick，而是在以下两个确定的安全点消费 `need_resched` 标志：

1. **系统调用退出安全点**：`riscv_mcu_syscall()` 在即将执行 `mret` 返回用户态时，调用 `proc_sched_safe_point()`；
2. **硬件中断返回安全点**：`kernel_preempt_at_irq_return()` 在硬件中断处理完成即将恢复任务现场时调用，满足非中断嵌套且当前任务调度锁深度为 0 时立即执行任务抢占。

这保证了高优先级任务能以确定性的微秒级延迟即刻夺取 CPU，避免 10 ms 的 tick 周期退化为系统最小抢占延迟。

## 4. 唤醒分发与就绪仲裁

信号量释放、互斥量解锁和事件标志就绪统一采用广播唤醒机制（`osb_bq_wake_all`）。被唤醒的等待线程被全部加入内核 RT 就绪队列，由内核调度器根据优先级从高到低依次调度执行；首个竞争成功的线程继续运行，其余低优先级线程在重新评估等待条件失败后自动重新挂起。

该设计由内核单一就绪队列负责仲裁，无需在内核中为每个等待对象额外建立优先级排队机制。

## 5. 时钟基准与 Tick 量化

系统的时钟基准由内核硬件定时器中断直接提供，时间分辨率为 tick：

| 运行平台 | 硬件 Tick 频率 | 最小延时单元（`osDelay(1)`） |
|---|---|---|
| Hi3861 靶板 | 100 Hz | 10 ms |
| `qemu-virt-riscv32-mcu` | 1000 Hz | 1 ms |

`osDelay`、接口超时与 `osTimer` 软件定时器的分辨率均以此粒度为最小单位。`osKernelGetTickFreq()` 返回 CMSIS 人格层编译时确定的频率，必须与底层的 `ARCH_TIMER_FREQ` 严格对齐。

`osKernelSuspend()` 在单核 MCU 上映射为 `kernel_idle`：得到当前任务的调度器锁，按 `proc_next_timer_interval()` one-shot 重装 MCU 定时器并执行 `WFI`，返回实际经过的 tick；IRQ 仍响应，但 IRQ 返回安全点因锁深度非零而不切换任务。SMP 和 hosted 后端返回 0，不会假装暂停其他 CPU/宿主。

## 6. per-task 调度器锁（osKernelLock）

`osKernelLock()` 映射为内核每任务独立的计数器 `sched_lock_depth`，而非简单的用户态优先级提升：

- **获取锁**：`sched_lock` 系统调用将任务的 `sched_lock_depth` 加 1；
- **释放锁**：深度递减；当计数减为 0 时，释放路径立即就地执行一次安全点抢占检查；
- **延迟生效**：在深度大于 0 期间，安全点检测到 `need_resched` 仍持续保持挂起，不触发切换。

调度器锁仅推迟调度器抢占，不屏蔽外部硬件中断；在持锁期间主动调用阻塞型接口属于应用编程错误。

## 7. RT 带宽管理策略

在 hosted 多任务环境下，内核默认对 RT 调度类施加每秒 95% 运行预算，强制保留 5% 运行带宽以防止普通任务饥饿。但在 MCU 专用固件镜像中，系统不存在分时普通工作负载。因此在 `CONFIG_MCU` 构建中，RT 调度器走 `a20_rt_take_best()`，旁路该预算限制器，避免实时任务每秒出现约 50 ms 的强制让出停顿。

## 8. 调度确定性边界与约束

系统在 Native/MCU 后端下提供以下确定性保证：

1. **绝对优先级抢占**：在任何时刻，CPU 均严格分配给当前处于最高优先级的就绪任务；
2. **即时剥夺**：更高优先级就绪任务在系统调用或中断退出的安全点立即切入；
3. **消除无界反转**：沿 `wait_owner` 链捐赠有效优先级，基准快照在最终释放时完整还原；
4. **确定性启动**：`osThreadNew` 创建的任务自首次执行即以指定优先级运行。

系统的运行边界如下：
- 系统不提供基于静态指令周期的 WCET 形式化证明；
- 在持锁或捐赠执行期间，禁止调用阻塞型接口或修改基准优先级；
- POSIX 宿主后端映射为宿主 pthread，不享受上述实时确定性保证；
- 多级优先级继承与 CMSIS 调度器锁基于单核单 runqueue 模型验证；AArch64 SMP2 已跑通 RT runqueue 迁移/IPI/`SCHED_DEADLINE` 压力，但跨核 RT PI 与 CMSIS `osKernelLock` 的全局语义仍未验证。
- PMP 栈 guard 只捕获第一次越界写入，不构成完整 MPU 任务隔离。

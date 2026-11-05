# 实时时延分析与基准测量体系

本文档定义 A20OS 在 Native/MCU 环境下的实时时延评估指标、调度时延的理论边界，以及已经落地的两类测量方法：

- **实验 4.8**（`user/tests/liteos-demos/latency.c`）在 MCU demo 镜像内通过 `clock_get` 的 raw clock id 7/8 读取 `rdcycle` 与频率，输出 tick jitter 和 syscall latency 的 min/avg/max。
- **独立时延探针**（`user/tests/test_native_latency.c` → `/bin/native-latency`）在 AArch64 上单独测量 RT `wake-to-run`，并读取内核累计的 timer IRQ 入口时延。它与 4.8 分开，避免额外线程/代码越过 MCU 共享堆的区域边界。

两类数据都只是运行机器上的观测分布，不构成 WCET 形式化证明；`irq_to_handler` 中“内核 timer handler 入口”与“U 态 Dispatcher 首行”仍是两个不同的端点。

## 1. 核心实时评估指标

| 评估指标 | 测量端点定义 | 关注的系统机制 |
|---|---|---|
| `tick_jitter` | 周期任务由 `osDelay(N)` 唤醒的实际物理时刻与期望时钟时刻的差值 | 定时器中断偏差与唤醒调度时延 |
| `wake_latency` | 释放互斥量/信号量发起 `osb_bq_wake_all()` 到最高优先级等待者首条指令执行的间隔 | 广播唤醒分发与 RT 分桶选择开销 |
| `preempt_latency` | 异步事件将更高优先级任务置为就绪到当前低优先级任务被切出的间隔 | 系统调用返回安全点与中断返回安全点的抢占时延 |
| `wake_to_run` | 协调者调用 `futex_wake` 到同 CPU 更高优先级 RT 线程执行第一条用户指令的周期数 | 内核唤醒、IPI/抢占请求、上下文切换与返回 U 态的综合耗时 |
| `timer_irq_entry` | 定时器 compare 到期值到内核 timer handler 第一条指令的周期数 | 硬件中断响应与 trap 入口；不含设备 IRQ，也不含 U 态分发 |
| `irq_to_handler` | 外部物理中断触发到用户态 Dispatcher 线程进入处理函数第一行的时间 | 硬件 trap 入口、内核边沿记录、futex wake 与用户态线程调度综合耗时 |
| `syscall_latency` | 用户态发起 `ecall` 到系统调用完成返回 U 态的执行耗时 | M 态上下文切换与内核系统调用分发表分发开销 |

## 2. 调度机制的理论上界分析

内核设计给出以下理论约束；4.8 的实测数据用于校验这些上界，而不是替代它们：

| 操作路径 | 理论上界与推导依据 | 机制说明 |
|---|---|---|
| **最小延时粒度** | 1 个硬件 tick（Hi3861: 10 ms；QEMU MCU: 1 ms） | `osDelay(1)` 严格对齐至下一个时钟中断周期 |
| **周期唤醒到期** | 1 个 tick + 单核上下文切换耗时 | 内核软件定时器最小堆按 tick 周期进行到期扫描 |
| **就绪抢占时延** | $\le$ 处于锁深度或不可中断临界区的最长耗时 | 高优先级任务就绪后，在最近的安全点（syscall 返回或中断返回）即可完成剥夺 |
| **中断到线程 Handler** | IRQ Trap 开销 + 1 次内核 futex wake + 1 次上下文切换 | 处理函数运行在独立 U 态线程，非原生硬件 ISR 上下文 |
| **`osKernelLock` 临界区** | 抢占被完全推迟，直至锁深度减为 0 | 硬件中断正常记录，所有抢占请求被延迟在安全点集中消费 |

## 3. 测量瓶颈与硬件计数器演进路径

### 为什么纯 Tick 无法评估时延抖动

MCU 构建的内核时钟仅由系统时钟中断步进，`a20_clock_get` 系统调用返回的分辨率即为 tick 本身（10 ms 或 1 ms）。使用基于 tick 的时间戳去测量任务唤醒，其量化误差（Quantization Error）远大于系统调度的真实微秒级抖动。

建立高精度时延基准必须依赖亚微秒级（Sub-microsecond）的硬件时间源。系统规划两条演进路径：

### 已落地：用户态自由运行硬件计数器（软件测量路径）

1. **内核只读计数器原语**：U 态不直接执行 `csrr cycle`；现有 `clock_get` 扩展 clock id 7/8，由内核在 M-mode 读取 `rdcycle` 与频率后返回。这样不新增 syscall，也不依赖 U 态 CSR 权限。
2. **测量时钟**：QEMU MCU 的 `rdcycle` 按 1 GHz 标注；Hi3861 为 160 MHz。两者都远高于 tick 频率，足以观察 tick 对齐误差。
3. **实验 4.8 已实现的两项测量**：

   ```text
   tick jitter:
     t0 = raw_cycle(); osDelay(1); t1 = raw_cycle();
     jitter = abs((t1 - t0) - tick_cycles)
   syscall latency:
     t0 = raw_cycle(); osKernelGetTickCount(); t1 = raw_cycle();
     latency = t1 - t0
   ```

   当前 demo 默认采样 32 次以适配 MCU 共享堆；正式基线可用 `-DLAT_SAMPLES=10000` 扩大样本，并记录空载/忙等/IPC 三种背景。把 `preempt_latency` 塞进同一镜像会越过 MCU 堆的区域边界，导致 4.6/4.9 失败，因此该测量保留在独立镜像。

### 已落地：AArch64 独立时延探针

`/bin/native-latency`（源码 `user/tests/test_native_latency.c`，随 `RAMFS_NATIVE_TESTS=1` 构建）使用同一组 `clock_get` 扩展：

```text
wake-to-run:
  coordinator: before = raw_cycle(); futex_wake(go)
  RT thread:   after = raw_cycle()   /* 第一条用户指令处 */
  latency = after - before
timer IRQ entry:
  kernel: deadline = CNTP_CVAL; now = CNTPCT; delay = now - deadline
          （在 timer_set_interval() 改写 compare 之前采样）
```

```sh
make ARCH=aarch64 BOARD=qemu-virt-aarch64 RAMFS_USER=1 \
     RAMFS_NATIVE_TESTS=1 NR_CPUS=2 kernel-only -j4
# guest: /bin/native-latency
# NATIVE_LATENCY: wake min=... avg=... max=... count=200 cycles
# NATIVE_LATENCY: timer_irq min=... avg=... max=... count=... cycles
# NATIVE_LATENCY: PASS hz=62500000 wake_avg_ns=... timer_irq_samples=...
```

说明与边界：

- `wake-to-run` 的协调者与探测线程固定在同一 CPU，探测线程优先级更高，因此区间包含真实的优先级抢占，而不只是 FIFO 队尾轮转。
- `timer_irq_entry` 在 AArch64 的 `handle_timer_irq()` 最前端采样，比较的是触发本次中断的 `CNTP_CVAL_EL0` 与 `CNTPCT_EL0`；统计是自启动以来的累计值，且只覆盖 timer IRQ。
- QEMU 上读到的是模拟 CPU 的计数，不代表任何具体硅片的缓存/总线行为；Hi3861 的 timer IRQ 入口与 U 态 Dispatcher 端到端仍需实板 GPIO/示波器路径。

### 路径 B：GPIO 物理引脚翻转（硬件示波器测量路径）

针对 Hi3861 实体靶板，利用逻辑分析仪或示波器消除任何软件插桩带来的观测者效应（Probe Effect）：

1. **引脚配置**：指定通用 GPIO（如 GPIO7）为测量标记引脚。
2. **时延捕获协议**：
   - 在触发硬件测试信号（或进入底层 M 态 trap）瞬间将 GPIO 置为高电平；
   - 在用户态 Dispatcher 线程进入 `handler` 的首行代码将 GPIO 置为低电平；
   - 示波器直接测量脉冲高电平宽度，作为最真实的物理端到端 `irq_to_handler` 耗时。

## 4. 基准数据归档与验收准则

所有实测数据产出后，应遵循以下工程归档规范：

- 明确记录测试硬件配置（Hi3861 硅片版本 / QEMU 版本）、CPU 主频（160 MHz / 虚拟时钟）、tick 设定值与编译优化等级（`-Os`）；
- 标明背景负载条件与测量样本总量（$N \ge 10,000$）；
- 验收基线目标：
  - 空载下 `tick_jitter` 的 p99 必须控制在单次 tick 的 5% 以内；
  - 高优先级线程 `preempt_latency` 的 p99 必须在微秒级，显著小于单个硬件 tick；
  - 完整原始串口数据日志留存于 `docs/status/` 测量归档目录。

## 5. WCET 边界

A20OS **不提供 WCET 形式化证明**。实验 4.8 的 max 值只是当前负载下观测到的上界样本，不能外推为芯片级最坏执行时间；以下因素仍未建模：

- 指令/数据 cache 与总线竞争；
- 最长不可抢占内核临界区、IRQ 嵌套和 DMA/网络干扰；
- MCU Flash 等待周期、软件原子操作的循环上界；
- 任务栈溢出/缺页等异常路径。

需要 WCET 的场景必须引入静态时序分析工具、芯片厂商的指令/存储时序模型，并保留可复现的测量环境（主频、tick、`-Os`、板级版本）。在此之前，4.8 的数据只能作为 **tick jitter / syscall latency 的回归基线**，不能作为硬实时截止期承诺。

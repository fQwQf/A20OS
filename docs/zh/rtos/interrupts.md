# 中断与 ISR 模型

本文说明 A20OS 在 NOMMU/U-mode 下如何处理中断，以及 CMSIS 应用能依赖什么、不能依赖什么。4.4 实验和 Hi3861 厂商扩展是这一模型的参考实现。

## 为什么用户态不能直接执行 ISR

MCU 镜像的用户程序运行在 U-mode，异常/中断入口在 M-mode 内核。用户态没有中断向量，也不能在中断上下文里执行任意 C 代码。因此 A20OS 不在用户态模拟“真正的中断服务例程”，而是：

```text
硬件/Timer IRQ
  → M-mode trap
  → 内核 vendor 模块记录边沿/计数
  → futex wake 用户态订阅字
  → 厂商 dispatcher 线程（普通 CMSIS 线程）
  → 应用 handler
```

handler 运行在线程上下文，因此可以使用 CMSIS 阻塞 API、互斥量、日志；代价是延迟包含一次调度。

## 4.4 的厂商模型

标准 CMSIS 库不包含任何 `hi_*`/`IoT*` 符号。厂商接口在独立目录：

```text
user/cmsis_rtos2/          标准 CMSIS-RTOS2
user/port/hi3861_hal/      hi_irq_* / IoTGpio* / IoTPwm*
kernel/arch/riscv32/mcu/vendor_hi.c
                           厂商 syscall 区间 0x7F00..，不进标准分发表
```

`hi_irq_request(irq, handler, arg)` 注册 handler，`hi_irq_enable/disable` 控制线路，`hi_irq_trigger` 在 QEMU 上产生测试边沿。dispatcher 线程以 `osPriorityAboveNormal` 创建，等待订阅字；内核 raise 后 futex wake，dispatcher 调用 handler。

厂商 syscall 在 `riscv_mcu_syscall()` 里先于标准表分流，所以 CMSIS 的 trim 检查不会把厂商调用算进 Native ABI。

## 临界区

`hi_int_lock()`/`hi_int_restore()` 不是关闭 CPU 中断，而是让厂商模块在锁内**延迟投递**：

- 边沿计数仍会记录；
- dispatcher 不会在锁内被唤醒；
- 解锁后补投递。

这避免用户在“临界区”里被 handler 重入，但不提供跨核或内核对中断的屏蔽。

## 延迟组成

一次 IRQ 到 handler 的路径包含：

1. 硬件/trap 入口；
2. 内核 vendor 模块记录 + futex wake；
3. 调度器选择 dispatcher 线程（它是 RT 线程，但仍受 tick 和当前任务影响）；
4. dispatcher 线程运行 handler。

因此延迟不是“几个微秒的 ISR”，而是“IRQ + 一次用户态调度”。具体上界取决于 tick、当前 RT 占用和是否在 `osKernelLock` 内；4.8 已给出 tick jitter 与 syscall latency 的周期计数基线，但 preempt/IRQ-to-handler 仍需独立测量（见 [实时时延分析与基准测量体系](latency.md)）。

## Tickless 与 PMP allow-list

MCU 定时器支持 one-shot 重装：`timer_set_interval()` 记录下一截止期，IRQ 返回前按 `proc_next_timer_interval()` 重新装载；`osKernelSuspend()` 在单核上以调度器锁 + WFI 使用同一条路径。IRQ 处理期间仍递增软件 tick（跨度超过 1 tick 时逐 tick 计入调度器）。

flat program 的镜像、程序栈和动态区被装成 PMP TOR 允许项，空隙显式拒绝；每次 trap 返回前按当前任务重建。U-mode 访问未注册地址进入 trap 后按 SIGSEGV 结束当前线程，而不是 panic 内核。M-mode 不受 PMP 检查，所以内核与 trap handler 仍可访问全部内存。

## 执行模型边界与限制

- **硬件中断上下文禁止调用 CMSIS API**：系统没有提供传统裸机环境下的 ISR 执行上下文；`osThreadFlagsSet` 等接口亦非 ISR-safe；
- **用户态不直接注册硬件向量**：异常/中断向量表由 M 态内核统一接管；
- **Handler 运行于线程上下文**：应用处理函数在普通线程中执行，可能被更高优先级的实时任务暂时推迟；
- **厂商专有调用独立隔离**：厂商 syscall（`0x7F00..`）走独立分发表，不混入标准 Native ABI 与 CMSIS 裁剪声明。

## 移植一个厂商 IRQ

1. 在 `user/port/<vendor>_hal/` 提供 `hi_irq_request/enable/disable` 等包装；
2. 在 `kernel/arch/<arch>/.../vendor_*.c` 增加厂商 syscall 分发和内核侧边沿记录；
3. 用 futex 字唤醒 dispatcher，不要在用户态轮询；
4. 把厂商头文件限制在 port 目录，`user/cmsis_rtos2/` 不 include；
5. 在实验中验证：注册、使能、触发/真实引脚、临界区延迟、禁用后静默、重新使能。

Hi3861 的 GPIO 中断还涉及 HimiDeer local vector 45 映射；当前 4.4 在 QEMU 上用 `hi_irq_trigger` 模拟边沿，真实引脚中断尚未在实板验收。

## 验证

```sh
tools/a20 test qemu-riscv32-mcu
# [4.4] PASS irq=5
```

4.4 检查：三次边沿到达 handler、临界区内延迟、禁用后静默、重新使能后恢复。QEMU 没有 GPIO，边沿由 `hi_irq_trigger()` 产生；Hi3861 上同一 raise 路径可由真实引脚中断驱动。

## 相关文档

- [执行模型](execution-model.md)
- [同步与优先级继承](synchronization.md)
- [实验 4.1–4.9](experiments.md)
- [实时时延分析与基准测量体系](latency.md)
- [Hi3861 平台页](../platforms/hi3861.md)

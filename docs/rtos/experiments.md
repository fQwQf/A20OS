# 实验 4.1–4.5

`user/tests/liteos-demos/` 是一份 RV32 M 态镜像，包含 LiteOS 教材实验 4.1–4.5。4.1–4.3 只用标准 `cmsis_os2.h`；4.4 增加 Hi3861 厂商 HAL；4.5 验证优先级、`osKernelLock`、PI 和链式 PI。

## 实验一览

| 实验 | 文件 | 内容 | 预期输出 |
|---|---|---|---|
| 4.1 线程 | `demo.c` | 创建/join、`osDelay`、两个线程交错 | `[4.1] PASS steps=5/5` |
| 4.2 信号量 | `demo.c` | 生产者/消费者、计数信号量 | `[4.2] PASS taken=5 produced=5` |
| 4.3 定时器 | `demo.c` | 单次/周期软件定时器 | `[4.3] PASS once=1 periodic=12` |
| 4.4 中断 | `interrupt.c` | 厂商 IRQ、GPIO/PWM、临界区延迟 | `[4.4] PASS irq=5` |
| 4.5 实时性 | `rt.c` | 创建优先级、SetPriority、`osKernelLock`、PI、链式 PI、初始线程 PI | `[4.5] PASS create=1 setprio=1 lock=1 inherit=1 chain=1 init=1` |

最终 launcher 输出：

```text
[DEMO] experiments 4.1-4.5: PASS (checks=58, heap-free=...)
```

## QEMU

```sh
tools/a20 test qemu-riscv32-mcu
```

它会构建内核和 flat-program 镜像、启动 QEMU、等待 `[MCU TEST] PASS`、`A20OS MCU READY` 和 `[DEMO] experiments 4.1-4.5: PASS`。实例声明在 `instances/qemu-riscv32-mcu.toml`。

QEMU 板 tick 为 1 kHz；4.4 没有 GPIO，边沿由 `hi_irq_trigger()` 产生。

## Hi3861

默认镜像不启动 demo，先保持串口 shell。确认 U-mode 行为后：

```sh
tools/a20 package hi3861 -- HI3861_LITEOS_DEMO=1
tools/a20 flash   hi3861 -- HI3861_LITEOS_DEMO=1
tools/a20 console hi3861 --serial /dev/ttyUSB0
```

预期串口输出包含 `[4.1]`–`[4.5]` PASS 和 `[DEMO] experiments 4.1-4.5: PASS`。烧录只写 kernel A（`0xD000`），不会动 FlashBoot/NV；不带 `HI3861_LITEOS_DEMO=1` 重新构建即可回到纯 shell 镜像。

Hi3861 tick 为 100 Hz，`osDelay` 粒度为 10 ms。4.4 的真实引脚中断尚未在实板验收；当前 raise 路径可由 GPIO 中断驱动，但 QEMU 用的是测试 trigger。

## 4.5 的六个子项

| 子项 | 验证点 | 失败时说明 |
|---|---|---|
| `create-priority` | 新线程在 publish 前拿到 High/Low；高优先级等待者先运行 | 创建优先级未下发，或唤醒未按 RT 队列 |
| `setprio` | `osThreadSetPriority` 在阻塞线程上生效 | `task_set_sched` 未带 policy/priority |
| `lock` | `osKernelLock` 推迟更高优先级线程，解锁后立即运行 | `sched_lock_depth` 未被安全点检查 |
| `inherit` | 单级 mutex PI，medium hog 不进入临界区 | 内核捐赠未生效 |
| `chain` | A→B→C 链式捐赠，B/C 的有效优先级都到 High | 人格层未遍历 `wait_owner` |
| `init` | main 线程作为 PI 目标 | `task_self` 未安装 handle |

这六项都做过负向对照：分别让创建 sched、`proc_sched_rt_donate`、`sched_lock` 检查、链式 walk、`task_self` 失效，对应子项会失败。

## 增加一个实验

1. 在 `user/tests/liteos-demos/` 新建 `.c`，只 include `cmsis_os2.h`（厂商实验再 include port 头文件）；
2. 用 `demo_log` 输出，用 `demo_check` 计数失败；返回 0/1；
3. 在 `demo.c` 声明并调用；
4. 把源文件加入 `tools/targets-images.mk` 的 `MCU_DEMO_SRC`；
5. 如果改了最终 `[DEMO] experiments ...` 文本，同步 `instances/*.toml` 的 `expect`；
6. 静态栈优先（demo 堆是共享 130 KiB arena），等待用 `osFlagsNoClear` 避免多个等待者抢同一个 event bit。

## 测试门禁

```sh
make -C user/cmsis_rtos2 test                     # 对象语义，POSIX + Native 宿主
tools/a20 test qemu-riscv32-mcu                   # 4.1–4.5 调度语义
python3 tools/cmsis_trim.py check --profile mcu --elf <kernel.elf>
python3 -m unittest tools.tests.test_cmsis_trim
make check-manifests
```

宿主测试不验证优先级（pthread 策略由宿主决定）；调度语义以 QEMU 4.5 为准。

## 相关文档

- [执行模型](execution-model.md)
- [同步与优先级继承](synchronization.md)
- [中断与 ISR 模型](interrupts.md)
- [移植与配置](porting-guide.md)
- [demo README](../../user/tests/liteos-demos/README.md)

# 实验 4.1–4.9

`user/tests/liteos-demos/` 是一份 RV32 M 态镜像，包含 LiteOS 教材实验 4.1–4.5 以及 RTOS 完善阶段新增的 4.6–4.9。4.1–4.3 只用标准 `cmsis_os2.h`；4.4 增加 Hi3861 厂商 HAL；4.5 验证优先级、`osKernelLock`、PI 和链式 PI；4.6 验证跨线程 suspend/resume；4.7 验证 `osKernelSuspend` 的 tickless WFI；4.8 用自由运行周期计数器测量 tick 抖动与系统调用时延；4.9 用 PMP allow-list 做越界访问的负向隔离测试。

## 实验一览

| 实验 | 文件 | 内容 | 预期输出 |
|---|---|---|---|
| 4.1 线程 | `demo.c` | 创建/join、`osDelay`、两个线程交错 | `[4.1] PASS steps=5/5` |
| 4.2 信号量 | `demo.c` | 生产者/消费者、计数信号量 | `[4.2] PASS taken=5 produced=5` |
| 4.3 定时器 | `demo.c` | 单次/周期软件定时器 | `[4.3] PASS once=1 periodic=12` |
| 4.4 中断 | `interrupt.c` | 厂商 IRQ、GPIO/PWM、临界区延迟 | `[4.4] PASS irq=5` |
| 4.5 实时性 | `rt.c` | 创建优先级、SetPriority、`osKernelLock`、PI、链式 PI、初始线程 PI | `[4.5] PASS create=1 setprio=1 lock=1 inherit=1 chain=1 init=1` |
| 4.6 挂起/恢复 | `rt.c` | 跨线程 suspend/resume；挂起期间计数冻结，恢复后继续 | `[4.6] PASS suspend=1 resume=1` |
| 4.7 tickless | `rt.c` | `osKernelSuspend` 进入调度器锁定的 WFI，`osKernelResume` 恢复 | `[4.7] PASS slept=<N> elapsed=<N>` |
| 4.8 时延 | `latency.c` | `clock_get` 原始周期计数器；`osDelay` tick 抖动与 syscall 时延 | `[4.8] PASS delay=<cycles> syscall=<cycles>` |
| 4.9 隔离 | `rt.c` | `vm_unmap` 后页面从 PMP allow-list 移除；写入触发 SIGSEGV 只结束探测线程 | `[4.9] PASS good=1 entered=1 survived=0` |

最终 launcher 输出：

```text
[DEMO] experiments 4.1-4.9: PASS (checks=77, heap-free=...)
```

## QEMU

```sh
tools/a20 test qemu-riscv32-mcu
```

它会构建内核和 flat-program 镜像、启动 QEMU、等待 `[MCU TEST] PASS`、`A20OS MCU READY` 和 `[DEMO] experiments 4.1-4.9: PASS`。实例声明在 `instances/qemu-riscv32-mcu.toml`。

QEMU 板 tick 为 1 kHz；4.4 没有 GPIO，边沿由 `hi_irq_trigger()` 产生。

## Hi3861

默认镜像不启动 demo，先保持串口 shell。确认 U-mode 行为后：

```sh
tools/a20 package hi3861 -- HI3861_LITEOS_DEMO=1
tools/a20 flash   hi3861 -- HI3861_LITEOS_DEMO=1
tools/a20 console hi3861 --serial /dev/ttyUSB0
```

预期串口输出包含 `[4.1]`–`[4.9]` PASS 和 `[DEMO] experiments 4.1-4.9: PASS`。烧录只写 kernel A（`0xD000`），不会动 FlashBoot/NV；不带 `HI3861_LITEOS_DEMO=1` 重新构建即可回到纯 shell 镜像。

Hi3861 tick 为 100 Hz，`osDelay` 粒度为 10 ms。4.4 的真实引脚中断尚未在实板验收；当前 raise 路径可由 GPIO 中断驱动，但 QEMU 用的是测试 trigger。4.6–4.9 当前验证基准是 QEMU MCU；Hi3861 的 one-shot 定时器重装和 PMP allow-list 只做了代码/静态审查，需在实板补启动日志。

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

## 4.6 跨线程 suspend/resume

`osThreadSuspend(t)` 由内核设置 `suspend_pending`，目标在下一个 syscall/IRQ 返回安全点进入 `PROC_STOPPED`；如果目标正在等待，唤醒路径先发布 STOPPED 而不是 READY。`osThreadResume(t)` 清除 pending，把 CMSIS-suspended 任务重新入 RT 队列。实验在目标线程 `osDelay` 时挂起，验证计数冻结，再恢复并验证继续运行。

## 4.7 osKernelSuspend / Resume

MCU 单核上 `osKernelSuspend()` 通过 `kernel_idle` syscall 获取当前任务的调度器锁，计算下一内核截止期，one-shot 重装定时器并执行 `WFI`；期间定时器/UART IRQ 仍响应，但 IRQ 返回安全点因 `sched_lock_depth` 非零而不切换任务。返回实际经过的 tick，`osKernelResume()` 恢复人格层状态。SMP 和 hosted 后端返回 0，不进入低功耗。

## 4.9 PMP allow-list 隔离

内核不再只装一个栈底 guard，而是把当前 flat program 的每个已注册区间（镜像、程序栈、`vm_alloc` 动态区）都变成一个 PMP TOR 允许项，区间之间的空隙显式拒绝。M-mode 不受 PMP 约束，所以内核和 trap handler 仍能访问全部内存；U-mode 访问未注册地址会触发 fault，按 SIGSEGV 结束当前线程。

4.9 先写一个已映射的动态页，再 `vm_unmap` 它（从 flat space 移除），然后让探测线程写这个页。期望：已映射页写入成功；撤销后的页触发 fault；探测线程没有继续执行；demo 主程序继续运行并打印 PASS。这验证的是“撤销即拒绝”的真实硬件边界，而不是仅靠 syscall 参数校验。

## 4.8 时延测量

`clock_get` 的 clock id 7/8 返回自由运行周期计数器和频率，不增加 MCU syscall 数量。实验测量：

- `osDelay(1)` 相对单 tick 周期数的偏差（tick jitter）；
- `osKernelGetTickCount()` 系统调用往返周期数（syscall latency）。

输出 min/avg/max，单位是 CPU cycles。QEMU 的 `rdcycle` 频率按 1 GHz 标注；Hi3861 为 160 MHz。该实验只测量，不构成 WCET 证明。

`preempt_latency` 与 timer IRQ 入口时延不放进这个 MCU 镜像，而是由 AArch64 的 `/bin/native-latency` 独立测量（详见 [latency.md](latency.md)）。在 4.8 同一镜像里再放一个 RT 线程会越过共享堆的区域边界并挤掉 4.6/4.9，因此两者保持分离。

## 增加一个实验

1. 在 `user/tests/liteos-demos/` 新建 `.c`，只 include `cmsis_os2.h`（厂商实验再 include port 头文件）；
2. 用 `demo_log` 输出，用 `demo_check` 计数失败；返回 0/1；
3. 在 `demo.c` 声明并调用；
4. 把源文件加入 `tools/mk/targets-images.mk` 的 `MCU_DEMO_SRC`；
5. 如果改了最终 `[DEMO] experiments ...` 文本，同步 `instances/*.toml` 的 `expect`；
6. 静态栈优先（demo 堆是共享 130 KiB arena），等待用 `osFlagsNoClear` 避免多个等待者抢同一个 event bit。

## 测试门禁

```sh
make -C user/cmsis_rtos2 test                     # 对象语义，POSIX + Native 宿主
tools/a20 test qemu-riscv32-mcu                   # 4.1–4.9 调度/时延语义
python3 tools/cmsis_trim.py check --profile mcu --elf <kernel.elf>
python3 -m unittest tools.tests.test_cmsis_trim
make check-manifests
```

宿主测试不验证优先级（pthread 策略由宿主决定）；调度语义以 QEMU 4.5–4.9 为准。宿主回归同时覆盖 `msg_prio` 排序、`osThreadGetAffinity`、`osThreadGetStackSpace` 高水位与跨线程 suspend/resume。CI 的 `host-tests` job 直接运行 `make -C user/cmsis_rtos2 test`、`trim PROFILE=mcu` 与 `tools.tests.test_cmsis_trim`。

SMP 侧已用 AArch64 `NR_CPUS=2`、`RAMFS_USER=1 RAMFS_RT_TESTS=1` 启动 `/bin/sched_stress`，`smp-runqueue`（迁移/IPI）、`lock-split` 和 `deadline` 均 PASS；这验证 RT runqueue 与 `SCHED_DEADLINE` 的 SMP 结构路径。`RAMFS_NATIVE_TESTS=1` 的 `/bin/native-futex` 还包含加强后的 `NATIVE_SMP_PI`：owner 固定在 CPU0，两个不同优先级的捐赠者在 CPU1 并发各捐赠 64 次，验证多捐赠合并到最大值、链式 `priority=0` 传播、boosted owner 跨核迁移后优先级/亲和性不变，并确认提升后的 owner 真正抢占了原本饿死它的 CPU0 spinner。

```sh
make ARCH=aarch64 BOARD=qemu-virt-aarch64 RAMFS_USER=1 RAMFS_RT_TESTS=1 \
     NR_CPUS=2 kernel-only -j4
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 1G -nographic -smp 2 \
     -kernel <build>/kernel.elf
# 在 guest shell 运行 /bin/sched_stress
```

跨核 RT PI 用同一 SMP2 guest 的 Native 运行时验证：

```sh
make ARCH=aarch64 BOARD=qemu-virt-aarch64 RAMFS_USER=1 \
     RAMFS_NATIVE_TESTS=1 NR_CPUS=2 kernel-only -j4
# 在 guest shell 运行 /bin/native-futex
# NATIVE_SMP_PI: PASS donors=2 prio=60/10 chain=50 migrate=1 preempt=starved->runs lock=progress
```

`NATIVE_SMP_PI` 覆盖四个语义：

1. **多捐赠合并**：owner 基准 FIFO 10；30 和 60 两个捐赠者各以 `priority=0` 并发捐赠 64 次，owner 的有效优先级必须稳定为最大值 60，较低捐赠不能覆盖较高值。
2. **真实抢占效果**：CPU0 上 prio-30 spinner 先让 prio-10 owner 完全得不到执行；提升到 60 后 owner 必须在同一窗口内开始推进，`UNDONATE` 后又回到被饿死状态。这验证的是调度结果，不是只读字段。
3. **跨核迁移**：owner 在 boosted 且已 park 的状态下从 CPU0 迁到 CPU1 再迁回，优先级 60 与亲和性掩码都必须保持不变。
4. **链式捐赠**：协调者把 mid 提到 50，mid 再用 `priority=0` 把自身的 50 下发给 low；恢复时逐级回到 20/10。
5. **per-task `sched_lock`**：CPU1 持有调度锁时 CPU0 的 spinner 仍推进。

独立时延探针（AArch64）与 4.8 分开构建，避免 MCU 堆边界问题：

```sh
# 同一 RAMFS_NATIVE_TESTS=1 镜像
# 在 guest shell 运行：
/bin/native-latency
# NATIVE_LATENCY: wake min=... avg=... max=... count=200 cycles
# NATIVE_LATENCY: timer_irq min=... avg=... max=... count=... cycles
# NATIVE_LATENCY: PASS hz=62500000 wake_avg_ns=... timer_irq_samples=...
```

`wake` 是 `futex_wake` 到同 CPU 更高优先级线程第一条用户指令的周期数；`timer_irq` 是内核在 `handle_timer_irq()` 入口用 `CNTP_CVAL_EL0` 与 `CNTPCT_EL0` 计算的 timer 中断入口时延。两者都是观测值，不是 WCET。

`/bin/native-futex` 在 SMP2 和单核上的完整序列现在都通过，包括 `NATIVE_COMPUTE_CANCEL`（纯用户循环的跨核取消）、`NATIVE_TASK_CONTROL`（线程级信号队列）、`NATIVE_SPAWN`、`NATIVE_ALLOCATOR` 与 `NATIVE_FUTEX`：

```text
NATIVE_SMP_PI: PASS donors=2 prio=60/10 chain=50 migrate=1 preempt=starved->runs lock=progress
NATIVE_COMPUTE_CANCEL: PASS workers=2 exit=137 pure-user=1
NATIVE_TASK_CONTROL: PASS signal=12 masked=1 exit=137
NATIVE_SPAWN: PASS children=2 exit=37 args=4 env=2 work=32
NATIVE_ALLOCATOR: PASS
NATIVE_FUTEX: PASS
```

这轮顺带修正了三处测试/ABI 对齐问题：需要 `task_wait` 的线程必须以 `A20_THREAD_CREATE_JOINABLE` 创建，否则退出时被 auto-reap；`native-spawn-child` 的 start-info 版本必须与内核的 `A20_START_INFO_VERSION` 一致；线程级信号必须使用 `a20_thread_signal_*`，不能与进程级 `a20_signal_*` 混用。

4.9 是唯一的负向用例：它用 `vm_unmap` 撤销页面后触发 U-mode fault。trap 路径按 SIGSEGV 结束探测线程，主程序继续运行，所以可以放在通过型回归里。若 fault 发生在主线程或镜像本身，仍会结束该 flat program，不能把这类用例混入常规路径。

## 相关文档

- [执行模型](execution-model.md)
- [同步与优先级继承](synchronization.md)
- [中断与 ISR 模型](interrupts.md)
- [移植与配置](porting-guide.md)
- [demo README](../../user/tests/liteos-demos/README.md)

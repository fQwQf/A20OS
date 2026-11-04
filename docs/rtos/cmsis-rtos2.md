# CMSIS-RTOS2 第三方 ABI 兼容层（personality）

> 实时子系统架构与微内核机制分工参见 [A20OS 实时执行模型与系统架构](overview.md)。本文是 CMSIS-RTOS2 人格层实现、API 支持矩阵与静态裁剪参考。

CMSIS-RTOS2 是 Arm 定义的**独立第三方 ABI**（source-level：头文件 + 库 + 语义），不是 A20OS Native ABI 的一部分，也不是第三种 syscall ABI。A20OS 在 `user/cmsis_rtos2/` 以用户态人格库实现它；Native ABI 只为“固定优先级 personality”增加了一小组通用调度原语（两个新 syscall + 两个既有接口的扩展），其中不包含任何 CMSIS 类型或 CMSIS 编号。

在 Native/MCU 后端下，CMSIS 线程不是“分时调度器上的一层线程库”：它们就是内核的 `SCHED_FIFO` 任务，具备固定优先级抢占、优先级继承和真正的 `osKernelLock`。时间粒度仍是内核 tick；具体保证与边界见下文“实时性模型”。

## 定位：第三方 ABI，不是 Native ABI

| | Linux ABI / Native ABI | CMSIS-RTOS2 |
|---|---|---|
| 归属 | A20OS 内核接口 | Arm 规范，A20OS 以人格库提供 |
| 形态 | syscall 编号 + 线格式 + 语义 | C 头文件 + 库 + 语义（source-level ABI） |
| 调用方式 | `ecall`/`svc`/`int` 陷入内核 | 普通函数调用，库内部再发起 syscall |
| 可移植性 | 预编译二进制可换内核 | 源码级可移植，必须链接实现 |
| 标准来源 | Linux / A20OS 自有规范 | Arm CMSIS 规范（事实标准，非 ISO） |

因此它**不能**被当作 Native ABI 的扩展章节；本文放在 `docs/rtos/`，Native ABI 文档只保留那些通用调度原语的 ABI 定义。

实现 CMSIS-RTOS2 实际用到的 Native ABI 表面：

| Native ABI 项 | 类型 | 用途 |
|---|---|---|
| `thread_create` 的 sched 追加字段 | 既有 syscall 的 E-APPEND 扩展 | 在 publish 前设置 CMSIS 优先级/策略 |
| `task_set_sched` 的 `A20_SCHED_DONATE` / `UNDONATE` | 既有 syscall 的新 flags | 内核 RT 优先级继承 |
| `task_get_sched` | 既有 syscall | `osThreadGetPriority` 读取有效优先级 |
| `sched_lock`（0x0219） | 新 syscall | `osKernelLock` 的 per-task 调度器锁 |
| `task_self`（0x021A） | 新 syscall | 初始线程安装自己的 handle，作为 PI 目标 |

这些原语是通用的：它们不依赖 `cmsis_os2.h`，也不进入 Native ABI 的 CMSIS 专属编号；未来其他 RTOS personality 可以复用同一组调度原语。

## 分层

```
应用（现有 CMSIS-RTOS2 源码，不修改）
        │  osThreadNew / osSemaphoreAcquire / osTimerStart / ...
        ▼
user/cmsis_rtos2        ← 本文
        │  后端契约（os_impl.h）：
        │  thread / clock / alloc / kernel-lock / blocking-queue
        ▼
Linux ABI（pthread + futex）   或   Native ABI（thread/sched/futex/atomic/vm_alloc）
        ▼
A20OS 内核机制（调度、futex、VM、定时器）
```

对象语义（互斥锁归属与递归、信号量计数、队列、事件标志、内存池、软件定时器）全部在可移植核心里实现。后端只提供四类原语，因此换 ABI 不需要改任何 `os*`函数：

1. 线程：创建/退出/join/detach/yield/睡眠/优先级/亲和性/调度器锁；
2. 时钟：单调纳秒；
3. 分配器；
4. 阻塞队列：互斥 + 定时等待（对象状态就由它保护）。

## API 覆盖

| 对象 | 接口 |
|---|---|
| Kernel | initialize/start/state/info/version、tick 与系统定时器时钟、lock/unlock/restore、suspend/resume、`osDelay`、`osDelayUntil` |
| Thread | create/exit/join/detach/terminate/suspend/resume、优先级、名称、状态、count/enumerate、亲和性、栈大小/剩余 |
| Thread flags | set/get/clear/wait（any/all/no-clear、超时） |
| Timer | once/periodic、start/stop/is-running/delete，回调在专用服务线程执行 |
| Mutex | 归属、递归、定时获取、删除 |
| Semaphore | 计数、上限、定时获取、get-count |
| Event flags | set/clear/get/wait（any/all/no-clear） |
| Message queue | FIFO、阻塞 put/get、capacity/space/reset |
| Memory pool | 固定块、阻塞分配、双重释放检测 |

控制块支持 `cb_mem`/`cb_size` 静态放置；线程支持 `stack_mem`/`stack_size`。

执行、抢占、tick、PI、`osKernelLock` 的完整语义在独立页面：

- [执行模型](execution-model.md)：线程→内核 RT 任务的映射、优先级表、publish 前设置、syscall/IRQ 返回点抢占、broadcast wake、tick、`osKernelLock`、保证/不保证；
- [同步与优先级继承](synchronization.md)：mutex/semaphore/event/queue/pool、`wait_owner` 链、内核捐赠/恢复、超时、已知边界；
- [中断与 ISR 模型](interrupts.md)：为什么用户态不能直接执行 ISR、4.4 厂商 dispatcher、临界区、移植厂商 IRQ。

简表：

| 属性 | Native/MCU 后端 |
|---|---|
| 调度类 | `SCHED_FIFO`，CMSIS 优先级 1..55 直接等于内核 RT 优先级 |
| 创建 | `thread_create` sched 字段在 publish 前应用 |
| 抢占 | syscall / IRQ 返回 safe point，不等 tick |
| 唤醒 | broadcast + RT 队列选最高 |
| PI | 内核 `proc_sched_rt_donate/restore`，人格层遍历 `wait_owner` 链 |
| `osKernelLock` | 内核 per-task `sched_lock_depth` |
| tick | Hi3861 100 Hz / QEMU 1 kHz；`OS_NATIVE_TICK_HZ` 必须匹配 `ARCH_TIMER_FREQ` |
| RT 带宽 | hosted 95%/秒；MCU 构建关闭 |

POSIX 后端不在实时保证范围内。

## 裁剪

`user/cmsis_rtos2/cmsis_rtos2.toml` 是唯一事实来源，声明每个 profile 保留哪些对象族、需要哪些 Native syscall。`tools/cmsis_trim.py` 用同一份声明做三件事：

```sh
# 1. 生成特性头和 syscall 表子集
python3 tools/cmsis_trim.py emit --profile mcu --out build/trim-mcu
# 2. 校验内核 ELF：不得链接 profile 之外的 sys_a20_*，也不得缺少声明项
python3 tools/cmsis_trim.py check --profile mcu --elf build/kernel.elf
# 3. 查看 profile
python3 tools/cmsis_trim.py list --profile mcu
```

生成物：

- `cmsis_config.h`：`OS_CFG_*` 宏，决定哪些对象族进入库；
- `syscall_table_<profile>.def`：主表 `kernel/abi/native/syscall_table.def`的子集。构建时用`-DA20_NATIVE_SYSCALL_TABLE='"syscall_table_<profile>.def"'` 覆盖主表即可，调度器对未命中的编号本来就返回 `-NOT_SUPPORTED`，不需要第二套分发器。

| Profile | 对象族 | syscall 数 |
|---|---|---|
| `full` | 全部 | 主表全部 |
| `mcu` | thread、mutex、semaphore、timer、event flags、message queue | 15 |
| `mcu-min` | thread、semaphore、timer（不含 join/terminate/priority 的最小集合） | 9 |

被裁掉的对象族仍保留入口（返回 `osErrorResource`），因此"链接兼容、镜像裁剪"：

- 不对头文件分叉；
- 不会因为少一个对象族而让应用链接失败；
- 不会把未使用的内核 syscall 带进镜像。

在 riscv32imc/ilp32、`-Os`、`-ffunction-sections`（链接 GC 前）下测得库 text：`full` 12.5 KiB、`mcu` 11.8 KiB、`mcu-min` 10.6 KiB。

`check` 是裁剪的闭环：它读 ELF 的 `sys_a20_*` 符号集合，与 manifest 比较，多一个或少一个都失败。因此"镜像里只有声明的 syscall"由构建检查保证，而不是靠代码评审。

## 验证

```sh
make -C user/cmsis_rtos2 test                     # POSIX + Native 两个后端
make -C user/cmsis_rtos2 freestanding PROFILE=mcu-min CC_RISCV32=<prefix>
python3 -m unittest tools.tests.test_cmsis_trim   # manifest/生成器/校验器
```

同一套行为用例跑两遍：

- POSIX 后端：pthread + condvar；
- Native 后端：后端本身（futex 队列、线程簿记、栈归属）运行在`tests/native_platform_host.c` 提供的宿主实现上，平台调用用 pthread 和 Linux futex 实现。

Native 运行结束会输出 `osn-used:...`。单元测试另外把`src/native_platform_a20.c` 里的 `a20_*` 调用集合与 `mcu` profile 的 syscall 清单逐项比对，因此"后端实际用到的 syscall"和"manifest 声明的 syscall"不会漂移。

用例覆盖线程与 join、信号量、递归互斥（4 个竞争者）、事件/线程标志、队列满/空/reset、内存池容量与双重释放、周期与单次软件定时器。宿主用例验证对象语义；**优先级、PI、`osKernelLock` 的调度语义由 QEMU MCU 的 4.5 实验验证**，因为宿主 pthread 不保证 RT 策略。

`osThreadJoin` 在 Native 后端使用线程控制块里的 **exit_word**（futex 唤醒）等待，而不是内核 `task_wait`：A20 的 wait4 模型描述的是进程子任务，兄弟线程不是可等待子任务。该决定删除了 `task_wait` 这个 syscall，manifest、生成的分发表和内核镜像随之一起缩小。

## 在 RV32 M 态镜像里运行

QEMU 板 `qemu-virt-riscv32-mcu` 与 Hi3861 使用同一条路径：

1. 用户实验链接成 **ET_DYN 静态 PIE**，只允许 `R_RISCV_RELATIVE`；
2. `kernel/arch/riscv32/mcu/program.c` 校验并装载 ELF，登记`a20_flat_space_t`（text/stack），`mret` 到 U-mode；
3. `ecall` 进入 M 态 `riscv_mcu_syscall()`，按`syscall_table_mcu.def`（由本 manifest 生成）分发；
4. 每个线程首次运行 `arch_flat_program_enter()`，把自己的内核栈顶写入`mscratch`，从 U 态陷入时硬件栈切换回该内核栈；
5. 实验输出写入栈上的 run record，由内核 launcher 排空并打印，用户程序不需要非 CMSIS 的控制台 API。

实验镜像、4.1–4.5 的预期输出、Hi3861 烧录和负向对照见 [实验 4.1–4.5](experiments.md)。4.4 的厂商 dispatcher 模型见 [中断与 ISR 模型](interrupts.md)；厂商接口位于独立目录，不进入标准 CMSIS 库：

```text
user/cmsis_rtos2/          标准 CMSIS-RTOS2（不出现 hi_*/IoT*）
user/port/hi3861_hal/      厂商扩展（hi_irq_* / IoTGpio* / IoTPwm*）
kernel/arch/riscv32/mcu/vendor_hi.c
                           厂商 syscall 区间 0x7F00..，不进标准分发表
```

## 验收

```sh
# 宿主对象语义（POSIX + Native 后端）
make -C user/cmsis_rtos2 test

# RV32 M 态 4.1–4.5
tools/a20 test qemu-riscv32-mcu

# 裁剪闭环
python3 tools/cmsis_trim.py check --profile mcu --elf <kernel.elf>
```

调度语义的预期输出与失败语义见 [实验 4.1–4.5](experiments.md)。

Hi3861 构建已编入同一套 flat-program 基础设施与 syscall 子集；实验镜像默认不在实板启动，确认 U-mode 后用`tools/a20 package hi3861 -- HI3861_LITEOS_DEMO=1` 构建、用同样的 make 参数烧录即可。烧录只写 kernel A，不带参数重新构建会回到纯 shell 镜像。

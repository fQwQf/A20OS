# CMSIS-RTOS2 兼容层（personality）

本文说明 A20OS 如何提供 Arm CMSIS-RTOS2 API。结论先行：**它是一套用户态兼容库，不是第三种 syscall ABI**。内核不新增 CMSIS 编号，`os*` 函数位于`user/cmsis_rtos2/`，通过一个很窄的后端接口落到 Linux ABI 或 Native ABI 上。

## 为什么不是"第三个 ABI"

| | Linux ABI / Native ABI | CMSIS-RTOS2 |
|---|---|---|
| 形态 | syscall 编号 + 线格式 + 语义 | C 头文件 + 库 + 语义 |
| 调用方式 | `ecall`/`svc`/`int` 陷入内核 | 普通函数调用，库内部再发起 syscall |
| 可移植性 | 预编译二进制可换内核 | 源码级可移植，必须链接实现 |
| 标准来源 | Linux / A20OS 自有规范 | Arm CMSIS 规范（事实标准，非 ISO） |

CMSIS-RTOS2 没有标准的 trap 编号或二进制接口，把它做成 syscall ABI 只能自造一套编号，收益是把"源码可移植"降级成"并不存在的二进制兼容"。因此本层遵循`docs/architecture.md` 的分层原则：内部实现与 ABI 无关，人格层在用户态。

## 分层

```
应用（现有 CMSIS-RTOS2 源码，不修改）
        │  osThreadNew / osSemaphoreAcquire / osTimerStart / ...
        ▼
user/cmsis_rtos2        ← 本文
        │  后端契约（os_impl.h）：
        │  thread / clock / alloc / kernel-lock / blocking-queue
        ▼
Linux ABI（pthread + futex）   或   Native ABI（thread/futex/atomic/vm_alloc）
        ▼
A20OS 内核机制（调度、futex、VM、定时器）
```

对象语义（互斥锁归属与递归、信号量计数、队列、事件标志、内存池、软件定时器）全部在可移植核心里实现。后端只提供四类原语，因此换 ABI 不需要改任何 `os*`函数：

1. 线程：创建/退出/join/detach/yield/睡眠/优先级/亲和性；
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

已知边界：

- `osThreadSuspend/Resume` 在 POSIX 后端是协作式（信号触发），Native 后端不支持并返回 `osError`；
- POSIX 后端的 `osKernelLock` 是递归互斥，不能阻止显式阻塞调用——在锁内阻塞属于应用错误；
- 消息队列为 FIFO，`msg_prio` 接受但忽略；
- `osMutexPrioInherit` 只会被记录，是否生效取决于后端调度器。

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
| `mcu` | thread、mutex、semaphore、timer、event flags、message queue | 12 |
| `mcu-min` | thread、semaphore、timer（不含 join/terminate/priority 的最小集合） | 9 |

被裁掉的对象族仍保留入口（返回 `osErrorResource`），因此"链接兼容、镜像裁剪"：

- 不对头文件分叉；
- 不会因为少一个对象族而让应用链接失败；
- 不会把未使用的内核 syscall 带进镜像。

在 riscv32imc/ilp32、`-Os`、`-ffunction-sections`（链接 GC 前）下测得库 text：`full` 11.7 KiB、`mcu` 11.0 KiB、`mcu-min` 9.8 KiB。

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

用例覆盖线程与 join、信号量、递归互斥（4 个竞争者）、事件/线程标志、队列满/空/reset、内存池容量与双重释放、周期与单次软件定时器。

`osThreadJoin` 在 Native 后端使用线程控制块里的 **exit_word**（futex 唤醒）等待，而不是内核 `task_wait`：A20 的 wait4 模型描述的是进程子任务，兄弟线程不是可等待子任务。该决定删除了 `task_wait` 这个 syscall，manifest、生成的分发表和内核镜像随之一起缩小。

## 在 RV32 M 态镜像里运行

QEMU 板 `qemu-virt-riscv32-mcu` 与 Hi3861 使用同一条路径：

1. 用户实验链接成 **ET_DYN 静态 PIE**，只允许 `R_RISCV_RELATIVE`；
2. `kernel/arch/riscv32/mcu/program.c` 校验并装载 ELF，登记`a20_flat_space_t`（text/stack），`mret` 到 U-mode；
3. `ecall` 进入 M 态 `riscv_mcu_syscall()`，按`syscall_table_mcu.def`（由本 manifest 生成）分发；
4. 每个线程首次运行 `arch_flat_program_enter()`，把自己的内核栈顶写入`mscratch`，从 U 态陷入时硬件栈切换回该内核栈；
5. 实验输出写入栈上的 run record，由内核 launcher 排空并打印，用户程序不需要非 CMSIS 的控制台 API。

实验源码在 `user/tests/liteos-demos/`，这是一份镜像、四段实验：

| 文件 | 内容 |
|---|---|
| `demo.c` | 4.1 线程、4.2 信号量、4.3 软件定时器（只用 `cmsis_os2.h`） |
| `interrupt.c` | 4.4 中断，只用 `cmsis_os2.h` + `hi_isr.h`/`iot_gpio.h`/`iot_pwm.h` |
| `runtime.c` | freestanding 启动、日志环、64 位除法 |
| `start.c` | `_start`，a0 = run record |

4.4 的厂商接口**不进入标准库**：

```
user/cmsis_rtos2/          标准 CMSIS-RTOS2（不出现 hi_*/IoT*）
user/port/hi3861_hal/      厂商扩展（hi_irq_* / IoTGpio* / IoTPwm*）
kernel/arch/riscv32/mcu/vendor_hi.c
                           厂商 syscall 区间 0x7F00..，不进标准分发表
```

厂商区间由 `riscv_mcu_syscall()` 在标准查表之前分流，因此 CMSIS 的裁剪检查不可能把厂商调用算进 Native ABI。中断模型是"内核 raise → 唤醒用户 futex word → 厂商 dispatcher 线程执行 handler"：用户态不能真正执行 ISR，延迟处理是 NOMMU 用户驱动的唯一正确形态；QEMU 没有 GPIO，实验用`hi_irq_trigger()` 产生边沿，Hi3861 上同一 raise 路径可由真实引脚中断驱动。

## 验收

```sh
# 宿主行为用例（POSIX + Native 两个后端）
make -C user/cmsis_rtos2 test

# RV32 M 态实跑：内核自检 + 4.1/4.2/4.3/4.4
tools/a20 test qemu-riscv32-mcu
#   [4.1] PASS steps=5/5
#   [4.2] PASS taken=5 produced=5
#   [4.3] PASS once=1 periodic=12
#   [4.4] PASS irq=5
#   [DEMO] experiments 4.1-4.4: PASS

# 裁剪闭环：内核 ELF 只含 manifest 声明的 syscall
python3 tools/cmsis_trim.py check --profile mcu --elf <kernel.elf>
```

Hi3861 构建已编入同一套 flat-program 基础设施与 syscall 子集；实验镜像默认不在实板启动，确认 U-mode 后用`tools/a20 package hi3861 -- HI3861_LITEOS_DEMO=1` 构建、用同样的 make 参数烧录即可。烧录只写 kernel A，不带参数重新构建会回到纯 shell 镜像。

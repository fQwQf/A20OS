# 实时应用移植与配置指南

本文档面向将现有 CMSIS-RTOS2 或 LiteOS 应用程序接入/移植至 A20OS 的工程实践：包含最小应用模板、后端选择、裁剪配置、时钟同步、内存规划、构建链接与 API 对照表。

## 1. 最小应用实现模板

```c
#include "cmsis_os2.h"

static void control_task(void *argument)
{
    (void)argument;
    for (;;) {
        /* 执行周期采样或控制算法 */
        osDelay(1);  /* 在 Hi3861 (100 Hz) 上对齐至 10 ms；QEMU MCU (1 kHz) 对齐至 1 ms */
    }
}

int app_main(void)
{
    /* 1. 初始化用户态人格库；若在初始任务中执行，将通过 task_self 安装自身句柄 */
    if (osKernelInitialize() != osOK)
        return -1;

    /* 2. 创建高优先级工作线程，显式配置小栈以节约 SRAM */
    const osThreadAttr_t attr = {
        .name = "control",
        .priority = osPriorityHigh,  /* 对应内核 RT 优先级 40 */
        .stack_size = 2048,          /* 显式限制栈大小为 2 KiB */
    };
    osThreadId_t tid = osThreadNew(control_task, NULL, &attr);
    if (!tid)
        return -1;

    /* 3. 启动 CMSIS 定时器服务线程；A20OS 内核早已运行，此调用配置完定时器后立即返回 */
    if (osKernelStart() != osOK)
        return -1;

    /* 4. 主任务进入低频监控或等待事件 */
    for (;;)
        osDelay(100);
}
```

> **生命周期差异提示**：在 A20OS 上，微内核调度器在加载 flat program 前已经处于运行状态。`osKernelStart()` 仅启动 CMSIS 内部的软件定时器服务线程并切换人格层状态，**函数会立即返回**，不会阻塞主线程。

## 2. 后端与 Profile 选取

### 后端类型

| 后端 | 目标运行环境 | 调度能力 |
|---|---|---|
| **Native** | Hi3861 硬件靶板、`qemu-virt-riscv32-mcu`、RV32 静态 PIE flat program | 内核 `SCHED_FIFO` 固定优先级抢占 + 优先级继承 + 调度器锁 |
| **POSIX** | Linux ABI hosted 运行、主机自测环境（`make -C user/cmsis_rtos2 test`） | 映射为宿主 pthread，策略由外部操作系统决定，不具备实时确定性 |

### 裁剪 Profile（基于 `cmsis_rtos2.toml`）

根据固件功能需求选择最小系统调用子集：

| Profile | 保留对象族 | Native 系统调用数 |
|---|---|---|
| `mcu` | `thread`, `mutex`, `semaphore`, `timer`, `event_flags`, `message_queue`, suspend/resume, idle, vm_unmap | 19 |
| `mcu-min` | `thread`, `semaphore`, `timer` | 9 |
| `full` | 全量对象族（包含 `memory_pool`、线程挂起等） | 主表全量 |

构建生成与符号门禁：

```sh
# 生成特性头文件与系统调用子表
python3 tools/cmsis_trim.py emit --profile mcu --out build/trim-mcu

# 校验构建出的内核 ELF（多一个或少一个未声明的 sys_a20_* 均会报错）
python3 tools/cmsis_trim.py check --profile mcu --elf build/kernel.elf
```

## 3. 时钟 Tick 频率对齐

CMSIS-RTOS2 的 tick 频率宏 `OS_NATIVE_TICK_HZ` 必须与微内核实际的硬件时钟中断频率 `ARCH_TIMER_FREQ` 完全一致：

| 平台 | 内核时钟中断频率 | 编译宏定义 | 单次 `osDelay(1)` 实际时长 |
|---|---|---|---|
| Hi3861 实板 | 100 Hz | `-DOS_NATIVE_TICK_HZ=100` | 10 ms |
| QEMU RISC-V32 MCU | 1000 Hz | `-DOS_NATIVE_TICK_HZ=1000` | 1 ms |

树内构建由 `tools/mk/targets-images.mk` 自动从板级 `MCU_TICK_HZ` 传入该宏。若在独立外部项目中链接人格库，务必显式声明该宏；若缺省使用默认的 100 Hz，在 1 kHz 的 QEMU 环境中会导致所有超时与延时扩大 10 倍。

## 4. 内存规划与栈尺寸量化

在典型 352 KiB SRAM 的 Hi3861 等硬件环境中，内核、flat program 镜像与堆栈共享物理内存，需严密控制内存消耗：

1. **禁用大尺寸默认栈**：人格库默认栈大小为 `OS_CFG_DEFAULT_STACK_SIZE = 16384`（16 KiB）。在 MCU 上创建数个线程即可耗尽物理内存。在调用 `osThreadNew()` 时，**必须通过 `osThreadAttr_t.stack_size` 显式指定尺寸**：
   - 纯传感器采样、GPIO 翻转任务：建议分配 **1024 字节 (1 KiB)**；
   - 涉及格式化打印、状态机或轻量协议封包任务：建议分配 **2048 字节 (2 KiB)**；
   - 定时器回调较多的系统：通过 `OS_CFG_TIMER_THREAD_STACK` 设置定时器服务线程栈（树内 demo 配置为 4096 字节）。
2. **优先使用静态内存分配**：并发线程较多时，优先传入外部静态数组作为栈内存（`attr.stack_mem`），避免因动态堆分配引起 16 KiB 块级碎片增长。控制块亦支持静态传入（`attr.cb_mem` 与 `attr.cb_size`）。
3. **用栈高水位校准尺寸**：`osThreadGetStackSpace()` 会对 Native 自有栈和调用方提供的静态栈做高水位扫描，可用于验证 1–2 KiB 的估算是否足够。初始线程和无栈元数据的 pthread 默认栈返回 0；目标线程运行中查询是 best-effort，阻塞或结束后查询才稳定。

## 5. 构建与链接接入

### 用户态应用链接参数

将应用源文件与 CMSIS-RTOS2 人格库、liba20rt 共同链接为静态 PIE（`ET_DYN`），仅允许 `R_RISCV_RELATIVE` 重定位：

```make
CFLAGS += -Iuser/cmsis_rtos2/include \
          -Iuser/cmsis_rtos2/src \
          -I$(CMSIS_TRIM_DIR) \
          -DOS_NATIVE_TICK_HZ=$(MCU_TICK_HZ) \
          -march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany \
          -fPIE -fno-stack-protector

LDFLAGS += -Wl,-pie -Wl,--no-dynamic-linker -Wl,-Bsymbolic -T user/tests/liteos-demos/link.ld

SRCS += user/cmsis_rtos2/src/os_kernel.c \
        user/cmsis_rtos2/src/os_thread.c \
        user/cmsis_rtos2/src/os_sync.c \
        user/cmsis_rtos2/src/os_timer.c \
        user/cmsis_rtos2/src/os_queue.c \
        user/cmsis_rtos2/src/backend_native.c \
        user/cmsis_rtos2/src/native_platform_a20.c \
        user/liba20rt/a20_malloc.c
```

### 内核裁剪覆盖

内核构建时将分发表替换为对应 profile 生成的精简表：

```make
CFLAGS += -I$(CMSIS_TRIM_DIR) \
          -DA20_NATIVE_SYSCALL_TABLE='"syscall_table_mcu.def"'
```

## 6. LiteOS API 语义对照表

| LiteOS 原生接口 | CMSIS-RTOS2 对应接口 | 核心行为差异与注意事项 |
|---|---|---|
| `LOS_TaskCreate` | `osThreadNew` | **优先级数值方向相反**：LiteOS 0 为最高、31 为最低；CMSIS-RTOS2 55 为最高、1 为最低。 |
| `LOS_TaskDelay` | `osDelay` | 延时单位均为 tick，需确保编译 tick 宏与内核时钟一致。 |
| `LOS_TaskSuspend / Resume` | `osThreadSuspend / Resume` | Native 后端是内核级跨线程挂起：目标在安全点停止，恢复后重新入 RT 队列。 |
| `LOS_SemCreate / Pend / Post` | `osSemaphoreNew / Acquire / Release` | 语义完全对应；超时参数换算为绝对纳秒后走 futex park。 |
| `LOS_MuxCreate / Pend / Post` | `osMutexNew / Acquire / Release` | CMSIS 默认启用递归与优先级继承（`osMutexRecursive \| osMutexPrioInherit`）。 |
| `LOS_SwtmrCreate / Start` | `osTimerNew / Start` | 回调均在独立的定时器服务线程上下文执行。 |
| `LOS_QueueCreate / Read / Write` | `osMessageQueueNew / Get / Put` | `msg_prio` 数值大者先出，同优先级 FIFO；对象状态仍由人格层维护，不进入内核。 |
| `LOS_EventRead / Write` | `osEventFlagsWait / Set` | 支持 `osFlagsWaitAny`、`osFlagsWaitAll` 与 `osFlagsNoClear`。 |
| `LOS_IntLock / Unlock` | 厂商 `hi_int_lock / restore` | 仅推迟厂商分发器线程的投递，不屏蔽硬件中断；非标准 CMSIS API。 |
| 在 ISR 中直接 Post 信号量 | 统一采用内核 raise + Dispatcher 线程 | NOMMU U 态不支持执行硬件 ISR，由 M 态捕获后通过 futex 唤醒 Dispatcher。 |

## 7. 移植关键边界与陷阱防范

1. **时钟频率不匹配**：若 `OS_NATIVE_TICK_HZ` 与内核硬件定时器频率脱节，所有相对延时与超时将按比例失真。
2. **优先级数值颠倒**：切忌将 LiteOS 的低数值（如 0、1）直接传入 `osThreadAttr_t.priority`，这会导致任务以最低优先级（`osPriorityIdle`）运行。
3. **调度锁内阻塞**：`osKernelLock` 只在调度安全点推迟抢占，并不禁止任务主动挂起。在锁内调用 `osDelay` 或阻塞等待信号量仍会导致 CPU 让出，属于逻辑错误。
4. **捐赠期间修改基准优先级**：当任务正持有互斥量且已被其他等待者提升优先级时，调用 `osThreadSetPriority` 将返回 `osError`，以防止破坏内核保存的基准调度快照。
5. **硬件中断上下文调用**：不要在任何底层硬件中断回调中直接调用 CMSIS API。应使用内核边沿记录与 U 态分发器线程模型。
6. **tickless 与 PMP allow-list**：`osKernelSuspend()` 在单核 MCU 上进入调度器锁 + WFI；flat program 的镜像、栈与动态区构成 PMP 允许列表，访问区间外的地址会以 SIGSEGV 结束当前线程。MCU 内核栈预算当前为 3 KiB，flat program 主栈 8 KiB。

## 8. 验证门禁与验收流水线

应用接入完成后，依次执行以下流水线确保正确性：

```sh
# 1. 宿主对象语义单测
make -C user/cmsis_rtos2 test

# 2. QEMU MCU 模拟器执行 4.1–4.9 实时/时延全流程
tools/a20 test qemu-riscv32-mcu

# 3. 校验内核 ELF 镜像的系统调用符号白名单
python3 tools/cmsis_trim.py check --profile mcu --elf <build/kernel.elf>

# 4. 全局测试实例契约检查
make check-manifests
```

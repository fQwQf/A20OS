# MCU、开发板与服务器的公共应用同步

## 设计与接口

A20 的执行域使用同一任务表、调度器和 Park/Wake 协议。应用同步同样应由
公共 Native ABI 定义，不应出现 STM32 专用的应用锁 API。平台差别只落在
地址映射、用户范围检查与底层原子操作 provider；作业/服务层仍使用 CL/CW。

| 层 | 公共实现/接口 | 平台职责 |
|---|---|---|
| 应用 | `user/liba20rt/a20_sync.h` | 使用同一等待循环，无板型分支 |
| Native ABI | `sys_native_sync.c`：0x0B00 wait、0x0B01 wake、0x0B02 atomic32 | syscall/trap 入口传入参数结构指针 |
| 等待机制 | `ipc/futex.c`、公共 wait_queue、Park/Wake | MMU 映射加锁；flat 动态内存在等待全过程持有分配引用 |
| 原子操作 | GCC 原子调用 | 一致性服务器使用 CPU 原子；非一致性共享 RAM 使用公共 fabric gate |

MCU Native 程序接入上述实现，不再使用 MCU 的空 futex wake stub。
普通未启用 Native 程序的 MCU 构建继续裁剪这两个源文件的实际代码。
ARMv7-M 的 SVC 派发只是 syscall 入口，不复制等待表和同步语义。

## atomic32 契约

0x0B02 接收 `a20_atomic32_args_t *`；32/64 位 ABI 中结构均为 40 B：

| 偏移 | 字段 |
|---|---|
| 0、4 | u32 size=40、version=1 |
| 8 | u64 addr：4 字节对齐的用户字地址 |
| 16、20、24、28 | u32 operation、expected、desired、flags=0 |
| 32、36 | u32 observed、exchanged 输出 |

操作为 LOAD=0、STORE=1、EXCHANGE=2、COMPARE_EXCHANGE=3，均顺序一致。
LOAD 返回当前值；STORE/EXCHANGE 返回替换前值；CAS 返回比较时读到的值，
成功替换时 exchanged=1，否则为 0。其他操作 exchanged=0。接口按本机字节序，
不是跨机器 RPC。SDK 提供通用函数及 load/store 辅助函数。

NULL/用户范围外地址返回 FAULT，未对齐/非法操作/flags/短结构返回
INVALID_ARGUMENT。32 位入口拒绝 u64 地址的高位，不能悄悄截断到本地地址。
MMU 写操作先解析 writable/COW 映射，再在映射锁内复查用户/读写权限；flat
程序只能访问其任务持有的镜像、栈及已登记的动态堆分配。映射锁内进行
范围检查与操作，动态 futex 字的生命周期见 [30](30-flat-user-memory.md)。用户操作数是普通共享 RAM，不能用作
设备 MMIO 操作接口。若原子操作完成后输出结构拷贝失败，会返回 FAULT，但
操作可能已经生效；调用者不能把错误当作无副作用而盲目重试。

原子系统调用允许非特权程序使用内核的公共 provider，不要求应用访问硬件门
或屏蔽 IRQ。在 16 位总线上的普通 32 位访问可能分成两次总线周期；应用须用
本接口访问同步条件字。不能混用普通 C 读写或只对单个 MCU 有效的 LDREX/STREX。
该接口优先保证功能，没有 lock-free、无 syscall 开销或吞吐承诺。

## futex 协议

应用循环执行 `atomic32_load`，条件不满足时 `futex_wait(word, observed, ns)`；
生产者先 `atomic32_store/CAS` 修改条件，再 `futex_wake`。值已改变返回
WOULD_BLOCK，正常唤醒后必须重新读取条件。wake 不保存令牌；不能先 wake，
随后不修改条件就期待未来 wait 自动返回。

wait 在同一个 bucket 锁内原子复查并加入队列，wake 在释放队列锁后通过公共
Park/Wake 唤醒，可在不同 CPU 执行。用户范围预检的 memcpy 不作条件判断，
避免窄总线读取撕裂影响决定。已有 Linux futex 使用同一等待核心。

timeout_ns 为结构中的完整 u64，相对时间向上取整到 tick；UINT64_MAX 表示
无限等待，零仍遵循现有至少一 tick 的行为。有限时间换算饱和以避免溢出。
wake 要求可读的完整用户字、count>=1、flags/reserved=0，实际唤醒数写回参数；
写回失败时也可能已经完成唤醒。无等待者时返回成功且 out_woken=0。

`execution_info.features` 中 FUTEX=8 表示 wait/wake，ATOMIC32=32 表示该
原子接口。服务器的 `abi_info.feature_bits[0]` bit 3 同样公布 atomic32，
bit 2 保持 futex 含义。MCU 通过公共 execution_info 查询，不发布尚不存在的
句柄或线程创建能力。这个阶段完成的是已有任务的同步接口。

## 验证和边界

```sh
make smoke-native-futex
make check-native-sync
make check-mcu-program
make check-mcu-program-rtl
make smoke-native-cluster
make smoke-futex-stress
make check-execution
```

`check-native-sync` 在 RV64、AArch64、x86-64 双核 QEMU 内核中执行同一个
原生应用，并记录镜像/内核哈希与模拟器退出方式。服务器原生用户态覆盖
公共 SDK 分配器的对齐/溢出/扩缩容契约，以及四种原子操作、成功/失败 CAS、
无效地址、只读映射、解除映射、短结构/非法 flags、超时与真实线程唤醒。
MCU 原生 ARM ELF 经实际 SVC 指令覆盖十二种 futex 参数/超时路径、八种原子
路径、64 位大超时、跨 CPU wake、wait 入队竞态、同 PID/栈迁移与退出回收。
退出测试接受 ZOMBIE 或持有引用的已自动回收 UNUSED，避免错过瞬时状态。
RTL 联合测试进一步经过共享总线、硬件 gate 和 RAM；仍不是物理板证据。

这些机制与 STM32 无关，但新增其他 MCU 的异常入口、共享 RAM 映射和板级
启动仍须分别实现和验证。树莓派的公共调度自检已完成，完整用户态/设备链路
仍待接入；不能把接口存在当作所有板型已联调。异构 ISA 可共享作业/服务接口，
原生线程直接迁移仍要求兼容 ISA、ABI、地址布局与真实的共享执行域。
线程入口同样使用公共 Native 参数：调用者给出 16 字节对齐的栈顶，公共线程
创建路径调用架构栈适配；x86-64 预留 CALL 对应的 8 字节偏移，ARM/RISC-V
保持对齐栈顶。入口须调用 thread_exit，不能返回。这修复了本轮 x86 测试暴露
的 C 函数栈对齐问题，Linux clone 和进程启动不走该适配。
RV32 Native 编译器沿用内核的多目标工具链选择，避免要求额外的专用 Linux
前缀；本轮 RV32 Native 同步程序仅编译链接，不是 RV32 用户态开机证据。
完整 JVM/图形输入和 Minecraft Java 客户端尚未完成。


本轮三种服务器双核测试和 MCU/RTL 测试均通过；AArch64/RV64 模拟器自然
退出，x86 在关机输出后由夹具 SIGTERM 结束，不计作自然电源关闭。
既有 Native 集群作业和 Linux futex 压力回归通过。默认 128 KiB/8 KiB MCU
固件 SHA-256 仍为
`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。
执行协议三 ISA 宿主测试及 ASan/UBSan、裁剪、ABI/文档边界检查通过；格式
门禁仍为既有 114 个文件失败，与前一提交证据中的失败集合完全相同，未扩充
baseline。快照与日志保存为 `.kernel-build/cluster/evidence/native-sync-<commit>/`。

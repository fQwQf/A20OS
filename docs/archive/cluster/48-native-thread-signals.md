# 实际线程协作：Native 定向通知与 POSIX 信号等待

本阶段让实际程序能够通知指定 pthread 并等待通知，解决真实 JamVM 初始化暴露的 `Tgkill`、`Sigtimedwait` 缺失。公共内核实现同时用于 MMU 服务器和共享 RAM 的 flat MCU 域，没有 STM32 专用信号分支。调度器和计算热路径没有加入轮询；等待接口暂用最长 2 ms 的休眠轮询，以先保证功能和不丢通知。

## 已实现的接口

公共常量在 `kernel/include/uapi/a20/task_signal.h`，SDK 包装在`user/liba20rt/a20_task.h`。不新增 syscall 编号；旧 SDK 调用传零标志，继续使用原来的进程共享队列和屏蔽字。新版 SDK 和 full mlibc 的检查/mask 操作统一通过 `signal_check(STRUCT, &control)` 传递 40 字节固定布局结构，包含 size/version、operation/flags 和 64 位 selected/mask/result。32 位 CPU 只在寄存器中传指针与状态，信号集合不会随寄存器宽度截断。完整结果也可直接通过`a20_thread_signal_control()` 取得，避免把高位 bitmap 当成负错误码。

| SDK 调用 | 行为 |
|---|---|
| `a20_thread_signal(handle, sig)` | 持 `SIGNAL` 权限的句柄向目标线程排队；0 只检查句柄，9 沿用强制退出 |
| `a20_thread_signal_mask(mask, old, flags)` | 当前线程的屏蔽字；QUERY 只查询；KILL/STOP 不可屏蔽 |
| `a20_thread_signal_check(flags, selected)` | 默认消费当前线程及进程队列中未屏蔽的信号 |
| `...check(PEEK \| SELECT, selected)` | 无副作用查询指定集合，包括已屏蔽的 pending 信号 |
| `...check(SELECT, selected)` | 原子消费指定集合中的一个信号，用于 sigwait；其他线程的定向队列不可见 |

位 N 对应信号 N，位 0 未用。定向发送使用 task_kill 的第 3 参数 THREAD。结构中的 operation=CHECK/MASK，flags 分别允许 THREAD/PEEK/SELECT 或 THREAD/QUERY。未知 flags、短结构、无效版本/operation 返回 INVALID_ARGUMENT。原寄存器形式的 signal_check/signal_mask 仍可使用，但 32 位原始标量传输不提供完整 64 位集合；新版包装统一使用结构形式。旧的进程级屏蔽字与新的线程级屏蔽字分别作用于各自的检查接口，混用时不能假定一个模式的 mask 会修改另一模式。

队列和线程 mask 都由共享句柄表锁保护，一次检查同时处理进程和当前线程队列。同一信号在两个作用域各 pending 一次时先消费线程事件，保留进程事件。跨线程排队持有稳定的句柄表引用，避免与目标退出/表释放并发时访问已释放内存。新任务继承创建者的线程 mask，pending 不继承。字段只编译进支持 Native 任务对象的配置，不增加默认精简 MCU task 的字段。

## libc 接入

- `pthread_create` 保存真实 Native 线程句柄，通过 task_info 获取真实 TID，不再用合成编号。TCB 发布使用 release/acquire；Clone 失败不发布未初始化的 TID。`pthread_kill` 只使用本进程已有句柄，不按任意 PID 查询任务。
- `pthread_sigmask`/`sigprocmask` 使用内核的每线程 mask，正确返回修改前的 mask，校验 how。`sigpending` 重复查询不会消费信号。
- `sigtimedwait`/`sigwaitinfo`/`sigwait` 消费目标集合中的一个信号，支持无限等待、零超时、有限超时和 timespec 校验；sigwait 返回 errno 风格错误码。
- `sigsuspend` 使用传入的临时 mask，处理通知后恢复原 mask；sysdep 返回 EINTR，由通用 libc 层设置 errno。handler 执行期间应用 sa_mask、NODEFER、 RESETHAND；handler 表的读取和修改有锁。
- task_clone 后重置 libc 的线程句柄登记和锁，更新当前 TCB 的真实身份，保留已有 fork/waitpid 接口。退出线程登记为不可投递。

这是显式检查点投递，不能在任意用户指令处异步调用 handler。当前 bitmap 接口不提供发送者 PID、完整 siginfo 或 ucontext；SA_SIGINFO 收到清零的附加信息和空上下文，只填写 signo。完整 POSIX 异步取消、pthread 句柄/TCB/栈回收以及 VM 的正确 GC safepoint 暂停协议仍未完成。

## 32 位边界的实际发现

增加高位参数检查时，ARM MCU 实际执行失败。原 `a20_syscall6` 按架构寄存器传标量，在 ARM/RV32 上只传低 32 位；因此不能把 uint64_t 函数参数当作完整 64 位 wire 值。新增固定布局结构没有改变整个 syscall 调用约定，也没有新增 MCU 特判；服务器、MCU SDK 和 libc 使用同一结构及内核处理函数。共同应用验证高位 mask 继承、信号 40 的排队/peek/消费，并保留截断诊断的失败日志。

## 实际执行结果

| 检查 | 结果 |
|---|---|
| full mlibc 真实 pthread 信号程序，单核/双核 | 同一个 ELF 各 10 轮正常退出 0、返回 shell，QEMU 自然退出 |
| 程序内检查 | mask 继承/隔离、指定线程投递、重复 pending、单次消费、sigwait、超时、handler mask、sigsuspend、join、fork 后身份及 waitpid |
| 无绑定的真实 pthread 计算 | 单核/双核各 10 轮，32 任务、总和 4359168，CPU mask 1/3；未设置 affinity |
| RISC-V / ARM64 / x86 Native 共同契约 | 双核均通过；旧进程信号行为、线程队列/mask/peek（含信号 40、继承 mask 位 41）、实际计算与自动分配均在同一应用中执行 |
| ARM MCU 固件指令模型与共享仲裁 RTL | 同一 ELF、两 CPU、各 14 次任务退出；公共高位线程信号契约和 32 计算任务通过 |
| 默认精简 MCU 固件 | SHA256 与修改前逐字节相同：`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d` |
| 完整 JamVM + GNU Classpath 初始化 | 不再缺 Sigtimedwait/Tgkill，继续加载真实类库，随后报告 UnsatisfiedLinkError 并退出 1 |

最后一项是诊断结果，不是 Java 应用通过。`check-jamvm-native.py` 报告中的 passed 只表示启动器/JNI 探针和诊断正常完成；`java_application_verified`仍为 false。下一步需要构建和装载 GNU Classpath Native JNI 库，验证真实 Java 程序与 GC，再推进图形/输入及 Minecraft 客户端。

运行证据目录为 `.kernel-build/cluster/thread-signals-*`；归档 manifest 保留最终输入、源码快照、构建日志、程序退出码和未完成范围。MCU 指令模型及 RTL 仲裁检查不替代物理多板验收。full mlibc/JamVM 本阶段仍限 RV64；公共 Native 线程接口的多架构验证不等于所有架构已有完整 Java 运行时。

## 复现

准备好 Native full mlibc、对应 CPU 配置的内核/镜像，以及由同一 libc 链接的`user/tests/test_mlibc_signals.c` ELF 后执行：

```sh
python tools/mlibc/check-mlibc-signals.py --cpus 2 --cycles 10 \
  --kernel /absolute/frozen/kernel.elf --image /absolute/frozen/fat32.img \
  --program /absolute/frozen/signals.elf \
  --output .kernel-build/verify-native-signals
python tools/native/check-native-sync.py --cpus 2 --timeout 180 \
  --output .kernel-build/verify-native-signals-servers
```

运行器冻结输入，拒绝覆盖已有目录，校验 PASS、数值退出码 0、shell 返回和 QEMU 自然退出；源码工作区哈希明确不作为外部 ELF 的构建来源。JamVM 的完整构建和类库诊断命令继续使用 [47](47-jamvm-native-bootstrap.md)。

真实 Java 多线程计算、Native JNI 文件操作和单核/双核自动执行验证见[49-native-java-execution.md](49-native-java-execution.md)。Java 应用阶段已通过； GC、物理 MCU Java 运行时与 Minecraft 客户端仍待继续实现。

原生 Java 的并发回收、栈根保存和实际对象移动已在 RV64 Native 单核/双核验证，见 [50-native-java-gc.md](50-native-java-gc.md)。物理多 MCU、其他架构完整 VM 和真实 Minecraft 客户端仍未验收。

完整 Native Java 运行时已继续接入 AArch64，并通过同字节码的单核/双核计算与 GC 压缩，RV64 回归同时通过；实际次数、JNI 修复和剩余范围见[51-native-java-portable.md](51-native-java-portable.md)。物理多 MCU 和 Minecraft Java 客户端仍待实现与验收。

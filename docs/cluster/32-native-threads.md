# 统一 Native 用户线程与应用生命周期

## 接口与机制

一次 Native 应用启动可以创建多个用户线程，共享映像、堆、句柄表和线程组，
每个线程有独立的寄存器状态和用户栈。服务器 MMU 与共享 RAM MCU 使用相同
SDK、参数布局、句柄表、等待/唤醒、调度器和退出回收机制。

| 接口 | 编号 | 当前行为 |
|---|---|---|
| handle_close | 0x0100 | 关闭句柄；不终止线程，也不等同于 join |
| task_wait | 0x0202 | 等待 TASK/THREAD 子对象；NONBLOCK 返回 WOULD_BLOCK |
| thread_create | 0x0205 | 共享地址空间和句柄表，返回 THREAD 句柄并写 out_thread |
| thread_exit | 0x0206 | 只退出当前线程 |
| task_set_sched / task_get_sched | 0x0209 / 0x020A | 复用公共调度配置，包括 CPU 亲和性 |
| task_exit | 0x0200 | 退出整个线程组，沿用既有实现 |

`a20_thread_create_args_t` 在 32/64 位均为 56 B、version=1。历史字段
`stack_base` 实际传入向下增长栈的 **顶部**，要求 16 B 对齐；stack_size
描述顶部以下的可用范围。未更改 SDK 名称、编号或这个既有约定。
入口接收一个 uintptr_t 范围的参数，必须显式 thread_exit，不能直接返回。
x86 的返回地址槽偏移仍由架构适配处理，Linux clone 不经过此调整。

`sys_thread_create.c` 完成统一参数验证、创建未发布任务、安装 THREAD 句柄、
写回用户结果，最后才发布到调度器。分配失败、句柄表耗尽或用户写回失败都
销毁未发布任务；已安装的句柄在写回失败时关闭，不留下已运行的孤儿线程。
既有内核 `proc_create_thread()` 立即发布接口保留，Native syscall 使用新增的
deferred 形式，以便在发布前完成事务。

## 地址模型与架构适配

MMU 后端沿用公共 clone 的 CLONE_VM/FILES/SIGHAND/THREAD/SETTLS 共享规则。
flat 后端的通用线程组、信号状态引用、句柄表引用和发布路径位于
`kernel/proc/native_thread.c`。`core/flat_thread.h` 定义 ISA 适配契约：准备
用户寄存器、入口继续点、共享映像引用与栈固定引用，不发布任务。
这个契约不包含 STM32 外设、总线或板号。

目前实际 flat 用户上下文适配为 ARMv7-M 静态 PIE。映像所有者保存程序与初始
栈、公共 flat space；每个线程上下文持有映像引用，并固定自己的完整栈范围。
动态栈未结束时 vm_unmap 返回 BUSY；join 回收最后任务引用后可以整块释放。
主线程先退出时，其他线程仍能访问映像、全局数据、初始参数和共享堆；最后
线程回收时才释放这些资源。该 profile 没有 TLS 段，非零 tls_base 明确返回
NOT_SUPPORTED；入口须属于本映像可执行段，栈须属于公共已登记地址范围。
NOMMU 仍为可信应用地址检查，没有新增 MPU 隔离。

`execution_info` 新增 THREAD_CREATE=128，已知掩码 255。它表示上述创建/加入
能力，不表示 TLS、动态库、跨 ISA 栈迁移或完整 Native ABI。
MCU 复用生产句柄表，只接纳 TASK/THREAD 类型，接通创建、等待、关闭和调度
操作；不公布代表完整句柄接口的 HANDLES 位。服务器的完整句柄能力保持原样。

## 退出与回收

Native 新建线程在发布前标记为可加入；只要同组还有等待者，它的退出记录
不会像 Linux clone 线程一样被自动回收。task_wait 消费退出记录，之后应关闭
线程句柄。线程组结束、对象失去同组接收者时沿用公共孤儿回收。Linux 创建
的线程没有该标记，保留原有自动回收行为。

关闭句柄不会主动消费退出记录，长期运行程序仍需 join。TASK/THREAD/DEBUG
现已引用独立的任务身份对象：join 回收后，保留的旧句柄不再解析为任何任务，
PID 复用不会让它指向新任务，仍可以正常关闭；dup 和通道传递保留同一身份。
具体生命周期与验证边界见 [33-task-object-lifetime.md](33-task-object-lifetime.md)。
全局实例身份、启动代次和对象目录仍属于
[31-runtime-instance.md](31-runtime-instance.md) 的后续工作。
SDK malloc 仍要求调用者串行化，多线程可使用公共 vm_alloc 或自行加锁。

## 验证与性能边界

```sh
make check-native-thread-admission
python3 tools/check-native-sync.py --output .kernel-build/cluster/native-thread-sync
make PYTHON=/path/to/venv/bin/python check-mcu-program-rtl
```

同一份 `user/tests/native-thread-contract.h` 在 MCU、RV64、AArch64、x86-64
真实 Native 用户路径执行，覆盖坏参数、共享变量、futex、嵌套线程创建、
非阻塞 join、退出码、句柄关闭和栈释放。应用通过公共 sched 接口选择另一个
在线 CPU，实际子线程查询并核对 CPU ID；MCU 固定为 parent=1、child=0。
MCU 还验证栈固定、主线程先退出、子线程继续访问映像/堆/初始参数以及最终
程序上下文全部回收，可用内核堆恢复至应用启动前（保留控制线程）的基线。
固件总计五次真实退出；外部模型不修改任务状态。
RTL 联合验收检查共享数据与实际指令读取，仍不代表物理多板运行。

独立宿主测试编译生产 syscall 处理函数，以依赖适配器确定性注入分配失败、
不支持、句柄耗尽、读入失败和写回失败，检查零发布及资源回滚；在 x86、
AArch64、RV64 指令环境运行。这项测试不代替真实内核和 SVC 证据。

功能代码按显式 Native MCU profile 裁剪；默认精简 MCU 无线程/句柄表新增代码。
普通文件、网络、堆访问不会因本阶段改变而触发远端请求；只在 Native 线程
创建和退出记录回收路径增加行为。默认精简固件哈希应继续与基线相同，
服务器以原有 Linux 进程/futex 和 Native 应用回归验证兼容性；没有全量性能测量。

本阶段尚未提供 JVM、Java 类库、本地库、文件/显示/输入运行时和物理多板
验收，也未启动 Minecraft。它补齐的是单个应用跨共享执行域使用线程的基础。

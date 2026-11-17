# 任务身份与生命周期

## 为统一运行实例补齐的基础

运行实例需要识别“哪个对象”，不能把可复用的 PID 或某个句柄表下标当成对象身份。本阶段先修复同一原生执行域内的 TASK/THREAD/DEBUG：公共实现位于`kernel/proc/task_object.c`，不包含板型、ISA、总线或远端协议条件。 MCU Native 程序和服务器 Native ABI 使用同一实现；MCU 的完整 DEBUG/IPC 接口仍未启用，不因这次内部改动而宣称支持。

| 持有者 | 持有内容 | 结束条件 |
|---|---|---|
| 任务 | 身份对象的一份引用 | 任务最后一次 proc_put |
| TASK/THREAD/DEBUG 句柄 | 同一身份对象的一份引用 | 关闭、替换或句柄表销毁 |
| 在途通道消息 / dup 操作 | 同一身份对象的一份引用 | 完成交付或回滚 |
| 身份对象 | 指向原任务的弱绑定 | proc_destroy_task 解除 |
| 正在执行的任务操作 | 原任务的一份 proc_get 引用 | 本次操作完成 |

首次安装任务句柄时才分配小型对象。多个句柄和调试会话引用同一个对象；对象不持有 task_t 强引用，所以自身句柄、共享句柄表不会形成保留任务的引用环。关闭一个引用不影响其他引用；任务回收也不等待旧句柄全部关闭。

## 关闭与复用的顺序

句柄查找在表锁内取得对象引用，随后在公共身份锁内把弱任务绑定转换成 proc_get 引用，再放下对象引用。并发关闭不能释放正在查找的身份；并发回收只能让操作获得原任务引用，或返回 BAD_HANDLE，不会让它看到替代任务。

proc_destroy_task 先将任务标记 UNUSED 并从调度/任务列表移除，然后解除弱绑定，最后撤销 PID 注册并放下任务拥有的引用。启用 Native 对象的配置将 PID 数字保留到最后一个任务引用释放，保证仍调用 PID 形式公共 core API 的等待/调试操作不能意外找到同号新任务。普通 Linux-only 与默认精简 MCU 保留原路径。句柄只保留身份对象，不占用旧 PID；原任务释放后即可复用 PID。

例如，线程 A 的 PID=7，用户持有线程句柄 h 和它的 dup。join 回收 A 后，新线程 B 可以复用 PID=7。对 h 或 dup 的 task_wait、调度、调试等目标操作返回 BAD_HANDLE；关闭它们仍成功。handle_query 仍能查询句柄自己的类型和权限，handle_poll 对已回收任务仍报告 EXITED。这不新增第二次 join 语义。

退出和信号事件以身份对象作为 watch key，TASK/THREAD/DEBUG 的别名都使用相同身份。任务退出时移除这些 watch；对象最后释放时再清理退出之后注册的 watch，避免对象地址复用导致事件串到后继对象。事件队列没有新增完成快照或晚注册 EXITED 补发语义；需要当前退出状态时可使用 handle_poll。

## 接口、性能和跨架构边界

用户句柄大小、syscall 编号、参数结构和 SDK 使用方式不变。任务身份指针不暴露给用户，也不序列化到 CL/CW 网络协议。任务创建后仅在首次 Native 句柄安装时分配身份对象；普通调度、内存访问、文件与网络数据路径没有新增远程调用或周期状态同步。Native 句柄操作与回收有明确的引用/锁开销，不宣称已做全平台性能评测；默认精简 MCU 应继续与既有固件二进制相同。

本对象只在当前内核启动期间有效。跨执行域对象目录仍需要实例身份、域启动代次、对象序号、权能与撤销规则，不能用这个内核指针充当网络对象 ID。异构 ISA 的服务和作业可以共享同一实例管理协议；原生线程上下文只在相容 ISA/ABI/地址模型的执行域内共享，生命周期对象不会改变这个限制。

## 验证

```sh
make check-native-task-object check-native-thread-admission
CCACHE= QEMU_ACCEL=tcg,thread=multi python3 tools/native/check-native-sync.py \
  --output .kernel-build/cluster/native-task-identity
make PYTHON=/path/to/venv/bin/python check-mcu-program-rtl
```

`check-native-task-object` 编译生产身份实现、生产 PID 分配器和通用对象引用/释放实现；宿主适配器提供任务存储、句柄表、事件和硬件锁。测试把 PID 上限设为 1，强制复用同号任务，检查旧任务正在操作时不复用数字、旧句柄与调试别名不命中新任务、任务回收后的在途引用交付、分配/安装失败、自身引用不形成任务环、四线程安装/查找/关闭与回收竞争及分配数量归零。它运行在 x86、 AArch64、RV64 指令环境；x86 另用 ASan/UBSan 检查。这不是完整 IPC 内核启动或物理板证据。

共同 Native 应用在三个服务器 ISA 的双核内核和双 MCU RTL 联合环境执行，在 join 后保留旧句柄并验证等待/调度返回 BAD_HANDLE、关闭成功；服务器还验证实际 handle_dup 的别名。MCU 同时保留真实跨 CPU 执行、栈固定、嵌套线程、主线程先退出以及最后可用堆完全恢复的验收。

Minecraft Java 客户端、JVM/类库、本地库、文件/显示/输入运行时、其他 MCU ISA 的用户上下文适配和物理多板验收仍未完成；本阶段不能作为这些功能的完成证据。

扩大回归发现两个既有失败：`smoke-native-handle` 在文件传输阶段输出 F:xfer，`smoke-native-debug` 在 attach 成功后出现调度锁等待。两者均用前一提交`cd2bb0a0` 的归档 RV64 单核内核和隔离磁盘副本实际复现，不能列为本阶段通过项，也不能据此宣称完整 Native 文件传输/调试可用。原有 Native IPC、信号、Linux 进程/futex 压力与 Native 集群回归通过；本阶段未改动这两个失败涉及的文件传输和调度核心算法。

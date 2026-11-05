# 公共运行实例命名空间与对象名

## 目标与边界

一个 A20 运行实例可以含多个原生执行域。每个域可以是一台服务器的 SMP 内核，也可以是多个相容 MCU 经共享 RAM 与硬件仲裁共同执行的一个原生内核。公共接口不含 STM32、树莓派、总线或特定 ISA 的字段；实例内的域仍通过[执行能力](28-execution-capabilities.md)与[执行域状态](31-runtime-instance.md)描述其 ISA/ABI、内存、原子操作和在线 CPU。

当前实现先解决身份基础：同一内核中的进程、线程、服务查询同一实例绑定，控制进程退出和重启不会更换绑定，任务退出/PID 复用不会改变旧对象名。`BOUND` 仅表示已配置身份。它不表示成员均在线、资源租约有效、全局目录一致、 JVM 就绪或 Minecraft 已启动。跨 ISA 共同提供服务与直接迁移原生栈的约束仍不同。

## 绑定与启动代次

Native `runtime_info` (0x0004) 返回 64 B；`runtime_bind` (0x0005) 输入 64 B。 SDK 为 `a20_runtime_info`、`a20_runtime_bind`，均适用于服务器和 Native MCU。 capacity 不足返回 INVALID_ARGUMENT，空指针/不可访问用户内存返回 FAULT；绑定结构 size 必须恰为 64、version=1，flags 与保留字必须为零。绑定调用要求当前任务存在且有 CAP_SYS_ADMIN，普通任务收到 PERM。

| 偏移 | 绑定输入 | 查询输出 |
|---|---|---|
| 0、4 | u32 size=64、version=1 | 相同 |
| 8、12 | u32 flags=0、reserved=0 | u32 state、flags=0 |
| 16、32 | instance_id[16]、domain_id[16] | 相同 |
| 48、56 | u64 boot_epoch、reserved64=0 | 相同 |

启动时 `UNBOUND=0`，查询的两个 ID 和 epoch 均为零。不凭地址、PID 或随机数伪造唯一实例。授权引导/部署层提供两个非全零、非全 ff 的 128 位 ID 与非零 64 位 epoch 后，状态成为 `BOUND=1`。完全相同的绑定重试成功；任何有效但不同的绑定返回 BUSY，原绑定保持不变。本次内核启动没有解绑或改绑接口。同一共享 RAM 内核的所有 MCU 共用一次绑定，不按物理 MCU 分别绑定。

部署层须保证 instance_id 表示期望的运行实例，domain_id 在该实例内唯一，同一 domain_id 的后续内核启动使用不同 epoch。不能把控制进程重启当作内核重启，也不能让两个独立内核同时复用 domain_id/epoch。可由可靠持久计数器或外部授权分配器发放；自动持久化与多域分配器尚未实现。内核拒绝改绑，但没有历史启动记录，无法识别重启后重复使用的静态配置。当前 CL boot nonce 也没有持久唯一性保证，因此不把它当作这里的 epoch。

绑定由公共 core/runtime.c 发布一次。发布与竞争绑定使用同一锁，查询经 acquire 读取不可变快照；不会按字段观察到半个绑定。查询不需要周期工作线程或远程请求。

## TASK/THREAD/DEBUG 的统一对象名

Native `handle_identity` (0x010D)，SDK `a20_hdl_identity`，返回固定 80 B：`u32 size=80, version=1, state, reserved=0` 后跟 64 B 对象名。要求已有句柄且具有 STAT 权限；缺权限返回 ACCESS，无效句柄返回 BAD_HANDLE，其他对象种类返回 NOT_SUPPORTED。未绑定时返回 WOULD_BLOCK，不生成伪全局 ID。句柄、类型和绑定检查在输出前完成；用户拷贝失败沿用普通 syscall 语义，可能部分写入用户缓冲。DEBUG 句柄也须显式具有 STAT；接口不扩大既有调试权限。

| 对象名偏移 | 字段 |
|---|---|
| 0、16 | instance_id[16]、domain_id[16] |
| 32、40 | u64 boot_epoch、serial |
| 48 | u32 kind=TASK(1)，TASK/THREAD/DEBUG 别名使用同一 kind |
| 52–63 | 三个 u32 保留字，必须为零 |

serial 在首次创建任务身份对象时从本内核的 64 位计数器分配。分配在既有身份发布锁内完成；不取 PID、对象地址或时间戳。序号耗尽返回 NO_SPACE，不回绕。同一对象的 dup、在途引用和不同 TASK/THREAD/DEBUG 别名共享同一对象名。任务回收后保留的句柄仍可查询原名，并返回 `RETIRED=2`；`LIVE=1` 表示任务尚未进入销毁阶段，包括待 join 的退出记录，不能解释为正在运行或健康。回收不保留任务地址空间，仍遵循[身份生命周期](33-task-object-lifetime.md)。

对象名是名字，不是访问权。当前没有按对象名打开句柄或绕过能力的接口；全局目录、远程引用租约、撤销和跨域代理尚待实现。命名空间相同也不会自动把独立内核合并成共享内核，或使另一 ISA 执行本域的原生返回地址。

公共 UAPI 提供 `a20_object_id_encode/decode`，线上固定 64 B：UUID 是原始字节序列，epoch/serial/kind 为小端整数，保留字为零。解码必须精确 64 B，验证成功才发布输出；不要把 C 结构体 memcpy 当作跨端序协议。绑定/查询结构是本机 syscall 数据，不能直接作为网络帧。

## 可选部署配置

既有不含这些字段的 cluster.conf 继续可用，不触发新绑定。需要运行实例身份时同时提供 `instance` 与 `epoch`，domain_id 使用已有的 `node`：

```text
version 1
node 00000000000000000000000000000001
role worker
instance a2200000000000000000000000000001
epoch 72623859790382856
```

以上仅是测试格式示例，不能作为每次开机重复使用的生产配置。instance 为 32 个十六进制字符；epoch 为非零十进制 uint64，不接受符号、溢出或重复字段。二者只出现一个则整个配置无效，解析失败不修改输出。supervisor 在启动受管服务前调用公共绑定接口；身份冲突以退出码 6 拒绝，现有控制实例继续运行。原有本地 CS/1 状态协议不变，实例身份通过内核公共查询接口读取。

## 普通使用与验证

已有 Native 编号、结构、execution_info 特征位和非集群应用接口不变。未调用新接口时没有远程访问、周期实例管理线程或调度路径中的实例查询。 Native 任务身份对象增加一个 64 位序号和首次发布时的一次分配；这有小量内存与创建成本，不把它宣称为零成本。默认非 Native 程序精简 MCU 排除这些代码，须重建验证二进制一致；其他配置以功能回归为证据，尚未完成性能基准。

```sh
make check-native-runtime check-native-task-object check-cluster-unit
python3 tools/native/check-native-sync.py --jobs 12 --output .kernel-build/cluster/native-runtime
make check-mcu-program-rtl
make smoke-native-cluster-supervisor
```

宿主测试编译实际 runtime core/syscall 处理函数，任务、用户拷贝、能力和表为明确标记的适配器；在 x86/AArch64/RV64 上覆盖竞争绑定、幂等/冲突、权限与用户拷贝失败、别名/回收状态、小端编解码。x86 另跑 ASan/UBSan。生产 PID 分配器测试强制 PID 复用，验证新旧对象序号不同。真实内核应用在三种服务器 ISA 的双核、ARMv7-M 共享内核/SVC/RTL 中检查同一契约、跨 CPU 的绑定、嵌套线程与 join 后对象名；实际 supervisor 测试检查冲突拒绝和重启后的绑定稳定。 MCU 新测试暴露并修复了 ARMv7-M PSP 线程迁移后 MSP 仍指向旧共享任务栈的问题：返回用户态和恢复 PSP 上下文时重置为独立的 2 KiB CPU 私有异常栈（不复用仍含 idle 上下文的启动栈），IRQ 不再覆盖另一 CPU 的活动栈。测试逐次检查 PSP 异常入口的 MSP 位于私有栈区，不改宿主任务状态。对应报告、日志与 ELF 必须随提交归档，未通过的项目不能记作已实现证据。

本阶段验证结果：三种服务器 ISA 双核均为 CPU1→CPU0；MCU RTL seed=20 与无 RTL 指令模型 seed=21 均通过五次退出和最终堆回收。RTL 两个 CPU 的私有异常栈检查分别为 132、358 次，第二种交错为 130、373 次。supervisor 冲突/重启、 Native IPC/信号/集群及 Linux 进程/futex 回归通过；RV32 公共应用仅编译链接，其他 SDK 头的既有指针转换警告仍在，不宣称 RV32 MCU 用户态已可启动。

默认精简 MCU 固件 SHA256 保持`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。 RV64 Linux-only 原始 kernel.bin 与前一提交也完全一致，SHA256 为`849673bd3c51de40a102491cd5057d6db449bf7bf4c58f58073cec37864d928d`。 ELF 符号文件可以因新增空编译单元而不同，不能据此宣称机器码改变。格式门禁仍为完全相同的 114 个既有失败，未修改基线。此前 Native 文件 transfer 和 debug smoke 的既有失败仍见 [33](33-task-object-lifetime.md)，本阶段未修复它们。证据归档于 `.kernel-build/cluster/evidence/native-runtime-<commit>/`，记录提交、生产源码、报告、实际 ELF 和日志的 SHA256，并保留 MCU 修复前故障 ELF/日志。

这一步尚未完成全局运行状态、对象目录和应用统一启动。后续还需持久启动代次提供者、全局应用归属、文件/显示/输入代理和 Java 运行时； MCU 其他 ISA 的实际用户上下文与物理多板验证也未完成。Minecraft Java 客户端验收继续依照 [24](24-single-system-minecraft.md)，不能以本次身份测试替代。

成员实例准入和 CD/2、CM/2 协议已继续接入，当前边界与复现命令见[35-instance-membership.md](35-instance-membership.md)。

对象目录已继续接入：按所属域解析全局对象名、租约发布和实际能力生命周期，当前接口与验证边界见 [36-object-directory.md](36-object-directory.md)。

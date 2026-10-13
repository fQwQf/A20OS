# 无 CPU 绑定程序的实际执行与均衡

本阶段用真正的 pthread 程序检查应用是否能执行、自动利用空闲 CPU，并正常
退出。程序使用现有 full mlibc 和 Native ABI，没有设置线程 affinity，没有
要求调用者选择 MCU、架构或 CPU，没有吞吐、帧率或等量分配指标。

## 实际失败与修复

### 条件变量广播终止进程

mlibc 的 `FutexWake` sysdep 按 errno 约定返回 0 表示成功。原 A20 适配层
却返回 Native futex 的唤醒数量；实际唤醒一个等待者时，libc 因非零结果
触发 `sys_futex_wake() failed` panic。现在保留 Native 数量输出，成功统一
返回 0，失败继续映射 errno。没有改变 Native syscall 或 SDK 的数量语义。

原始实际程序和失败日志保留。生产函数定向回归还覆盖单个/全部唤醒、0/1/7/
UINT32_MAX 个等待者及错误映射；恢复原返回值的负例失败。

### 两线程都停留在同一 CPU

持续计算对照程序实际允许使用 CPU mask 3，却在旧内核只观察到 mask 1。
队列的 `nr_running` 只统计 READY 任务，未包括正在执行的任务。旧空闲接取
条件要求远端至少排队两项，遗漏了“正在运行一项、另有一项排队”的真实剩余
负载。修复后这个状态允许空闲 CPU 接取；远端只有一项工作且自身空闲时仍
保留它，原有 affinity、RT、节流、在线 CPU 和队列锁约束保留。

远端 current 只作均衡提示。代码将它与永久 idle 存储比较，不解引用可能
已经切出并被回收的远端任务，也不增加运行线程的周期扫描或全局锁。
逐字节相同的对照用户 ELF 在旧、新内核分别输出 mask 1 和 mask 3；首个
修复运行分配为 15/17。生产函数回归还核验队列计数、权重、dispatch 所有权
转移、全部拒绝条件，并以恢复旧条件的版本作负例。

### 工作完成后 shell 不恢复

更严格的正式程序加入主线程条件变量屏障后，旧退出路径在双核第 4 轮、另一
诊断第 21 轮及单核首轮停住：工作结果与 PASS 已输出，但没有返回 shell。
只读现场显示两 CPU 都进入 idle，shell 已停泊，子进程是没有剩余线程的
zombie。首次调试类型漏了 CONFIG_XLATOR，后续快照按实际布局纠正；错误偏移
的快照不作验收证据。

POSIX shell 用 SIGCHLD 配合 WNOHANG 回收进程。主线程先退出而其他线程尚未
结束时，过早的 SIGCHLD 不能得到可回收的子进程；原最后成员退出路径只唤醒
阻塞式 wait，没有再唤醒等待信号的 shell。现在在线程组整体可等待时领取
一次终结通知，在 tasklist_lock 下记录已领取状态并引用父任务，解锁后完成
唤醒和信号，再释放引用。信号直接发给引用的任务，避免再次按 PID 查找。
退出信号为 0 时仍可唤醒 wait；忽略 SIGCHLD 的自动回收规则保留。
精简 MCU 非应用配置继续使用原路径。

同一正式用户 ELF 在补齐通知后的双核和单核分别连续通过 30 轮，逐轮检查
计算、两线程参与及返回 shell。生产函数回归覆盖主线程先结束、最后成员
结束、一次性通知、父引用、零/自定义退出信号及自动回收父进程。

## 可重复程序与检查

`user/tests/test_mlibc_threads.c` 使用真实 pthread、mutex、condition 和 TLS，
动态领取 32 项工作。每项实际计算 128 轮 4096 字节 FNV，并核验固定结果；
另检查逐项计算、无重复领取、完整位图、总和 4359168、两个工作线程参与和
实际 CPU ID。允许多 CPU 时必须覆盖至少两个 CPU。主机只启动、读取日志和
核验输出，不填写工作结果。每次程序以零状态退出并返回 shell 后才计作一轮通过。

```sh
python3 tools/tests/test_sched_idle_surplus.py
python3 tools/tests/test_mlibc_futex_wake.py
python3 tools/tests/test_group_exit_notification.py
python3 tools/check-mlibc-threads.py --cpus 2 --cycles 30 \
  --output .kernel-build/cluster/verify-unbound-dual
python3 tools/check-mlibc-threads.py --cpus 1 --cycles 30 \
  --output .kernel-build/cluster/verify-unbound-single
```

需要项目现有 RV64 工具链、QEMU、mtools、Meson/Ninja；首次完整 libc 构建
按 mlibc wrap 取得依赖。`--mlibc-build/--sysroot` 可指定独立构建缓存。

驱动冻结 kernel、实际 FAT、用户 ELF 和 SHA256，QEMU 使用 `-snapshot`，
拒绝覆盖已有证据目录。可用 `--kernel/--image/--program` 指定冻结输入作
同 ELF 对照；报告明确区分提供的映像与当前工作区源码哈希，不把后者当成
外部映像的构建来源。超时和程序失败保留为失败，不只凭 PASS 字符串放行。

full mlibc 现有完整构建端口为 RV64，新增 `make ARCH=riscv64 mlibc-threads`
是可选目标；普通 rootfs 构建不引入它。公共调度、线程组及 Native API 继续
跨架构；RV64 的 libc 测试通过不代表其他 ISA 已具备完整 pthread/libc 端口。

最终验证与冻结证据见下表；归档入口为
`.kernel-build/cluster/evidence/unbound-programs-<commit>/manifest.json`。
失败、诊断、中间版本与最终验收分别列明。

## 当前边界

这个工作负载证明程序实际执行、自动利用空闲 CPU 和正常退出，没有证明
最优均衡或整机性能。原 Native 多架构与 MCU 回归还包含显式 affinity 迁移
检查；不能把其 CPU mask 3 单独当作未绑定线程初始分配的证据。

JVM、Java 字节码、物理 MCU 和 Minecraft Java 客户端仍未执行。本阶段清除
真实 libc 多线程运行的阻塞点，下一步仍需接入完整解释器、类库、JNI 与客户端
所需显示/输入依赖，最终物理验收保持 [24](24-single-system-minecraft.md)。
mlibc 通用线程层的 TCB 释放、Native 线程句柄回收和 GC 所需的线程定向暂停
仍需继续实现；本测试不将它们判为已完成。

## 最终验证

| 环境 | 结果 |
|---|---|
| RV64 QEMU 双核 full mlibc 程序 | 同一次启动 30 轮；每轮 32 项、总和 4359168、CPU mask 3、两线程参与、退出码 0 并返回 shell |
| RV64 QEMU 单核，相同用户 ELF | 同一次启动 30 轮；CPU mask 1，其余工作和退出检查全部通过 |
| RV64 / AArch64 / x86 QEMU 双核 | 原有完整 Native 应用回归通过，包含实际线程、文件、子程序、纯计算停止和动态工作 |
| x86 QEMU 单核 | 原有完整 Native 应用回归通过 |
| 双 ARMv7-M 指令模型，seed 36 | 14,712,806 rounds；14 次退出、32 项工作、双 CPU 计算和最终堆恢复 |
| 相同 MCU ELF + 共享 RAM/仲裁 RTL，seed 37 | 15,180,742 rounds；14 次退出、32 项工作、双 CPU 计算和最终堆恢复 |
| 默认精简 MCU，QEMU=1 / FLASH=128K / RAM=8K | kernel.bin 与父提交逐字节相同，SHA256 为 c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d |
| ARM32 / RV32 | 编译链接通过；没有运行板卡验收，保留原有指针宽度警告 |
| 原 Linux 进程压力回归、ABI / 生命周期 / 退出 / 文档边界 | 通过；格式检查保留父提交已有 114 个失败路径，无新增豁免 |

正式 pthread 用户 ELF SHA256 为
`76625078614dae63343ad9ac4f3a1fde9ec197f6b055b20a5e944a7c0d0641f1`；
最终 MCU 内核 ELF 为
`a113e8903d6ee7d190c56c57aaaa9f3f2545e7b41f951b832293a6f2a81109f6`。

另外，对原正式 pthread ELF 做了一次只读执行轨迹检查，过滤范围
0x4002e8..0x400303 包含 FNV 的输入读取、异或和乘法循环。两个 CPU 都出现
该计算块的执行轨迹，程序继续以 0 退出并返回 shell。符号参照 ELF 的全部
可分配节和代码与正式 ELF 相同，差异只在 ELF 节表元数据；实际运行的仍为
原正式 ELF。原始轨迹、反汇编、字节一致性检查和驱动均保留。

QEMU 10.0 的 [执行日志实现](https://raw.githubusercontent.com/qemu/qemu/v10.0.0/accel/tcg/cpu-exec.c)
将 Trace 后的编号取自 cpu_index。这里据此核对计算块进入了两个 CPU；
TB 日志次数不能换算为退休指令数、各 CPU 的工作比例或性能指标。

真实 JamVM Native 启动、完整类库构建和文件映射修复见
[47-jamvm-native-bootstrap.md](47-jamvm-native-bootstrap.md)。

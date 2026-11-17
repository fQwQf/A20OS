# 实际程序的执行可靠性

本阶段继续以真实程序的执行、动态工作分配和资源回收验收统一执行机制。修改位于公共调度器和 Native ABI；MCU 与服务器继续使用相同 SDK 和工作算法。没有新增 STM32 专属应用接口，没有要求应用选择 CPU，也没有增加性能目标。

## 从运行失败定位的修复

### 调度时间记账

`proc_yield` 原先在关本地中断之前采样时钟并调用 `eevdf_charge`。时钟中断可以在采样后更新同一任务的 `last_account`；外层随后用较旧的时间相减，无符号时间差下溢，破坏虚拟运行时间与公平调度。旧 AArch64 实际停住现场的工作线程虚拟运行时间达到 `3265598530399343927`，现场寄存器和任务列表保留。

启用内核抢占时，采样和记账现在位于原有 `park_lock` 关中断临界区内，在发布 RUNNING → READY 之前完成。没有增加全局锁；原协作配置保持原路径。`tools/tests/test_sched_yield_accounting.py` 提取实际生产函数，精确注入采样后的中断，同时检查两 CPU 和调用者原中断状态。恢复旧位置的负例得到异常运行时间 `18014398509482134`；修复版在同一时间线得到正确的 150。该定向测试证明中断窗口，不把现场所有超时都归因于这一处。

### 并发文件测试的句柄所有权

句柄是可复用的表槽位。线程关闭文件后，另一线程可以立即得到同一编号；旧测试此时继续读关闭的编号，错误地要求 BAD_HANDLE，实际可能读到新文件，还会改变新所有者的文件偏移。两个真实用户线程已经定向复现此问题：旧文件检查返回 12，新文件偏移被推进到 1。

公共文件测试现在明确区分串行和并发调用。串行阶段保留关闭后读失败检查；并发阶段用永久无效的 `A20_HANDLE_NULL` 检查 BAD_HANDLE。全部 2048 字节内容、 FNV 校验、seek、EOF、权限及打开关闭检查保留。相同槽位复用的修复版真实程序完整通过，另一所有者的文件偏移保持为 0。没有改变句柄 ABI 或复用规则。

两个工作线程的屏障也传播实际错误；一方文件检查失败时，另一方不再永久等待成功状态。错误不计作工作完成，最终的 32 项领取、无重复、总和和 CPU 覆盖检查仍然必须全部通过。`tools/tests/test_native_balance_failure.py` 提取这些实际应用函数，以 hosted 系统调用适配器注入文件错误并检查同伴退出；恢复不发布错误的处理会停在屏障。该定向回归不冒充内核启动或文件后端验收。

### 就绪与等待

x86 单核的第二个真实文件子程序曾返回 75；诊断副本进一步把线程阶段错误传回父进程，得到 112，即线程检查 12：固定 100000 次 yield 已耗尽，但工作线程仍未完成就绪。修复改用公共 atomic32/futex 等待和唤醒，不以轮询次数推断程序能否启动。没有放宽就绪值或退出码要求。

另一个独立问题是 Native `task_wait` 把 `proc_wait4` 的中断错误也映射成 INVALID_ARGUMENT。现在 EINTR/ERESTARTSYS 返回 INTERRUPTED，释放临时引用，保留子任务供重试；其他错误和非阻塞语义保留。真实线程在父线程等待期间持续发送信号 12，逐字节相同的用户 ELF 在旧、新内核分别观察到错误映射和正确中断；消费待处理信号后，两者都能继续等待子线程实际退出 37。文件子程序测试只消费该阶段预期的 SIGCHLD，然后重试同一个子任务。处理函数回归还检查中断时不写输出、引用释放及五种等待结果。

## 复现入口与证据

```sh
python3 tools/tests/test_sched_yield_accounting.py
python3 tools/native/check-native-thread-admission.py
python3 tools/native/check-native-sync.py --cpus 2 --timeout 180 \
  --output .kernel-build/cluster/native-reliability-ready-servers
python3 tools/native/check-native-sync.py --arches x86_64 --cpus 1 --timeout 180 \
  --output .kernel-build/cluster/native-reliability-ready-single
```

最终映像、实际 FAT、用户 ELF、定向复现源码与驱动、串口日志、GDB 只读现场、失败及中间版本、源码哈希保留在`.kernel-build/cluster/evidence/native-reliability-<commit>/manifest.json`。归档区分最终验收、预期负例及未通过的诊断。一次后续单核失败的原始 FAT 已被下一次构建覆盖，仅保留日志和冻结的诊断副本；详细保留范围见归档 README。主机不填写工作结果、任务状态或屏障数据。默认精简 MCU 按原配置重建，其 kernel.bin SHA256 仍为`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。本阶段没有测量整机性能，不能把二进制相同比较扩展成所有配置的性能结论。

先发送 Native 信号再进入等待的初始诊断曾超时，保留为未通过案例；最终中断对照验证的是等待期间收到信号。未新增通用的等待前信号消费语义。早先 AArch64 init 引用下溢和其他子程序阶段超时，尚未逐项确认根因；后续成功不代表它们都已解决。历史网络集群六应用 2/2/2 分配证据仍见[43](43-native-task-control.md)，本阶段没有重新执行该网络场景。

物理多 MCU、JVM 和 Minecraft Java 客户端仍未执行；最终验收保持[24](24-single-system-minecraft.md)。任务组整体停止及 32 位高信号 bitmap 传递仍待完善。指令模型、共享 RAM/仲裁 RTL 和服务器 QEMU 不替代物理验收。

## 最终验证结果

| 环境 | 结果 |
|---|---|
| RV64 / AArch64 / x86 双核 A20OS | 同一公共用户源码完整通过；32 项动态工作、结果和 4359168、CPU mask 3 |
| AArch64 双核连续运行 | 同一次启动 20 轮完整程序通过，逐轮核验实际子程序、纯计算停止和动态工作 |
| x86 单核 | KVM 同一次启动 20 轮完整程序通过；相同最终映像的 TCG 完整程序也通过 |
| 双 ARMv7-M 指令模型，seed 30 | 14,775,659 rounds；14 次退出、两 CPU 计算、最终堆恢复 |
| 双 ARMv7-M 指令 + 共享 RAM RTL，seed 31 | 14,982,569 rounds；相同 ELF、14 次退出、两 CPU 计算、最终堆恢复 |
| 原无文件共享 MCU | 5 次退出及完整检查通过 |
| 默认精简 MCU | 相同配置 kernel.bin 与父提交逐字节相同 |
| ARM32 / RV32 | 相同最终父程序链接通过，已有 SDK 指针转换警告保留；未计作启动 |

最终共享 MCU ELF SHA256 为 `7bc37cce899fe81924bf0a1ba89f8b57ef858a22128d1a72142046337fa5e275`。 ABI、IDL、裁剪、共享 fabric 可移植性检查通过；文档 1498 个行号引用有效。格式门禁仍有与父提交逐项相同的 114 个失败路径，没有增加豁免。

无 CPU 绑定的真实 pthread 执行、空闲 CPU 接取和线程组终结通知见[46-unbound-program-execution.md](46-unbound-program-execution.md)。

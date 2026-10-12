# 纯计算 Native 程序的调度与停止

## 实际问题与实现

共享 MCU 的内核切换原先依赖任务主动进入调度点。两个用户线程一旦分别
进入没有 syscall 的无限计算循环，两个 CPU 都会被占住，已经到期的父线程
也不能恢复执行。这会直接妨碍运行多线程应用和控制实际负载。

可选 Native 程序端口现在保存 SysTick 打断时的完整用户寄存器、原 SP、PC、
xPSR/IT 状态和栈对齐信息。每颗 CPU 累计十次用户态 tick 后，把异常返回
重定向到该任务自己的特权 Thread-mode 入口；入口调用公共 `proc_yield`，
恢复时复用原有 SVC 返回路径。持久退出请求也在这个入口消费。
异常处理器只保存和重定向，不在 Handler mode 切换任务或销毁资源。

这项用户态时间片独立于 `CONFIG_KERNEL_PREEMPT`。MCU 构建有意不定义该
内核抢占宏，内核代码仍按原有安全边界协作切换，不能用该宏挡住用户态时间片。
已经在 syscall 中的程序不会被这个入口异步打断；阻塞 syscall 完成清理后
的退出检查继续由 [43](43-native-task-control.md) 提供。没有新增板卡专属
应用接口。未启用 Native 程序时，原精简 MCU 的 SysTick 指令路径保持不变。
十次用户 tick 是端口的调度策略，不是实时延迟保证或性能验收。

## 相同用户程序的验收

`user/tests/native-compute-cancel-contract.h` 在 MCU 和服务器使用相同源码：

1. 分配两份真实用户栈，创建两个线程；两线程的 affinity 都允许整个在线域。
2. 两线程经公共 atomic32/futex 同步后，分别进入不同机器码范围的纯 C 无限
   循环。循环没有函数调用、syscall 或主动 yield。
3. 父线程休眠 200 ms，恢复后确认两线程仍在运行，再向两者发送信号 9。
4. 等待实际退出码 137，核验对象身份和 RETIRED 状态，关闭句柄、释放用户栈。

MCU 检查器统计实际执行的用户 PC，要求两个循环各至少执行 1000 条指令，
且总覆盖至少两颗 CPU。完整文件程序场景现在要求精确 14 次任务退出，保留
原有文件读取、子程序、动态工作领取和最终堆完全恢复检查。主机不填写任务
结果、ready 字、退出状态或调度数据。

计算工作按动态索引领取的结果和负载证据仍见
[40](40-native-application-balance.md)。这里验证持续占用 CPU 的任务不能
阻止其他程序恢复执行；没有把执行指令数相等作为负载均衡要求。

## 复现与失败证据

```sh
make CCACHE= BUILD_DIR=.kernel-build/cluster/native-compute-final-mcu \
  ARCH=armv7m BOARD=stm32f103 ABI=native STM32_FLASH_KB=512 STM32_RAM_KB=64 \
  STM32_QEMU=0 STM32_XUANWU=0 STM32_CLUSTER_PORT=0 NR_CPUS=2 \
  ALLOW_UNVERIFIED_SMP=1 MCU_SHARED_KERNEL=1 MCU_SHARED_RAM_KB=1024 \
  MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=1 NATIVE_FLAT_FILES=1 -j6 kernel-only
python3 tools/check-mcu-program.py .kernel-build/cluster/native-compute-final-mcu/kernel.elf \
  --backend .kernel-build/cluster/shared-rtl-verilator/Va20_shared_top \
  --seed 26 --rounds 22000000 \
  --report .kernel-build/cluster/native-compute-final-rtl.json
python3 tools/check-native-sync.py --cpus 2 --timeout 180 \
  --output .kernel-build/cluster/native-compute-server-final
```

旧生产中断路径与新测试程序组合的负例在 3M rounds 超时。检查器确认两个
实际循环在两 CPU 上持续执行，累计分别超过一百万条指令；共享时钟已经
超过父线程的休眠期限，父线程仍没有机会调用 task_kill，没有任务退出。
最终负例保留旧生产目标文件，只重新编译嵌入用户 ELF 的汇编并正常链接，
使用逐字节相同的最终父程序。全部链接输入和哈希保留。该负例没有可选 kallsyms，
内核自测元数据也与最终版本不同，不能声称整个内核源码相同。负例只证明旧缺陷，不能计作验收通过。
第一次修复仍错误地使用内核抢占宏，因此再次超时；这份映像和日志也保留。
单核测试还暴露了就绪同步依赖 100000 次 yield 轮询的问题；最终使用实际
futex 等待与唤醒，线程就绪不再受轮询次数限制。初版单核文件子程序等待
检查也曾失败，诊断副本与最终版本通过，尚未确认首次失败根因。最终 AArch64
复核中，纯计算停止已通过，首个文件子程序阶段曾超时；失败现场保留，不能
把后续成功当成该偶发问题已修复的证据。

服务器三种双核内核 ELF 与 [43](43-native-task-control.md) 的已验证映像
逐字节相同，本次新增同一用户测试。此前三架构网络集群的六应用 2/2/2
分配证据继续保留在该阶段归档，本轮没有重新执行该网络场景。

## 尚未完成

信号 9 当前针对传入句柄引用的任务，整组进程停止仍待实现。32 位高位信号
bitmap 传递、AArch64 偶发 init 引用下溢也没有在此阶段修复。指令模型、共享
RAM/仲裁 RTL 与服务器 QEMU 都不构成物理多 MCU 验收；尚未执行 JVM 或
真实 Minecraft Java 客户端，最终标准保持 [24](24-single-system-minecraft.md)。

## 最终版本的已完成检查

| 环境 | 实际结果 |
|---|---|
| 双 ARMv7-M 指令模型，seed 25 | 14,636,975 rounds；两个纯循环分别执行 141,675 和 38,116 条指令，覆盖两 CPU；14 次退出、最终堆完全恢复 |
| 双 ARMv7-M 指令 + 共享 RAM RTL，seed 26 | 14,600,734 rounds；两个纯循环执行 108,654 和 35,121 条指令，覆盖两 CPU；14 次退出、最终堆完全恢复 |
| RV64 / AArch64 / x86 双 CPU A20OS | 最终相同公共源码的纯计算停止、文件子程序、动态工作领取和全部原有检查通过；AArch64 首次超时另存，未宣称修复 |
| x86 单 CPU，KVM 与 TCG | 最终就绪同步版本的完整程序通过；两计算线程均返回 137，子程序均返回 37 |
| 原无文件共享 MCU | 原 5 次退出及全部检查通过；最终源码重建 ELF 与实际测试映像逐字节相同 |
| 默认精简 MCU | 重建 kernel.bin SHA256 仍为 `c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d` |
| ARM32 / RV32 | 相同最终父程序链接通过；已有 SDK 指针转换警告保留，没有计作启动验收 |

最终共享 MCU ELF SHA256 为
`29d48787adb326dd66407863601cb541fa8f25428cc95ac624e020e28fd9ec30`。
ABI、IDL、裁剪注册、共享 fabric 可移植性与文档检查通过，1498 个文档行号
引用有效；格式失败路径仍为父提交的 114 项，没有增加豁免。这些检查没有
测量整机性能，默认精简配置和服务器内核的逐字节比较只证明对应二进制未变。

证据入口为 `.kernel-build/cluster/evidence/native-compute-<commit>/manifest.json`。
归档保留最终和中间映像、同父程序负例、实际 FAT、串口日志、最终源码和
SHA256；负例的 90 个链接目标文件还能重新链接出逐字节相同的失败映像。

真实程序的调度记账、并发句柄所有权、就绪与等待中断修复见
[45-native-execution-reliability.md](45-native-execution-reliability.md)。

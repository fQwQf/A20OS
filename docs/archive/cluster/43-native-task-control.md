# 实际 Native 任务的停止和回收

## 公共实现

`task_kill`、`signal_check`、`signal_mask` 现由`kernel/abi/native/sys_task_signal.c` 提供。实现从服务器原有代码提取，MCU 可选 Native 程序配置接入相同 syscall 号、句柄解析、权限检查、信号队列和公共 Park/调度退出路径。没有增加板卡停止接口或改变已有服务器信号语义。 MCU 原先缺少对持久退出请求的消费：可中断等待被唤醒后，用户程序可能继续重复进入等待。现在在首次用户入口、syscall 分发前和 syscall 完成清理后调用公共退出检查；执行点位于任务自己的 Thread mode，避开异常入口和仍持有 futex 节点、映射 pin 或句柄引用的处理中途。未启用 Native 程序的精简 MCU 不链接这些函数。

信号 0 检查句柄，信号 9 请求所引用任务在安全边界退出；其他信号进入进程句柄表中的 bitmap，并尝试中断目标的可中断等待。普通信号在应用显式调用 `signal_check` 时消费。线程共享进程句柄表，也共享这里的 mask/bitmap，并不是每个线程各有一份 POSIX 信号屏蔽字。信号 9 不允许被该 mask 屏蔽。原有 `proc_force_exit` 保持持久的退出请求并处理 Park 接纳竞态，syscall 处理者不会直接销毁正在另一颗 CPU 上运行的任务。

本轮没有扩展整组进程停止语义：强制退出针对传入句柄引用的任务，不能据此宣称一个进程中所有未结束线程都已被同时停止。本项还没有增加新的执行能力 feature 位；旧能力协议的版本协商仍需单独处理。本阶段尚未验证只执行纯用户计算、始终不进入 syscall 的任务被远程强制停止；后续用户态中断返回边界和实际验收见 [44](44-native-user-preemption.md)。32 位寄存器 syscall 的高位 bitmap 传递也未在本轮完善或验收；下面实际检查的信号为 0、9、12 和非法 64。

## 实际用户程序

MCU 和服务器父程序都运行 `user/tests/native-task-control-contract.h`：

1. 先消费已有通知（服务器前置子进程退出可能留下 SIGCHLD），再分配真实用户栈并创建线程。
2. 工作线程尝试屏蔽 12 和 9，核验有效 mask 只保留 12，然后发布 ready。
3. 父线程检查非法信号和信号 0、确认任务仍运行，并把工作线程调度到另一颗在线 CPU（单 CPU 配置则留在唯一 CPU），发送 12 并唤醒其 futex。
4. 工作线程核验屏蔽时没有消费 12，解除屏蔽后准确消费一次，再经过调度点核验实际 CPU，发布 checkpoint，继续等待。
5. 父线程发送 9，等待真实退出码 137；核验对象身份保持不变、状态变为 RETIRED，已回收任务不能再解析或停止；关闭句柄并成功释放原用户栈。

整个流程是固件或服务器内核执行真实用户机器码和 syscall。主机没有写入 ready、checkpoint、退出码或句柄状态。发送 9 与下一次 futex 接纳可能先后发生，两种顺序都必须正常结束。本轮没有把调度到另一 CPU 的测试描述为负载均衡策略；实际动态工作领取与集群应用分配仍由 [40](40-native-application-balance.md)和 [37](37-application-execution.md) 验收。

新增控制线程后，文件配置完整 MCU 场景应精确观察 12 次退出；原无文件场景仍是 5 次。文件启动的两个子程序继续执行标准 CRT、参数/环境、文件读取和 32 项实际计算，见 [42](42-native-file-spawn.md)。

## 验证方式和边界

```sh
python3 tools/check-native-sync.py --cpus 2 --timeout 180 \
  --output .kernel-build/cluster/native-control-server-final2
# 独立构建目录保留了此前失败映像；不改变文件程序 profile。
make CCACHE= BUILD_DIR=.kernel-build/cluster/native-control-fixed-mcu \
  ARCH=armv7m BOARD=stm32f103 ABI=native STM32_FLASH_KB=512 STM32_RAM_KB=64 \
  STM32_QEMU=0 STM32_XUANWU=0 STM32_CLUSTER_PORT=0 NR_CPUS=2 \
  ALLOW_UNVERIFIED_SMP=1 CONFIG_KERNEL_PREEMPT=1 MCU_SHARED_KERNEL=1 \
  MCU_SHARED_RAM_KB=1024 MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=1 \
  NATIVE_FLAT_FILES=1 -j6 kernel-only
python3 tools/check-mcu-program.py .kernel-build/cluster/native-control-fixed-mcu/kernel.elf \
  --backend .kernel-build/cluster/shared-rtl-verilator/Va20_shared_top \
  --seed 23 --rounds 20000000 \
  --report .kernel-build/cluster/native-control-fixed-rtl.json
```

检查器要求实际 `NATIVE_TASK_CONTROL` / `PROGRAM-TASK-CONTROL` 标记和 MCU 的 0x0203、0x020f、0x0210 SVC，保留此前文件装载、动态计算、资源最终全部回收及 ABI/裁剪/文档检查。证据归档保存实际内核、用户 ELF、FAT 和逐项 SHA256。

首次测试未消费历史 SIGCHLD，因此错误地要求待处理信号为空；诊断定位后修正测试，保留失败日志。MCU 编译器还要求为测试的不可返回辅助函数明确标注 unreachable；修正后重新构建并重跑服务器最终源代码，旧成功结果另存为中间版本，不能替代最终映像验收。随后两种 MCU 交错在 20M rounds 超时；寄存器和候选栈返回地址诊断确认已执行 task_kill，先前 8 个任务已退出，目标仍在反复调用 futex，进一步定位并修复了上述退出请求消费缺项。这些失败映像与日志独立保留。诊断脚本在打印第二 CPU 的栈底后越过映射边界，附带的诊断读取错误也保留，不能当成生产内核错误。未扩大格式豁免。

仍未执行 JVM 或 Minecraft，也未在物理共享 MCU 上验收。AArch64 偶发 init 引用下溢仍待定位；通过当前功能检查不表示该问题已解决，也不代表全量性能测量已完成。最终目标和缺项继续见 [24](24-single-system-minecraft.md)。

## 最终结果

| 环境 | 结果 |
|---|---|
| RV64 / AArch64 / x86 双核实际 A20OS | 相同程序、信号屏蔽/消费、另一 CPU 上的目标停止、退出 137、身份终态与栈回收，全部通过 |
| 普通 x86 单 CPU A20OS | 同一接口与程序、工作领取、停止与回收通过 |
| 双 ARMv7-M 指令模型，seed 24 | 修复后 12 次退出、完整文件子程序和停止检查、全部堆恢复，通过 |
| 双 ARMv7-M 指令 + 共享 RAM RTL，seed 23 | 同一最终 ELF、12 次退出、完整程序和总线校验、全部堆恢复，通过 |
| 原有无文件 MCU 程序 | 修复后原 5 次退出及全部检查通过；该回归没有连接 RTL |
| 三架构真实网络集群 | 六程序活动数 2/2/2、48 次退出/回收、12 次 CLI 参数程序、远程停止返回 137，通过 |
| ARM32 / RV32 | 同一公共父程序链接通过；已有 SDK 指针转换警告保留，没有计作内核启动或实机验收 |

最终共享 MCU ELF SHA256 为`4363b47ced5ca023970924f21d0d1c6751ca4f5089a7accc7c879250a2010f95`。默认精简 MCU kernel.bin 保持`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。 ABI、IDL、裁剪注册与文档门禁通过；格式失败路径仍与父提交相同，为 114 项，没有新增豁免。证据入口为`.kernel-build/cluster/evidence/native-control-<commit>/manifest.json`，保存最终及明确标注的中间映像、日志、源代码和 SHA256。服务器的三项 syscall 函数经规范化文本逐一比较，与提取前实现相同；MCU 的退出消费边界为本轮实际修复。

这些结果证明模型或虚拟机中的实际程序执行，尚不构成物理多 MCU 或 Minecraft Java 客户端已启动的证据。

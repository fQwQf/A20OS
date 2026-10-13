# 多 MCU 原生单内核与 Minecraft Java 客户端

2026-10-12。用户明确要求一个 A20OS 原生内核直接跨多个物理 MCU 运行，
允许增加共享 RAM 和硬件仲裁。先实现功能，不要求帧率、运行速度或可玩性，
保留现有功能。此前“每 MCU 运行独立承载内核，再共同模拟一台客户机”的
候选方案已被此目标取代。

后续进展：x86_64 双 QEMU 同一原生内核已实际运行单个 Minecraft 客户端，
欢迎页、主菜单、设置、键盘交互和正常退出通过，见
[61](61-shared-minecraft-client.md)。这是跨实例的系统机制证据；下面的
Cortex-M 原型和物理 MCU 运行时要求仍需各自完成，其他 ISA 可能需要额外移植。

## 1. 当前判断

允许共享 RAM 后，可以把多个 MCU 作为同一原生内核的多个处理器：本地
Flash 执行相同固件，共享 RAM 提供唯一的内核对象、任务栈和堆，仲裁器提供
跨芯片同步。实现契约、构建方法及证据见
[25-native-shared-kernel.md](25-native-shared-kernel.md)。

现已通过两个 Cortex-M3 指令执行环境运行实际 ARM 固件，完成主从启动、
共享 allocator、原子操作及同一线程跨 CPU 迁移。这是原生共享内核端口的
功能原型；尚未在物理 MCU 上运行，也未执行 JVM 或 Minecraft。

Minecraft Java 客户端最终是否能启动，仍需实际完成并验证运行时闭包。
放弃性能要求可以采用 Java 字节码解释执行、软件渲染，但不会消除 Java
类库、本地库、内存、文件、输入和图形接口的要求。JVM 是 Java 应用运行时，
这里不再用整机模拟或客户机 OS 替代原生内核。

## 2. 应用设计

```mermaid
flowchart TB
    G[真实 Minecraft Java 客户端] --> J[Java 运行时与类库]
    G --> L[所需本地库、软件图形与输入]
    J --> K[一个 A20OS 原生共享内存内核]
    L --> K
    K --> C0[物理 MCU0：原生执行]
    K --> C1[物理 MCU1：原生执行]
    C0 --> R[硬件仲裁器与共同 RAM]
    C1 --> R
```

第一步选择同 ISA、同 ABI 的 MCU，避免把任意机器码跨 ISA 迁移作为前提。
最小演示使用 STM32F103 类芯片是为了验证内核机制，不意味着默认 20 KiB
SRAM、Xuanwu 的 1 MiB 外部 SRAM或原型的 1 MiB 共享 RAM足以启动客户端。
普通单端口外部 SRAM不能靠连接第二块 MCU 自动变成符合同步契约的共享内存。

完整应用的 MCU 型号、数量、共享 RAM 容量和版本还未锁定。用户允许旧版，
应先在普通 A20OS 上固定一个真实客户端、JVM、本地库及资源，显示主菜单并
测内存和存储需求，再据此选择能直接寻址足够 RAM 的 MCU/控制器。不可仅把
各 MCU 的内部 SRAM 容量相加宣称形成应用堆；新的硬件内存窗口也必须反映在
链接、allocator、用户态内存布局和驱动中。

## 3. 需要补齐的实现

最初 MCU trim 只运行内核线程；现有可选应用模式已运行 ARM 静态 PIE、PSP
非特权应用和部分 Native 系统调用，完成休眠期间迁移，见
[26-native-programs.md](26-native-programs.md)。它仍没有完整 Java 用户态环境。
集群公共机制不以 STM32 为限，跨平台契约与缺口见
[27-portable-domains.md](27-portable-domains.md)。需要继续补齐：

1. 物理共享总线和仲裁器，主从固件启动及任务迁移的板上验收。
2. NOMMU 用户态入口、系统调用、可执行文件/动态库装载或明确的原生运行时
   装载方式；文件系统、时间、线程同步、共享 heap 和本地内存访问语义。
3. 能在所选 MCU ISA/ABI 上原生运行的 Java 解释器、相应类库和本地依赖。
   已有 x86_64 JVM/LWJGL 二进制不能直接在 Cortex-M 上运行。
4. 所选版本要求的真实软件图形路径、显示设备和输入事件。声音可关闭。

仓库已有 `tools/fetch-minecraft.sh` 和桌面包材料，默认载荷为 1.21.11。
组件依赖见 [Minecraft 组件记录](../distro/minecraft-components.md)，呈现路线见
[GPU 路线](../graphics/gpu-3d-roadmap.md)。这些材料可复用，但不证明当前
共享内核端口已有游戏运行能力。按
[Minecraft 1.20.5 官方说明](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-20-5)，
该版本要求 Java 21 和 64 位 OS；因此不能把现代桌面载荷直接作为当前
32 位 Cortex-M 原型的默认运行目标。旧版依赖需逐项实际验证。

## 4. 完成标准

- 至少两块物理 MCU 运行同一原生 A20OS 内核，使用同一个 PID 空间和共享 RAM。
- MCU 侧原生运行真实 Java 客户端，显示主菜单并响应一次真实输入。
- 保留硬件拓扑/容量、固件与运行时/游戏哈希、串口日志和实际屏幕证据。
- 不设 FPS、启动时限或创建世界要求；如实记录耗时。
- 主机可以构建和调试；最终演示中的 JVM、渲染和应用内存均由已声明 MCU
  硬件承载。独立 OS 的 RPC 算子、服务器版、方块演示、宿主游戏串流或测试
  日志中的菜单文字，不能替代这个验收。

这个目标尚未完成。当前已把路线落实为可编译、可验证线程迁移的原生
共享内核端口，后续必须以物理硬件和完整客户端启动证据继续推进。

## 2026-10-13 运行时基础进展

公共 Native 线程和共享工作领取已实际执行，见 [32](32-native-threads.md) 与
[40](40-native-application-balance.md)。可选 flat 文件后端现在能让同一用户
程序在共享 MCU 内核和三种服务器 ISA 上读取实际文件，并由两个线程继续
共同处理计算任务，见 [41](41-native-files.md)。该后端当前只读、短文件名，
MCU 使用 FAT 扇区 fixture；这一步没有执行 Java 字节码或加载 Minecraft，
下一步仍需在真实 Native 程序中逐项接入 Java 运行时及所选客户端依赖。

从文件装载真实 Native 子进程、标准 CRT、argv/envp 和子程序双线程计算路径
已继续接入，见 [42](42-native-file-spawn.md)。这使文件读取继续进入实际程序
启动路径，仍未执行 Java 字节码或真实客户端。

实际 Native 任务的公共停止、信号检查点及资源回收见
[43-native-task-control.md](43-native-task-control.md)。

持续纯计算 Native 线程的用户态时间片、停止与实际执行检查见
[44-native-user-preemption.md](44-native-user-preemption.md)。

真实程序的调度记账、并发句柄所有权、就绪与等待中断修复见
[45-native-execution-reliability.md](45-native-execution-reliability.md)。

无 CPU 绑定的真实 pthread 执行、空闲 CPU 接取和线程组终结通知见
[46-unbound-program-execution.md](46-unbound-program-execution.md)。

真实 JamVM Native 启动、完整类库构建和文件映射修复见
[47-jamvm-native-bootstrap.md](47-jamvm-native-bootstrap.md)。

真实线程通知与等待已接入，JamVM 类库初始化推进到 Native JNI 库装载；
见 [48](48-native-thread-signals.md)。Java 应用、GC 和物理多板客户端仍未验收。

真实 Java 多线程计算、Native JNI 文件操作和单核/双核自动执行验证见
[49-native-java-execution.md](49-native-java-execution.md)。Java 应用阶段已通过；
GC、物理 MCU Java 运行时与 Minecraft 客户端仍待继续实现。

原生 Java 的并发回收、栈根保存和实际对象移动已在 RV64 Native 单核/双核
验证，见 [50-native-java-gc.md](50-native-java-gc.md)。物理多 MCU、其他架构
完整 VM 和真实 Minecraft 客户端仍未验收。

完整 Native Java 运行时已继续接入 AArch64，并通过同字节码的单核/双核
计算与 GC 压缩，RV64 回归同时通过；实际次数、JNI 修复和剩余范围见
[51-native-java-portable.md](51-native-java-portable.md)。物理多 MCU 和
Minecraft Java 客户端仍待实现与验收。

main 的真实 Minecraft 客户端依赖已合入；本 worktree 的 x86 A20OS 内核
实际显示 1.21.11 主菜单，键盘交互及正常退出通过，Native/MCU 回归与准确
边界见 [52-main-minecraft-integration.md](52-main-minecraft-integration.md)。
物理多 MCU 客户端仍未完成。

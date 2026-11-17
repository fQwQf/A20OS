# 合入 main 后运行真实 Minecraft 客户端

集群 worktree 合入 main 的 `85fa78d2` 及其祖先提交，包括 Minecraft 启动器/配置、Wayland/virgl、GPU 共享与同步、输入、线程组退出和通用 MM 修复。保留 `codex/unified-cluster` 分支和既有 Native 实现，未修改 main 工作目录或其正在运行的 AArch64 QEMU。

## 真实客户端结果

在本 worktree 构建的 A20OS x86_64 四核抢占内核上，使用已有生产 XFCE world 的独立副本，实际运行 Minecraft Java 1.21.11 demo：

- 显示欢迎界面，Tab/Enter 完成初始设置并进入主菜单。
- Tab/Enter 打开 Options，Escape 返回主菜单，画面与按键事件均有记录。
- 从菜单选择 Quit Game，日志显示 `Stopping!`，启动脚本取得退出码 0。

运行时是 OpenJDK 21.0.12，通过 A20OS 的 Linux ABI 执行，窗口使用 Wayland 和 virgl。客体内核仍是 A20OS。该路径复用通用内核线程/调度/文件和设备机制，应用没有 CPU 绑定；本次只核验在线 CPU=4，没有把它当作 Minecraft 各线程的 CPU 利用率均衡测量。

这验证的是完整 MMU 主机上的真实客户端启动和键盘响应，不是 JamVM 执行现代 Java 21 客户端，也不是物理 MCU 客户端。本次没有进入或保存世界，没有验收音频、账号、联机、鼠标转向。主菜单和 Options 截图保存在`.kernel-build/cluster/minecraft-main-integration/client-01/`。

生产 world 原始 SHA-256 为`19418c7d553b320a977542a9b215a35bf406aa07095dc6fe30458906b9c6b194`；本次客户端内核 SHA-256 为`544c7b925cbca8ce5f814142d17def564bc1ed7bfbdcb794aed781d4ce92b666`。复制后仅向测试副本写入 QA 启动入口和当前 launcher/java wrapper。原生产镜像、已有玩家目录和 main 的运行实例保持。

## 合并兼容处理

双方都修过组退出通知。保留 Native 已有的组完成、一次通知和父引用 helper；Linux/BOTH 构建同时引用真正的组 leader，用 main 的`signal_notify_child_exit` 填写 CLD_EXITED、PID、UID 和退出码。纯 Native 裁剪继续使用已有信号入口，不依赖 Linux siginfo。没有留下两套相互独立的“已通知”字段。任务初始化同时保留 Native 信号 mask 和 main 的 pending siginfo 指针初始化。

MCU 的 cgroup stub 更新到 main 的 task 指针接口；新增的`resource_ns_init` 在精简 Native MCU profile 提供启动适配。这种 profile 不含 Linux resource namespace syscall/表，不以空初始化宣称支持其隔离功能。共享实例和对象机制保持原实现。slab 冲突只涉及等价注释，保留禁用 IRQ 期间 refill/drain 的既有修复。

main 的 dispatch ownership 回归会抽取实际 steal 函数，新增了集群已有的 idle hint 依赖；harness 补齐该依赖，原来的同时 wake 负控仍能复现重复入队。另清理合入测试的一处行尾空白。

## 原有功能回归

| 路径 | 实际结果 |
|---|---|
| RV64 Native Java | 新内核一轮计算、文件、JNI、48 次 GC/堆压缩通过，CPU mask=3，QEMU 自然退出 0 |
| AArch64 Native Java | 同样一轮通过，CPU mask=3，QEMU 自然退出 0 |
| x86_64 Native SDK | 四核计算、文件/子程序、对象/实例、停止和同步通过，动态工作 mask=0x3 |
| 双 MCU 处理器模型 | 新固件执行实际用户指令/SVC，32 项任务总和 4359168、mask=3，文件、子程序、取消及对象回收通过 |
| 真实 Linux SIGCHLD 程序 | leader 先退出不提前通知；最后成员退出后信息正确、可 waitpid，程序退出 0 |

MCU 模型 seed=26，在 15439859 轮交错后自然达到检查条件；两个 CPU 分别执行 176761/396669 条用户指令、1022/1564 次 SVC。模型支持不等于物理板验收。RV64/AArch64 使用 [51](51-native-java-portable.md) 的冻结 VM 和相同字节码，只有内核来自合并结果。

x86 的 q35 `poweroff` 输出后仍保持停止：Native SDK 原有 checker 明确记录由主机 SIGTERM 收尾；独立 Linux 组通知 runner 因关机截止时间而保持`passed=false`，其程序 PASS/退出码 0 单独保存在`program-only-verification.json`。不把这些记录列为 QEMU 自然退出通过项。 Minecraft 自身正常退出与测试 VM 的关机结果分开记录。

ABI 边界/覆盖、外部依赖边界、架构静态门禁、MM 锁模型静态门禁、调度实际函数回归及文档漂移检查通过。完整 `make check-arch-boundary` 的 ARM32 MMU 矩阵构建因缺少工具链未完成；静态门禁单独执行，不能把它替代完整矩阵。缺少模型依赖、tmpfs 解包空间不足和工具 PATH 错误的尝试均保留日志，后续使用隔离依赖目录与既有 ARM 工具链执行成功。

独立证据以合并提交短 SHA 命名于`.kernel-build/cluster/evidence/minecraft-main-<commit>/manifest.json`，包含源码快照、运行报告、输入/输出哈希、QA 脚本、按键和截图。运行后 world 是有变化的测试磁盘，不能把它称作初始输入镜像；原生产 world 的哈希与注入配方另行保留。

继续实现的目标保持 [24](24-single-system-minecraft.md)：至少两块物理 MCU 上同一原生 A20OS 的真实 Java 客户端。此次主机客户端验证与 Native 跨架构执行回归分别提供依赖和机制证据，没有替代该最终验收。

统一 appd/appctl 入口现已覆盖 Linux ABI 程序，并验证三架构真实程序 2/2/2 分配及完整 Minecraft 客户端菜单、输入、退出；能力、部署与验收边界见[53-linux-application-execution.md](53-linux-application-execution.md)。

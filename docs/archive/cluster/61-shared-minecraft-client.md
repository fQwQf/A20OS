# 单个 Minecraft 客户端跨两个 QEMU 实例

2026-10-13，x86_64 A20OS 已在两个独立 QEMU/KVM 进程提供的 CPU 上运行同一个真实 Minecraft Java 1.21.11 demo 客户端。欢迎页、完整主菜单、Options、 Escape 返回和 Quit Game 正常退出均已实际检查；普通双核单实例对照也通过。

这里沿用 [54](54-stock-qemu-shared-kernel.md) 的同一内核、任务表、地址空间、共享 RAM 和调度器，以及 [56](56-shared-device-owner.md) 的设备 owner 路径。使用系统已安装的 QEMU 10.0.13，没有下载或编译 QEMU 源码。

## 运行状态和执行路径

两进程各一个 vCPU，映射同一 3 GiB RAM 文件。只有主实例连接系统磁盘写层、只读测试 payload、GPU、键盘、鼠标和无输出音频设备；次实例没有这些设备。此 RAM 容量包含 PC 的低地址和 4 GiB 以上区域，不能把 RAM 文件大小理解为 guest 连续低地址范围。QMP 和 `/proc/PID/maps` 留存了实际共享映射信息。

RAMFS 中的普通 Linux 测试程序在 CPU 1 挂载原系统的 qcow2 写层和运行时文件系统，子进程 chroot 后允许 CPU 0/1 运行。测试 payload 只部署当前 Java 启动脚本、Minecraft 启动脚本、Native 应用服务和自动启动脚本，再复用镜像现有 `/sbin/init`、设备服务、Wayland/labwc 与 Java/图形依赖。

客户端通过已有 cluster-supervisor、appd、app-exec、appctl-run 启动，沿用同一应用身份、根任务、查询与退出语义。OpenJDK 使用正常 JIT；设置`-XX:ActiveProcessorCount=2`，其余堆参数沿用现有 Minecraft 启动器。没有为 Minecraft 拆分成多个客户端，也没有在宿主执行游戏逻辑。

## 跨 CPU 证据与负载含义

`/proc/PID/stat` 现保留原有前 24 字段并扩展至 Linux 第 39 字段 processor。 CPU 赋值通过调度器快照 API 读取；这只在读取 procfs 时增加工作，不给普通调度热路径加入新的计数器。测试程序分别绑定 CPU 0/1，核对该字段与自身 sched_getcpu 一致，输出 `CLUSTER_PROC_CPU: PASS field=39 cpus=0,1`。

客户端观察器每十秒读取同一 Java 根进程的 `/proc/PID/task/TID/stat`。共享运行 owner-minecraft-02 的根 PID 为 145，记录了 37 个线程、2082 条完整快照；普通 SMP 对照也是 PID 145（不同运行），39 个线程、2232 条快照。共享运行的渲染线程、C2 编译线程和 GC Thread#0 都留下了 CPU 0/1 赋值，其累计 guest CPU ticks 持续增加。没有对这些 Minecraft 工作线程逐一绑核；分配与迁移由已有通用内核调度器处理。

这些记录证明本次同一 JVM 的线程处于两个实例提供的 CPU 的调度域，并与此前 [59](59-shared-jvm-execution.md) 的实际双 CPU Java 计算证据衔接。 processor 是调度 CPU 的采样，累计 ticks 是线程总时间；它们不是逐 CPU 运行时长剖析，不能据此宣称游戏负载严格各占 50%，也没有测量帧率或吞吐量。

## 图形、输入与生命周期

人工查看实际 VNC 帧缓冲，确认以下顺序：

1. Minecraft Java Edition 欢迎页；
2. Tab 导航到 Continue，Enter 进入完整 demo 主菜单；
3. Tab 导航到 Options，Enter 打开设置页；
4. Escape 返回主菜单，Tab 选中 Quit Game；
5. Enter 正常退出，appctl 和会话均返回 0。

输入经主实例 QMP 的真实键盘设备进入 guest，没有向游戏界面直接注入结果。按下与释放分开发送，间隔 150 ms；批量同帧按键曾未产生预期的逐键导航，保留原始交互记录，最终界面验收使用分开的按键事件。

virgl GL 输出可能没有 QMP screendump 可读的 surface。因此启动时启用本地 Unix VNC；截图工具在 screendump 返回 no surface 后读取实际 VNC 原始像素。它不合成游戏画面。此前 [60](60-shared-graphics-input.md) 的 GPU 像素回读、 DMA-BUF、fence 和真实键盘鼠标事件验证也继续作为底层证据。

Quit Game 后正常回收应用服务并结束 compositor。系统服务仍可能持有挂载，随后由测试工具停止本次 QEMU 子进程；没有声称完成整套桌面 daemon 的独立卸载或保存游戏世界。测试没有连接网络，账号、Realms 相关失败日志不作为离线菜单启动失败；音频使用 No Output。

## 复现

需要已有 x86_64 Minecraft 系统镜像、KVM、支持 virgl 的系统 QEMU、render node、qemu-img、mkfs.ext4，以及截图工具需要的 Python Pillow。新建证据目录：

```sh
IMAGE=<repo>/build/images/xfce-x86_64.img
RUN=.kernel-build/cluster/multi-qemu/minecraft-new
python3 tools/qemu/check-multi-qemu-kernel.py --workload minecraft \
  --system-image "$IMAGE" --out "$RUN" --timeout 900 --jobs 4
```

该命令保持运行，等待客户端正常退出。在另一个终端观察和输入：

```sh
python3 tools/cluster-qmp.py "$RUN" shot welcome.png
python3 tools/cluster-qmp.py "$RUN" key tab
python3 tools/cluster-qmp.py "$RUN" key ret
python3 tools/cluster-qmp.py "$RUN" key esc
```

按实际焦点重复导航，截图使用新文件名，不覆盖已有证据。欢迎页包含文本焦点，不能假定固定 Tab 次数总能选中 Continue。选中 Quit Game 后 Enter，原检查器会核对退出并清理自身实例。普通对照添加 `--mode smp` 并使用新目录。可用 `--rendernode` 指定实际节点；默认 RAM 为 3072 MiB，其他用例仍为 512 MiB。`--memory-mib` 支持测试工具当前的 512..3072 MiB 范围。

基础 4 GiB 系统镜像始终只读作为 backing，每次运行前后核对完整 SHA256：`19418c7d553b320a977542a9b215a35bf406aa07095dc6fe30458906b9c6b194`。三次运行的写层实际分别约 2.57、2.63、2.63 MiB；没有复制完整系统磁盘。临时共享 RAM 只在本次 QEMU 全部停止后删除，不处理其他任务的进程。

## 证据检查

目录均在 `.kernel-build/cluster/multi-qemu/`：

| 目录 | 结果 |
| --- | --- |
| owner-minecraft-01 | 客户端实际启动、跨 CPU 线程观察和 Alt+F4 正常退出；未启用 VNC，不能作为菜单画面验收 |
| owner-minecraft-02 | 双实例同内核；欢迎、主菜单、Options、返回、Quit Game、退出 0 通过 |
| owner-minecraft-baseline-01 | 普通 SMP 的相同客户端场景通过 |

检查器的 report PASS 仅自动确认运行和退出契约；画面必须另行检查。`minecraft-shared-evidence.json` 汇总三次原始 report、冻结 ELF/ROM、payload、写层、日志、截图和 QMP 交互哈希，不改写早期报告。后两次分别已查看 welcome.png、main-menu-ready.png、options.png、quit-selected.png。

```sh
python3 tools/archive/audit-shared-minecraft.py "$RUN" --screenshots-reviewed \
  --out .kernel-build/cluster/multi-qemu/minecraft-audit-new.json
```

只有实际看过上述截图才能使用 screenshots-reviewed；文件存在和哈希不等于界面正确。审计工具额外核对同一 JVM 的线程证据；普通冻结内核不含 shared MMIO/设备 owner 符号。独立负例删除实际 CPU 1 记录、拆成不同 JVM 根身份，两者均被线程检查器拒绝。

## 支持边界与后续工作

Minecraft 已知可运行架构仍只限 x86_64。其他平台可沿用统一集群接口，但 JVM、LWJGL、本地动态库和图形栈可能需要移植，必须单独执行验收。本次不是物理 MCU 的硬件验证，也不证明混合 ISA 可以直接共享同一指针 ABI 的堆。

设备仍采用主 CPU 启动时的 embedded 初始化；任意 CPU 的动态 PCI 探测与配置空间 owner 路由、owner 故障恢复仍需继续完善。物理多板的共享 RAM、原子仲裁、缓存一致性、Java 运行时闭包及完整故障验收仍属于原始目标，不能因这次 x86_64 客户端通过而标为完成。

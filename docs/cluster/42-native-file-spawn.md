# 从文件启动 Native 程序

## 公共接口和执行路径

共享 flat 内核现在提供与服务器同号、同结构的 `task_spawn`。应用先用
`path_open` 取得带 READ/EXEC 权限的文件句柄，调用 `task_spawn`，再使用
`task_wait` 和 `handle_close`。v1 的 72 字节结构与 v2 结构的头部验证由
`kernel/abi/native/spawn_params.h` 共用。没有增加板卡 syscall，也没有新增
后台服务；可选 flat 文件配置不启用时保留原启动方式。

flat 路径把参数和环境字符串复制为内核快照，读取不可变文件的完整映像，再
通过 `core/flat_program.h` 的 ISA 适配器准备一个尚未发布的任务。公共 Native
代码构建标准 `a20_start_info_t`、子进程自己的 argv/envp 和 self_task 句柄。
返回给父进程的任务句柄及用户结果复制都成功后，才交给公共调度器执行。
新进程具有独立映像、初始栈、flat 用户空间和句柄表；用户线程继续共享其所属
进程的映像与堆。文件读取快照不改变打开文件的偏移，所有存储访问复用既有
FAT 扇区回调和共享 mutex。

ISA 适配器负责 ELF/寄存器/栈入口，公共代码负责参数、对象引用和发布回滚。
当前新增实测 ISA 适配器为 ARMv7-M；已有服务器 full VFS/VM 装载路径保留。
从文件公开启动要求 `PT_A20_START_INFO` 标记，并继续校验机器类型、Thumb
入口、EABI、段、重定位和导入。已有明确自测入口的嵌入式 PIE 装载方式保留，
不会因为缺少这个标记被新路径替换。

## 当前 flat 配置的范围

配置继续为 `MCU_NATIVE_PROGRAM=1 NATIVE_FLAT_FILES=1`。flat 打开文件现在可
授予 EXEC，调用者仍可缩减权限；可执行文件必须经过装载验证。当前上限为
512 KiB 文件、256 KiB 映像跨度、16 KiB 初始用户栈，每类最多 32 个参数/
环境变量、两类字符串合计 4096 字节。适配器与配置均可继续扩展，这些数值
不是 Minecraft 的容量测量或最终硬件选型。

当前支持静态 PIE 和标准 Native CRT/main，argv/envp 使用目标机器的指针宽度。
不支持动态解释器、动态库、TLS 段、普通 MMU 地址空间接口。flat v2 的 stdio
继承、目录/事件队列句柄及显式句柄转移尚未实现，明确请求这些能力返回
NOT_SUPPORTED；v1 缺失的后缀不被当作 stdio 请求。普通服务器原有功能保留。
父退出绑定沿用公共 pdeathsig=9 字段，本轮实际子程序按自身返回码退出，没有
把接收这个标志的检查描述为父进程死亡场景已覆盖。

执行能力协议没有新加未经版本协商的 feature 位；该配置未公布完整 HANDLES。
跨网络域仍通过 [37](37-application-execution.md) 的应用 owner 与 ISA 变体启动，
本项没有把不同 ISA 的机器码或线程栈混用，也没有把网络域变成同一 RAM。

## 实际程序检查

`user/tests/native-spawn-contract.h` 是 MCU 与服务器共用的父程序。它打开
`/bin/SPAWN.ELF`，使用同一文件句柄分别以 v1、v2 启动两个独立子进程，传入
四个参数和两个环境变量，包含空参数、带空格参数和空环境值。每次等待真实
退出码 37，核验任务对象身份、终态与关闭。拒绝新版本、未知 flags、没有 EXEC
权限的文件和普通数据文件。flat 另外验证错误 argv 地址和不支持的句柄转移。

`user/tests/native-spawn-child.c` 是各 ISA 共用的实际子程序：从标准 CRT 进入
main，核验初始参数、自身任务句柄和运行实例，读取文件，并创建两个工作线程
共同领取 32 项任务。子程序核验计算总和 4359168、没有重复/遗漏以及多 CPU
参与后才返回 37。没有宿主写入任务状态、伪造退出码或代替子程序计算。
MCU 检查保留原用户线程、共享堆/futex 和退出后全部资源回收，预期实际退出
从 5 次增加到 11 次；原无文件配置仍要求原来的 5 次。

`tools/check-native-spawn-admission.py` 另在三种宿主 ISA 上编译生产 flat 启动
处理函数，通过明确的依赖适配器注入参数复制、分配、文件读取、装载、子/父
句柄安装和结果复制失败。十二条路径和五类参数边界（空向量、精确上限、字符串超限、个数超限和
缺少终止项）核验引用/分配全部释放，失败的子任务不
发布。这个单元检查使用模拟依赖，不能代替前面的实际 ELF/内核运行。

```sh
python3 tools/check-native-sync.py --cpus 2 --timeout 180 \
  --output .kernel-build/cluster/native-spawn-server
make CCACHE= check-native-spawn-admission
# 与 41 相同的 file profile；构建会把真实子程序写入 FAT fixture。
# 使用 Unicorn/pyelftools Python 与 RTL backend 执行新生成的 kernel.elf：
python3 tools/check-mcu-program.py <kernel.elf> --backend <Va20_shared_top> \
  --seed 22 --rounds 20000000 --report .kernel-build/cluster/native-spawn-mcu-final.json
```

MCU fixture 是真实 FAT32 的无损稀疏只读扇区镜像，当前包含数据文件和已链接的
Native 子程序；生成报告同时保存磁盘、数据和子程序 SHA256。它证明固件从
文件装载执行的路径，不证明物理 SD 控制器、多板或所选 MCU 的最终存储容量。

## 仍需完成

本项尚未启动 JVM 或 Minecraft，仍需完成 Java 运行时、所选客户端依赖、显示/
输入和物理共享 MCU 的完整验收，见 [24](24-single-system-minecraft.md)。
AArch64 偶发 init 引用下溢仍未定位。默认普通 MCU 二进制和既有程序分别检查，
但没有完成全量性能测量；不以功能检查代替性能结论。

## 2026-10-13 最终结果

| 执行环境 | 结果 |
|---|---|
| RV64 / AArch64 / x86 双核真实内核 | 各两次文件启动、四参数两环境、子程序计算后退出 37；原文件/线程/工作领取检查全部通过 |
| 双 ARMv7-M 指令 + 共享 RAM RTL | 同一父/子程序、标准 CRT、11 次退出、最终全部堆资源恢复，通过 |
| 原有无文件 MCU 程序 | 原五次退出与全部检查通过；该回归未连接 RTL |
| 三架构真实网络集群 | 六程序 2/2/2、48 次退出/回收、12 次参数 CLI、远程停止退出 137，通过 |
| 生产 flat 启动处理函数的宿主依赖适配测试 | 三 ISA 各十二条发布/回滚和五类参数边界，通过 |

默认精简 MCU 的实际 kernel.bin SHA256 仍为
`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。
最终文件启动 MCU 内核 ELF SHA256 为
`bf79a28039f571b9943fbf4e8c28ca392a39f43561368ad4050b68e92b41c236`。
当前 FAT fixture 非零扇区为 46 个，约 23 KiB；逻辑磁盘 64 MiB 不代表
消耗了相同容量的 MCU RAM/Flash。最终镜像、子程序和完整磁盘均冻结并核对
哈希；包含锁补充之前的中间运行及测试适配器编译/断言失败记录，分别标注。

ABI、裁剪注册、IDL、文档门禁通过。格式门禁仍为父提交同一组 114 个失败
路径，未增加豁免。归档入口为
`.kernel-build/cluster/evidence/native-spawn-<commit>/manifest.json`，逐项保存
源文件、报告、日志、用户程序和内核二进制哈希。所有上述硬件执行证据为
模型或虚拟机，不表示真实 Minecraft 客户端或物理多 MCU 已运行。

后续公共停止接口和新增控制线程的 12 次退出验收见
[43-native-task-control.md](43-native-task-control.md)；本页 11 次退出是 a263ef17 阶段的
已归档结果，未改写为后续映像结果。

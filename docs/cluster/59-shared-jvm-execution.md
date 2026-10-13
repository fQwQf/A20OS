# 同一个 JVM 跨两个 QEMU 实例执行

在 [58](58-shared-filesystem-execution.md) 的磁盘程序加载基础上，已实际启动
Minecraft 镜像中的 OpenJDK 21，并让同一 JVM 的两个普通 Java 线程在两个
独立 QEMU 实例中执行、切换 CPU 和领取共享计算任务。此阶段不修改内核 ABI，
使用已有 Linux 进程、线程、动态链接、文件系统及共享设备实现。

## 真实运行路径

主 QEMU 挂载已有 4 GiB 系统镜像的 qcow2 overlay，次 QEMU 没有磁盘或 UART。
两者各一个 vCPU，通过同一 RAM inode 运行同一个 A20OS 内核。已有镜像只作为
只读 backing，运行前后计算完整 SHA256；不为测试复制一份完整系统磁盘。

RAMFS 中的普通 Linux 测试程序先完成原有线程和控制台验证，再在 CPU 1 挂载
系统 ext4，并为新根挂载 devtmpfs、proc、sysfs 和临时 tmpfs。子进程 chroot
后直接 exec `/usr/lib/jvm/java-21-openjdk/bin/java`。运行时为现有镜像中的
OpenJDK 21.0.12+8-alpine-r0，版本用例在 CPU 1 完成初始化、打印版本并退出 0。

Java 双线程用例由 `tools/shared-jvm-test/ClusterProbe.java` 和小型 JNI 库
构成。宿主只编译 class 和 JNI 动态库，将其放入第二块只读 ext4 盘；执行
发生在 guest JVM，宿主没有运行这些计算来替代 guest 输出。

JNI 库只有两个方法，分别调用 libc 的 sched_getcpu 和 sched_setaffinity。
其不含集群 syscall，不访问宿主状态，也不计算工作结果。链接时不引入宿主
libc，动态符号由 guest 的 musl 解析。Java 运行时和测试程序没有修改过 JVM
线程模型；JNI 用于确定实际执行 CPU，并主动覆盖迁移路径。

## 执行与工作分配验收

Java 主进程允许在逻辑 CPU 0/1 上运行，两个工作线程各执行 8 轮交替 CPU
绑定，并核对 sched_getcpu 返回值，共 16 次亲和性检查。注意这不是声称
16 次都发生了迁移：每线程首次绑定可能与初始执行 CPU 相同，其余交替轮次
才必然需要切换。

然后两个线程分别绑定 CPU 0/1，通过 AtomicInteger 从同一个共享队列领取
512 个任务。每线程先领取一个真实任务并等待启动屏障，保证两者均参加；
其余任务动态领取。每项分配一个 Java 数组、计算结果，并通过共享访问计数
检查没有任务遗漏或重复。Thread.join 后核对：

- 两个线程均完成工作，实际 CPU 掩码为 3；
- 512 个任务各执行恰好一次；
- 两线程结果总和为 1084243968；
- JVM 正常退出 0，父进程正常回收、卸载各文件系统，并退出 0。

这证明单个真实 JVM 的线程、堆、同步和执行状态可以跨当前两个实例使用。
它没有证明内核会自动把任意串行 Java 方法拆成并行任务；这里是正常多线程
应用利用同一个内核调度器，工作均衡的单位为领取的任务而非执行时长。

测试使用 `-Xint -Xms16m -Xmx96m -XX:ReservedCodeCacheSize=32m`，优先验证
功能，不以 JIT、吞吐量或游戏帧率作为本阶段条件。

## 复现

需要系统 QEMU/KVM、qemu-img、mkfs.ext4，以及包含 jdk.compiler 模块的
宿主 Java 21 和 x86_64-linux-gnu-gcc。测试通过 `java -m
jdk.compiler/com.sun.tools.javac.Main` 编译，不要求单独的 javac 启动文件。
用 `-source 21 -target 21` 固定 class 版本，不需要 JRE 缺少的 ct.sym 归档。

```sh
# 现有原始 ext4 系统镜像；仅用作只读 backing。
IMAGE=<repo>/build/images/xfce-x86_64.img

python3 tools/check-multi-qemu-kernel.py --workload jvm \
  --system-image "$IMAGE" \
  --out .kernel-build/cluster/multi-qemu/jvm-version-new --timeout 240

python3 tools/check-multi-qemu-kernel.py --workload jvm-threads \
  --system-image "$IMAGE" \
  --out .kernel-build/cluster/multi-qemu/jvm-threads-new --timeout 240

python3 tools/check-multi-qemu-kernel.py --mode smp --workload jvm-threads \
  --system-image "$IMAGE" \
  --out .kernel-build/cluster/multi-qemu/jvm-threads-smp-new --timeout 240
```

检查器保留冻结 ELF/ROM、源文件哈希、QMP 状态、共享内存映射、独立串口日志、
编译命令、只读 Java payload 和 qcow2 写层。原镜像 SHA256 为：
`19418c7d553b320a977542a9b215a35bf406aa07095dc6fe30458906b9c6b194`。
三个成功运行在结束后均验证其未变；两个 Java 工作运行的 overlay 实际仅约
772 KiB。临时共享 RAM 在本次进程停止后删除，不触碰其他任务的 QEMU。

证据位于 `.kernel-build/cluster/multi-qemu/`：

| 运行 | 结果 |
| --- | --- |
| owner-jvm-01 | CPU 1 上 OpenJDK 21 版本启动、退出和卸载通过 |
| owner-jvm-threads-01 | 宿主 JRE 不支持 `--release 21`，编译失败，未运行 guest |
| owner-jvm-threads-02 | 同一归档缺失也使 `--release 8` 失败，未运行 guest |
| owner-jvm-threads-03 | 单 JVM 双实例执行通过；任务分配 190/322，总和 1084243968 |
| owner-jvm-threads-baseline-01 | 普通 SMP 对照通过；任务分配 257/255，总和相同 |

首次成功共享运行采用宿主 Java 21 默认目标，普通对照改为显式 source/target
21；两个 class 文件哈希也在证据审计中比较，避免将不同代码误作对照。
`jvm-owner-evidence.json` 汇总并核验冻结产物、payload、日志和写层哈希，保留
失败记录。原有 Linux 线程迁移、权限异常、共享计算和控制台验证同时通过。

## 剩余目标

**Minecraft 客户端跨实例还没有验收完成。** 下一步需要接通和验证图形、输入
以及运行时驱动初始化，再在同一共享域启动实际客户端菜单并完成输入/退出。
本用例也没有覆盖物理多 MCU、混合指针 ABI 的共享 JVM、owner 故障恢复或
普通模式的性能基准。已有其他 ISA 的公共代码验证不能替代这些运行验收。

后续 x86_64 GPU 像素回读及实际键盘鼠标事件已通过，见
[60](60-shared-graphics-input.md)。这些证据仍不等于跨实例 Minecraft 客户端
验收；其他架构可能需要 JVM、本地库和图形栈移植。

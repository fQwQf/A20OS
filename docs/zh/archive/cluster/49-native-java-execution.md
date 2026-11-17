# 真实 Java 程序在 Native 上执行与自动分配

本阶段在真实 A20OS Native 内核上运行完整上游 JamVM 2.0.0、GNU Classpath 0.99 和 Java 程序。普通 `Thread` 创建两个无 affinity 的工作线程，共同领取 32 个任务，实际计算校验和并通过 JNI 查询真实当前 CPU。最终同一组 VM ELF 和 Java 字节码在单核、双核各执行 10 轮；所有轮次结果正确、退出 0、返回 shell，QEMU 自然退出 0。双核每轮均观察到两个 CPU，任务分配为 15/17 或 16/16。没有用主机 Java 执行器替代 Native 运行结果。

## 实现

`tools/build-jamvm-jni.py` 接收已完成的 Native VM 构建和校验和固定的 Classpath/ECJ 源码包，冻结 libc、VM 对象、头文件、依赖、移植源文件和命令。它实际配置并编译 upstream 的 JNI 与 fdlibm C 源码，没有替换 JNI 方法体。旧 fdlibm 不认识 RISC-V 的平台宏，构建器读取目标编译器的真实字节序和 binary64 定义，选择 upstream 的 IEEE 宏；不从构建主机推断目标布局。 VM adapter 与 Classpath 分开使用各自的 config.h，避免配置互相覆盖。

本次构建的模块与实际导出数：

| 模块 | JNI 导出 |
|---|---:|
| javaio | 35 |
| javanio | 70 |
| javalang | 40 |
| javalangreflect | 1 |
| javalangmanagement | 1 |
| javautil | 1 |

这是上述模块的构建与应用所用路径验收，不表示全部方法及 POSIX 后端均已运行通过。javanet、图形、音频等其他 Classpath 原生模块尚未构建。

`user/ports/jamvm/static-jni.c` 提供可选静态模块表。表由实际对象文件的已定义 JNI 符号生成，使用 upstream 头文件中的真实函数原型，再转换为统一函数指针存储。库名只匹配完整的固定 bootstrap 路径，不把任意同名文件当成已存在的库；方法只在所属模块中查找。JamVM 保留原有的类加载器归属检查、JNI 解析与 libffi 调用，模块随可执行文件常驻。

模块共用同一个 libc、TLS、文件描述符和 Native pthread 实现，没有加载第二份 libc。JCL 使用 upstream 明确支持的 `JCL_NO_JNIONLOAD` 懒初始化模式。普通动态库调用继续进入原来的 dlopen 路径；本阶段没有实现通用静态可执行文件的动态符号导出或宣称任意 DSO 都可用。

静态表及接口没有 MCU/STM32 条件分支。当前完整 VM/类库构建仍只支持 Native RV64；公共线程与调度机制已在其他架构验证，不等于其他架构的完整 JVM 已完成移植。默认内核、rootfs、普通接口和调度热路径没有修改；只有显式构建带 `A20_STATIC_JNI` 的可选 VM 才包含该模块表。

## 程序与运行器

`user/tests/java/A20NativeWorkload.java` 通过普通 Java Thread、monitor、 wait/notifyAll、join 和 yield 协作；32 个任务逐项计算 FNV hash 和整数和，检查没有遗漏或重复。两个线程都必须实际完成任务。Java 文件 API 写入、读取并删除同一个文件，还检查不存在的 JNI 库必须抛 UnsatisfiedLinkError。

可选验收模块 `a20execution` 仅含 `test_jamvm_execution.c` 的 CPU 查询：直接调用公共 `a20_thread_get_cpu`，失败时抛异常。它不分配任务、不设 affinity、不报告猜测的 CPU，也不计算 Java 的结果。应用的实际线程分配由已有 Native pthread 和公共调度器完成。

`check-jamvm-native.py --execute-java` 在启动前核对并冻结 VM、JNI 探针、类库、应用、内核和 FAT 镜像。每轮必须有正确计算输出、32 个任务、两个非空线程计数、符合单核/双核配置的真实 CPU mask、退出码 0 和新 shell 提示符；最后要求 QEMU 自然退出 0，才设置 java_application_verified=true。默认启动器探针模式仍不标记 Java 应用通过，GC/Minecraft 字段仍为 false。

## 最终证据

| 项目 | 结果 |
|---|---|
| 双核 Java 10 轮 | 每轮 jobs=32、sum=4359168、mask=3，workers=15/17 或 16/16 |
| 单核同一程序 10 轮 | 每轮 jobs=32、sum=4359168、mask=1，两个线程都完成任务 |
| 文件读写/删除与缺失 JNI 库 | 包含在每轮实际 Java 执行中，均通过 |
| 原有启动器和 JNI 调用适配探针 | 单核/双核各 40 次，通过 |
| 程序与模拟器退出 | 各轮程序退出 0，返回 shell；两次 QEMU 均自然退出 0 |

最终 VM SHA256：`b69d35e0a671705764b8e2f1234fa517c23cf3a8cf5761677882264bc67a04e9`。构建目录为 `.kernel-build/cluster/java-runtime/jamvm-jni-05`；运行报告为`.kernel-build/cluster/classpath-jni-final-{single,dual}/report.json`。独立证据归档包含压缩构建、运行输入、逐文件 SHA 和提交源码快照。归档后的重复 FAT 镜像可移除，复查时从对应 tar.gz 恢复。

早期缺少 configure 解码工具、fdlibm 字节序宏、VM config.h 隔离的失败日志保留；01/02/03 的非活动源码和 libc 输入无损压缩，解压后 SHA 与原清单一致。04 是首次程序通过的中间版本；正式结果使用格式及文件异常清理更新后的 05。没有覆盖失败日志或将它们改成成功。

## 复现

在独立 worktree 根目录，已有 full libc、Native VM 基线、真实类库及源码包时执行；build-triplet 必须填写实际构建主机的值，输出目录必须为空：

```sh
python tools/build-jamvm-jni.py \
  --native-build .kernel-build/cluster/java-runtime/jamvm-native-09 \
  --archives .kernel-build/cluster/java-runtime/upstream \
  --classlib .kernel-build/cluster/java-runtime/classlib-final \
  --build-triplet x86_64-pc-linux-gnu \
  --output .kernel-build/cluster/java-runtime/jamvm-jni-new
python tools/check-jamvm-native.py \
  --build .kernel-build/cluster/java-runtime/jamvm-jni-new \
  --classlib .kernel-build/cluster/java-runtime/classlib-final \
  --kernel .kernel-build/cluster/evidence/thread-signals-3c72d761/runtime/thread-signals-wide-dual/kernel.elf \
  --image .kernel-build/cluster/evidence/thread-signals-3c72d761/runtime/thread-signals-wide-dual/fat32.img \
  --output .kernel-build/cluster/classpath-jni-new-dual \
  --cpus 2 --cycles 10 --execute-java
```

单核使用匹配的 wide-single 内核/镜像和 `--cpus 1`。运行器不下载类库、不重新编译运行中的 ELF，也不把超时或主机终止当作通过。

## 下一步

本阶段尚未验收的强制 GC、并发分配、根保存及对象移动，现已继续实现并通过真实执行，见 [50-native-java-gc.md](50-native-java-gc.md)。本页原有证据仍对应 GC 之前的构建。线程资源完整回收、JNI 外部线程附加栈查询、32 位 MCU/其他 ISA 的 full libc/JVM、图形/输入和真实 Minecraft Java 客户端仍待完成。物理多 MCU 的共享 RAM/硬件仲裁和同一原生内核执行仍需实物验收。

完整 Native Java 运行时已继续接入 AArch64，并通过同字节码的单核/双核计算与 GC 压缩，RV64 回归同时通过；实际次数、JNI 修复和剩余范围见[51-native-java-portable.md](51-native-java-portable.md)。物理多 MCU 和 Minecraft Java 客户端仍待实现与验收。

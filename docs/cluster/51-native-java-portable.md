# 同一 Native Java 程序的 RV64 / AArch64 执行

本阶段把真实完整 libc、JamVM、GNU Classpath JNI 和上一阶段的 GC 接到
AArch64 Native。两种 ISA 使用相同的 Native 线程、文件、同步接口和 VM
原子/GC 代码。真实 Java 程序的六个 `.class` 文件逐字节一致；各自静态链接
的 Native ELF 按 ISA 构建。没有 Linux 客体或替代 Java 解释器。

## 实际执行结果

所有通过项都要求程序返回 0、计算/资源检查通过、QEMU 自然退出 0；观察到
启动、日志或 CPU 活动均不算通过。输出目录位于 `.kernel-build/cluster/`：

| 运行目录 | CPU | 重复次数 | 结果 |
|---|---:|---:|---|
| `native-aarch64-threads-01` | 2 | 5 | 原有真实 C pthread 程序通过 |
| `native-aarch64-threads-single-01` | 1 | 1 | 同一 C 程序通过 |
| `native-java-aarch64-03` | 2 | 5 | JNI、Java 计算、GC 压缩均通过 |
| `native-java-aarch64-single-02` | 1 | 1 | 同一 VM 和 Java 计算/GC 压缩通过 |
| `native-portable-rv64-02` | 2 | 5 | 公共 JNI 修复后 Java/GC 回归通过 |
| `native-portable-rv64-single-01` | 1 | 5 | 同一 RV64 VM 单核回归通过 |

Java 计算程序的两个工作线程竞争领取 32 项任务，核对无遗漏/重复、每项
结果及总和 4359168，并实际读写 Native 文件。应用没有 CPU 绑定或预分配
每线程份额。两种架构的双核运行 CPU mask 均为 3，单核均为 1；双核的
线程完成数量包括 15/17、16/16、17/15。这证明实际工作由线程共同领取且
调度到两个 CPU，不代表已经测得 CPU 利用率均衡或跨物理芯片执行。

每次 GC 程序使用 4 MiB 堆，累计分配 64 MiB，完成 48 次真实回收，验证
128 个私有图节点、16 个弱引用回收、实际对象移动和 Java 线程
sleep/interrupt/status/join。AArch64 双核累计对象移动 6469–6492 次，
单核 6472 次；RV64 双核为 9191–9192 次。计数包含一个对象多次移动，
不是不同对象数量。VM 启动探针另有每轮 11 项实际 JNI 调用检查。

AArch64 首次单核运行使用每程序 180 秒截止时间，Java 阶段超时，已保留
`native-java-aarch64-single-01` 为失败。独立 C 程序通过、类加载跟踪仍在
推进后，用相同 VM/字节码重新运行，每程序允许 900 秒，完成一轮计算和
GC，自然退出。成功运行开放本机 GDB 端口，只读取状态；没有写寄存器、
内存或改变程序判断。这个结果不构成启动耗时保证，也不覆盖五轮单核重复。

## 跨架构 JNI 修复

原有 generic JNI stub 把引用和 `int` 参数压缩为相同描述，libffi 又将它们
作为指针宽度处理。AArch64 的负 `jint` 返回值在 VM 栈中成为零扩展值，
直接 JNI 探针得到 `0x00000000ffed2979`，而预期为符号扩展后的
`0xffffffffffed2979`。

只把 libffi 的 `I` 改成 32 位是不完整的修复：压缩描述里的对象引用也会
被截成 32 位，真实 Java 在 AArch64、RV64 上均出现 SIGSEGV。最终
`user/ports/jamvm/jni.patch` 在 Native 构建中保存完整方法描述，每次 JNI
调用使用声明的参数宽度，引用仍为指针，`jint` 返回显式符号扩展。
已有 JNI 局部引用和返回引用处理保持。两种架构均使用这个实现。

该修复仅在 `__a20__` 下启用，其他上游端口保留原 stub 路径。可选 JVM
的 JNI 调用因此经过 libffi；本阶段优先正确执行，没有声称其开销不变。
内核 syscall、调度器、mlibc sysdeps、默认 rootfs/Make 配方没有更改，
普通程序不经过这个可选 VM 路径。

初版整数返回失败、部分修复导致的两种 ISA Java 崩溃、第一次 libc 配置
误选主机 linker，以及单核超时均保留原日志/报告；没有用后续通过覆盖。

## 构建与重放

新增 `tools/build-mlibc-native.py` 在空目录冻结完整 mlibc 源码，使用目标
cross file、Meson/Ninja 构建独立静态 sysroot，不下载、不安装到系统。
AArch64 使用 `user/external/mlibc/ci/a20-aarch64.cross-file`。构建器记录
源码哈希、编译器命令/版本/入口哈希、完整命令和 sysroot 哈希；构建成功
本身不代表运行验收。需要已安装目标 C/C++ 工具链以及源码自带依赖。

Native VM 构建器的 `--arch` 选择工具链、libffi 目标和 ELF 链接参数；
默认仍为 `riscv64`。JNI/GC 构建器从冻结基础元数据继承 ISA。检查器验证
元数据和 ELF machine 一致、静态 Native 装载信息，再选择对应 QEMU。
AArch64 pthread 检查器要求显式提供目标 ELF，不调用原 RV64 Make 配方
伪造可移植构建。

以下命令从已有校验归档和完整类库构建 AArch64 运行时。`$archives`
指包含 JamVM 2.0.0、zlib 1.3.1、libffi 3.4.6、Classpath 0.99 和 ECJ
4.6.1 的本地目录；各文件 SHA 与 [47](47-jamvm-native-bootstrap.md)、
[49](49-native-java-execution.md) 相同。所有输出必须为空目录。

```sh
python3 tools/build-mlibc-native.py --arch aarch64 \
  --output .kernel-build/cluster/java-runtime/mlibc-arm-new
python3 tools/build-jamvm-native.py --arch aarch64 \
  --archives "$archives" \
  --sysroot .kernel-build/cluster/java-runtime/mlibc-arm-new/sysroot \
  --output .kernel-build/cluster/java-runtime/jamvm-arm-new
python3 tools/build-jamvm-jni.py \
  --native-build .kernel-build/cluster/java-runtime/jamvm-arm-new \
  --archives "$archives" --build-triplet x86_64-pc-linux-gnu \
  --classlib .kernel-build/cluster/java-runtime/classlib-final \
  --output .kernel-build/cluster/java-runtime/jamvm-jni-arm-new
python3 tools/check-jamvm-native.py --arch aarch64 \
  --build .kernel-build/cluster/java-runtime/jamvm-jni-arm-new \
  --classlib .kernel-build/cluster/java-runtime/classlib-final \
  --kernel "$kernel" --image "$image" \
  --output .kernel-build/cluster/native-java-arm-new \
  --cpus 2 --cycles 5 --timeout 900 --execute-java --execute-gc --gc-compact
```

`$kernel`/`$image` 必须是匹配的 AArch64 Native、双核、用户抢占和线程
通知配置，参照 [48](48-native-thread-signals.md)；不能借用 RV64 镜像。
在其他构建主机替换 `--build-triplet`；可用 `--cc/--cxx` 指定已有编译器。
本次仅缺少 AArch64 C++ 前端，提取本地 Debian 包并使用显式交叉工具链
搜索路径，没有安装/替换系统编译器。包、wrapper 和 SHA 随证据保存。

RV64 使用 `tools/build-jamvm-gc.py` 重编全部 46 个核心编译单元，复用
经哈希验证的既有 JNI/依赖。当前 `arch.h` 用于整批核心重编，既有
`config.h` 必须完全匹配；补丁已存在时执行精确 reverse dry-run 验证。

本阶段独立证据以当前提交短 SHA 命名：
`.kernel-build/cluster/evidence/native-portable-<commit>/manifest.json`，
包括完整 AArch64 libc/VM/JNI 构建、RV64 增量构建与独立基础归档引用、
通过和失败运行、源码快照、逐文件清单、归档校验、字节码相等证明及门禁
输出。运行归档含静态 ELF、完整类库和冻结内核/镜像，可独立重放；重新
构建仍需要相应主机工具链和恢复冻结依赖路径。

## 尚未达到的目标

上述是分别启动的两种 ISA 内核执行同一个 Java 程序，不是混合 ISA 线程
共享一套堆。完整 VM 目前只接入 RV64/AArch64，x86、RV32/ARM32 MCU、
物理共享 RAM/硬件仲裁上的用户态原子契约仍需实现和验收。公共 Native
接口的已有多架构执行结果不能替代这些完整运行时验证。

长 JNI 阻塞的 GC 转换、外部 JNI 附加线程栈边界、线程资源完整回收、
图形/输入和 Minecraft 客户端依赖仍需继续。最终目标保持
[24](24-single-system-minecraft.md)：至少两块物理 MCU 的同一原生
A20OS 上，真实 Minecraft Java 客户端显示主菜单并响应输入。

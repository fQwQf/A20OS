# 真实 Java VM 的 Native 启动与文件映射修复

本阶段直接移植 JamVM 2.0.0 的完整上游解释器和 JNI 调用层，构建真实
GNU Classpath 0.99，不替换 Java 语义、类库或 Minecraft。普通内核、默认
rootfs、Native syscall 和既有 MCU 构建没有改动。当前 full mlibc 端口为
RV64，因此这项运行证据限于 RV64 QEMU；其他架构及物理 MCU 尚未验收。

[JamVM 官方说明](https://jamvm.sourceforge.net/)明确要求搭配类库才能组成
完整 Java 运行环境；GNU Classpath 配置主要对应 Java 5，不能据此声称支持
现代 Minecraft。原有 [Minecraft/物理集群验收要求](24-single-system-minecraft.md)
继续有效；版本输出、类文件数量、JNI 测试和 QEMU 启动均不替代该验收。

## 实现

- `user/ports/jamvm/arch.h`：RV64 C 原子、内存屏障和 Java 浮点位表示。
  使用 libffi 的真实 RV64 ABI，不加入 Linux 汇编 syscall；当前明确拒绝
  其他架构，后续必须随各平台 full libc、TLS 和原子语义一起扩展。
- `user/ports/jamvm/os.c`：使用公共 Native execution-domain 查询在线 CPU。
  VM 的普通命令行启动沿用上游提供的外层栈帧。JNI Invocation/外部线程
  附加还缺少可靠栈边界查询，当前明确失败，不能用猜测边界进行 GC。
- `tools/build-jamvm-native.py`：校验源码档案，冻结 libc、SDK、端口代码、
  链接脚本、测试源和构建驱动；编译真实 zlib/libffi、46 个 VM 编译单元、
  静态 Native ELF 与 JNI ABI 探针。解释器采用完整 switch 实现，不开启
  内联解释器等性能优化。源码列表取自上游 Makefile，未裁掉类加载和 GC。
- `tools/build-jamvm-classlib.py`：用固定 ECJ 编译完整 GNU Classpath 类库和
  JamVM 配套类，保留资源；检查每个 class 的魔数和 Java 5 版本。这个
  命令只构建字节码和资源，尚未构建 Classpath 的 Native JNI 库。
- `user/ports/classpath/compiler-checks.patch`：修正禁用命令行工具时遗漏
  Java 编译器检查的条件，同时保留真实编译器/宿主 Java 检查。显式选择
  glibj.zip 构建；生成的 base64 解码辅助工具实际解码上游测试 class，
  由宿主 Java 执行，不伪造 configure 检查结果。

JamVM 使用 GPL-2.0-or-later；GNU Classpath 保留其源码中的链接例外。
构建输入保存原始源码档案及许可证，端口以单独文件和补丁保存。

## 从实际启动发现的文件映射问题

VM 最初能够输出版本、帮助并正常退出，但真实类库仍报
`NoClassDefFoundError: jamvm/java/lang/VMClassLoaderData`。配套档案中确有
这个 class。进一步运行 Native 文件映射程序，打开同一个 ZIP、fstat 和
pread 均成功，文件 mmap 却实际返回 `EINVAL`，程序退出码为 4。

原因是 mlibc 的 `a20_vm_map_args` 仍为 56 字节，遗漏当前公共 ABI 的
`vmar` 字段；内核按 64 字节校验并拒绝旧结构。现补齐字段，并在普通映射
中显式传 `A20_HANDLE_NULL`。该句柄值为全一，不能用零初始化代替。
内核与 SDK 的 ABI 不变，匿名映射路径不变。

修复后同一文件映射源程序检查真实 ZIP 内容、关闭 fd 后映射仍有效、正常
unmap，退出码为 0。定向回归直接编译生产 sysdep，对照 SDK 的大小和字段
偏移，覆盖普通/匿名映射、错误映射和无效 fd；把空 VMAR 改为 0 的负例失败。

## 可复现命令

准备已有 Native RV64 full mlibc sysroot、RISC-V ELF 工具链、Python、make、
patch、mtools、QEMU、Java、zip，以及固定的源码/ECJ 档案。构建程序不下载
文件，不接受校验不匹配的档案。目录必须为空，失败证据不能覆盖重跑。

```sh
python tools/build-jamvm-native.py \
  --archives .kernel-build/cluster/java-runtime/upstream \
  --sysroot .kernel-build/cluster/java-runtime/sysroot-rv64 \
  --output .kernel-build/jamvm-native

python tools/build-jamvm-classlib.py \
  --archives .kernel-build/cluster/java-runtime/upstream \
  --native-build .kernel-build/jamvm-native \
  --build-triplet x86_64-pc-linux-gnu \
  --output .kernel-build/jamvm-classlib

python tools/check-jamvm-native.py \
  --build .kernel-build/jamvm-native \
  --kernel /absolute/frozen/kernel.elf --image /absolute/frozen/fat32.img \
  --cpus 2 --cycles 10 --diagnose-bootstrap \
  --output .kernel-build/jamvm-launch

# 当前应失败，并保留真实类库加载及缺失 sysdep 的日志。
python tools/check-jamvm-native.py \
  --build .kernel-build/jamvm-native --classlib .kernel-build/jamvm-classlib \
  --kernel /absolute/frozen/kernel.elf --image /absolute/frozen/fat32.img \
  --cpus 2 --cycles 1 --diagnose-bootstrap \
  --output .kernel-build/jamvm-bootstrap

python tools/tests/test_mlibc_filemap.py
```

`--build-triplet` 必须是实际构建主机的 triplet，示例对应本次 Linux x86_64
主机。冻结内核和镜像须匹配指定 CPU 配置；当前可复用上一阶段的双核/单核
验收镜像。运行器验证 ELF 的 RV64 双精度 ABI、Native PHDR 和静态启动，
核对构建产物 SHA 后才冻结并启动 QEMU。

## 已有证据与边界

| 项目 | 实际结果 | 范围 |
|---|---|---|
| VM 与依赖构建 | 46 个 VM 编译单元，真实 zlib/libffi 链接完成 | 编译不等于 Java 启动 |
| JNI 调用适配器 | 每轮实际执行 11 个 C 回调；静态/实例、整数、long、浮点、对象、void | 直接链接 VM 同一个 `dll_ffi` 对象，不是 Java 应用 |
| 启动器/JNI 重复执行 | 单核、双核各 10 轮、每轮 4 个探针，数值退出码 0 并返回 shell | 不证明 JVM GC 可用 |
| 完整字节码类库 | GNU Classpath 7,249 个 class，VM 配套 18 个 class，全部版本 49 | 不证明所有 API 可用 |
| 真实 ZIP mmap | 修复前 `EINVAL`/退出 4；修复后内容、fd 生命周期、unmap 检查通过/退出 0 | 同一内核和同一 ZIP，源程序相同 |
| 新 libc 的真实 pthread 回归 | 同一程序在双核/单核各 10 轮正常计算、join、退出；双核 mask 3，单核 mask 1 | 沿用无 affinity 的实际工作测试 |
| 带类库的 VM 初始化 | 已实际加载 Object、Thread 等类并创建辅助线程；缺失 `Sigtimedwait`/`Tgkill` 后失败 | 明确不是 Java 应用通过 |

关键最终 VM ELF SHA-256：
`d955ddd9ffc3fd8f167b40e2cf6ef660cd65fa0e8b57ede433ab8239c6703321`。
类库 SHA-256 与全部冻结输入见构建及运行 report；早期配置失败、错误参数
诊断和中间版本只作定位证据，最终结果以归档 manifest 指定的路径为准。

下一步必须实现真实 Native 线程身份、定向信号/等待、正确的每线程 mask
及 GC 暂停协调，再补 Classpath Native 库和动态装载。不能将缺失函数改成
成功返回、关闭 GC 或忽略线程暂停来宣称 Java 已启动。pthread 资源回收、
JNI 附加栈查询、跨架构 full libc/VM 端口、图形/输入和真实 Minecraft 客户端
仍需继续实现；物理多 MCU 的共享 RAM/仲裁和同一内核验收仍未完成。

后续已实现的 Native 线程身份、定向通知与等待，以及初始化推进到原生类库
装载的实际结果见 [48](48-native-thread-signals.md)。上述本阶段失败日志保留原样。

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

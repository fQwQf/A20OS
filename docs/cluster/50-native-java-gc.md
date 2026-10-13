# 原生 Java 的并发 GC、根保存和堆压缩

真实 Java 程序现在可以在 A20OS Native 上一边运行两个工作线程，一边执行
上游 JamVM 的垃圾回收。固定 4 MiB Java 堆，单次程序累计分配 64 MiB，
实际执行 48 次 GC，随后 join 并正常退出。双核、单核和双核强制压缩各重复
5 轮；强制压缩实际移动对象，两个工作线程私有对象图的内容和连接仍正确。
这补齐了 Java 应用继续运行时的内存回收路径。自动负载分配继续使用已有
公共 Native pthread/调度器，同一 VM 和字节码没有 CPU affinity。

## 修复了什么

旧 VM 在普通计算测试通过后，遇到 `System.gc()` 会等待纯 Java 工作线程
停止。Native libc 在检查点交付线程信号，纯字节码计算不进入 libc，因此
单靠 SIGUSR1 不能完成 stop-the-world。旧 VM 的负面对照实际停在第一次
回收，日志和超时结果保留，没有把主机终止当作程序退出。

`user/ports/jamvm/gc.patch` 为 Native 的未缓存 switch 解释器加入完成字节码
边界上的暂停检查。请求者发布暂停标志；工作线程到达检查点后调用上游
`suspendLoop`，先通过 `sigsetjmp` 保存 C 寄存器和栈顶，再发布已暂停状态。
收集器观察到已暂停或上游已有的 blocking 状态后才扫描。恢复清除暂停标志，
工作线程离开等待；没有关闭 GC、跳过根扫描或替换收集算法。

Native 的暂停请求和状态使用 C 原子字段。内部暂停/恢复不再排队 SIGUSR1，
防止延迟交付的处理器覆盖收集器正在扫描的栈顶。普通 Java Thread.interrupt
仍保留原有 SIGUSR1 通知；处理器只在确有活动暂停请求时进入暂停路径，
保留 VM 阻塞区先前发布的根。其他 OS 保持 upstream 的信号暂停实现。
当前 Native 配置显式拒绝尚未验证的 threaded/cached 解释器构建。

实际回收验证还发现 Thread.interrupt 在初始检查之后、等待队列发布之前
到达时，睡眠可能丢失唤醒。补丁在等待队列发布后重新检查中断标志，并为
跨线程读取的中断状态、等待位置及 monitor owner 使用原子字段。中断者看到
已发布队列，或者等待者看到已发布中断；已有 monitor 锁协议继续负责等待。
保留失败版本的日志，修复后的普通 Thread.sleep/interrupt/join 在每轮通过。

补丁只以 `__a20__` 区分 Native，没有 STM32、MCU 或具体 ISA 分支。默认
内核、rootfs、公共 ABI 和调度热路径没有改动。解释器检查会增加这个可选
Native VM 的字节码执行开销；本阶段追求功能正确，没有性能提升承诺。

## 实际程序和验收条件

`user/tests/java/A20NativeGC.java` 使用真实 upstream 类库，编译为 major 49
字节码。两个工作线程各创建 64 个节点的私有循环图，每个节点包含 32 个
可校验整数；根保留在工作线程 Java 栈上。工作循环只执行 Java 字节码，
不靠 JNI、分配、sleep、yield 或 monitor 进入暂停点，持续校验数据与连接。

主线程进行 16 轮分配压力，每轮两次显式 GC 和 128 次 32 KiB 分配。额外
的堆满回收由真实 VM 触发。每轮等待两个工作线程继续取得进展；结束后
校验弱引用回收、停止并 join 两线程，然后验证睡眠中断和中断状态清除。
日志中的 PASS 不是唯一条件：运行器同时核对实际 GC 记录、回收对象数、
压缩模式下的移动对象数、数值退出码、返回 shell 和 QEMU 自然退出 0。

| 最终运行 | 普通 Java 工作 | GC 程序 | 实际结果 |
|---|---|---|---|
| 单核，5 轮 | 每轮 32 jobs、sum=4359168、mask=1 | 每轮 48 次 GC | 每轮 16 个弱引用被清除，128 节点对象图完整 |
| 双核，5 轮 | 同样结果，mask=3 | 每轮 48 次 GC | 每轮 16 个弱引用被清除，两工作线程持续进展 |
| 双核强制压缩，5 轮 | 同样结果，mask=3 | 每轮 48 次 GC | 每轮移动累计 9166–9195 个对象，图及数组校验通过 |

移动数是一次程序中各次回收的累计计数，同一对象可以移动多次；不是不同
存活对象的数量。每次 GC 程序分配 67108864 字节，堆上限 4194304 字节。
三组运行各包含 20 个原有启动器/JNI 探针，全部通过。15 次普通 Java 程序、
15 次 GC 程序均退出 0，三次 QEMU 均自然退出，host_terminated=false。

最终 VM SHA256：
`bc9f70f876e90ddab8987a88c2d75d942458fcc572db843777dfb80a1cc8c31d`。
Java workload.jar SHA256：
`13442ff9a453e2399a2be494aaa4c913524e7a9a28e3a5128b7e902f16636a1e`。
最终运行器 SHA256：
`466137e0e71d6aa11370a35706d60aac1f9432e766d63179e44ecd6f3ca35b1c`。

构建目录为 `.kernel-build/cluster/java-runtime/jamvm-gc-03`，正式运行报告为
`.kernel-build/cluster/native-gc-verified-{single,dual}/report.json` 与
`.kernel-build/cluster/native-gc-verified-compact-02/report.json`。
证据归档保留完整 ELF、字节码、类库、内核、镜像、日志及逐文件 SHA；缓存
中已归档的重复大文件可以移除，从 storage.json 指定的 tar.gz 恢复。

旧 VM 首次 GC 停住、修复中间版本的普通中断失败、运行器将 GC 次数误用为
shell 提示符次数的记账失败均单独保留。首次压缩验证因磁盘满在启动前失败，
没有启动 QEMU；成功压缩结果使用新的空输出目录和同一份最终 VM。失败结果
没有合并进正式通过统计。

## 构建与复现

新建 Native VM 的标准 `tools/build-jamvm-native.py` 已直接应用 gc.patch。
`tools/build-jamvm-jni.py --classlib ...` 同时编译普通 Java 和 GC 验收程序。
构建完成仅设置 gc_workload_built，不会宣称已经通过运行。

已有 [49](49-native-java-execution.md) 的完整、冻结 JNI 构建时，可以只重编
所有 46 个 VM C 编译单元，复用经 SHA 校验的 libc、依赖和 80 个 JNI 对象：

```sh
python tools/build-jamvm-gc.py \
  --jni-build .kernel-build/cluster/java-runtime/jamvm-jni-05 \
  --output .kernel-build/cluster/java-runtime/jamvm-gc-new
python tools/check-jamvm-native.py \
  --build .kernel-build/cluster/java-runtime/jamvm-gc-new \
  --classlib .kernel-build/cluster/java-runtime/classlib-final \
  --kernel .kernel-build/cluster/evidence/thread-signals-3c72d761/runtime/thread-signals-wide-dual/kernel.elf \
  --image .kernel-build/cluster/evidence/thread-signals-3c72d761/runtime/thread-signals-wide-dual/fat32.img \
  --output .kernel-build/cluster/native-gc-new-dual \
  --cpus 2 --cycles 5 --execute-java --execute-gc
```

强制压缩使用另一个空输出目录并增加 `--gc-compact`。单核使用匹配的
wide-single 内核/镜像和 `--cpus 1`。每条测试的默认超时为 180 秒；超时不算
通过，也不会覆盖失败目录。实际执行在 Native 内核中，主机 Java 只用于 ECJ
编译。没有 Linux 客体 JVM。

增量构建器在编译前后核对冻结基础输入，并记录全部复用路径和哈希；基础
构建必须保持不可变且可访问。最终构建证据引用上一阶段的独立
`evidence/native-java-ecb354f1/build.tar.gz` 和文件清单，不能只保存新对象后
丢弃基础库。运行归档则已经包含完整静态程序，重放运行不需要重新链接。
已应用同一补丁的新基础源码会经过 reverse dry-run 验证，避免重复打补丁。

## 还需要实现

本阶段完整 VM 运行仍只验证了 RV64 Native。公共接口没有 ISA 分叉，但不能
由此宣称 ARM/RV32/x86 完整 JVM 已通过，更不能把 QEMU 双核当作物理多 MCU。
通用长时间 JNI C 代码的暂停/阻塞转换、JNI 外部附加线程的栈边界、线程资源
完整回收及 32 位 MCU full libc/JVM 仍需推进。当前验收覆盖纯 Java 和 VM
已有的受管理等待路径，并没有验证任意 JNI 方法都能及时参加 GC。

物理 MCU 的用户态原子指令还必须与共享 RAM/硬件仲裁的一致性契约匹配，
不能从内核共享原子后端推导用户 LDREX/SC 已经跨芯片有效。图形、输入和所选
Minecraft Java 客户端依赖也未完成。最终标准继续使用
[24](24-single-system-minecraft.md)：至少两块物理 MCU 上的同一原生 A20OS，
实际客户端显示主菜单并响应真实输入。

# 公共 Native 用户堆与 flat 内存生命周期

## 范围与接口

统一机制按执行域和内存模型划分，不按 STM32、树莓派或服务器划分。应用调用同一 `user/liba20rt/a20_mem.h`，Native 0x0300 `vm_alloc` 和 0x0301 `vm_unmap` 由 `kernel/abi/native/sys_mem_basic.c` 派发。既有 MMU 实现继续调用 proc_mmap/proc_munmap；flat 实现使用`kernel/mm/flat_user.c`，没有 ARM、STM32、fabric 寄存器或总线依赖。共享 RAM 作为内核堆时分配结果自然属于该共享执行域；本模块不创建跨板内存。

| 契约 | 公共语义 | flat 当前限制 |
|---|---|---|
| 分配 | 请求字节数，返回 4096 字节对齐地址；新内存清零 | 长度向上取整；hint=0、prot=3（RW）、flags=0 或 0x20 |
| 释放 | 地址及长度标识该分配 | 只支持整个向上取整后的分配，不支持子区间 |
| 所有权 | 空间登记固定镜像/栈和动态块 | 固定块由装载器拥有，不能 vm_unmap |
| 内核访问 | 在映射锁内检查完整范围并复制/原子操作 | 跨块访问即使相邻也拒绝 |
| 等待引用 | 动态 futex 字在整个等待期间固定 | 固定期间 unmap/destroy 返回 BUSY |
| 退出 | 最后任务引用释放后销毁空间和残留动态块 | 空间对象须晚于所有调用者与等待引用销毁 |

flat 的页大小是接口分配粒度，不表示存在页表、MPU 隔离或需求分页。动态块采用内核 kcalloc/kfree，保留原始指针并返回对齐后的地址。分配节点和内存申请在空间锁外进行，入表时重新检查 closed 和重叠；释放先在锁内摘除，随后在锁外归还，避免空间锁与内核堆锁的逆序。分配失败不改变输出地址；结果结构复制失败时回滚新分配。重复登记同一固定节点也会拒绝。

USER_HEAP=64 公布基本匿名堆能力，已知执行位掩码扩为 127；结构仍为 64 B、 version=1。旧 agent 拒绝未知能力位，发布双方应同步更新公共头和 codec。这一位不表示支持 mprotect、VMAR、VMO、完整 libc 或 JVM。 32 位 SDK 先拒绝宽地址/宽 unmap 长度，不能把高位截断后释放另一个有效块； vm_alloc 参数结构保留 u64 长度，由内核检查是否能用本机 size_t 表示。页数辅助函数先拒绝乘法溢出，再转换为字节数。

## futex 与原子操作的生命周期

仅在进入 syscall 时检查地址不足以保证等待安全：任务阻塞后，另一个 CPU 可能释放该分配并重新使用地址，导致旧等待键错误地引用新对象。公共 futex 入口在有效 flat 字上增加 pin，之后才执行比较、入队、Park/Wake、超时/中断处理和出队，最后解除 pin。所有返回路径都经过同一包装函数。持有 pin 时，释放和销毁返回 BUSY，不会强制复用地址或中断等待。

copy_to/from_user 和 atomic32 在同一空间锁内复查范围并操作；分配不能在这段操作中被摘除。atomic32 再通过既有平台原子 provider 完成操作，一致性服务器使用本地 CPU 原子，共享 fabric 使用硬件 gate。普通用户直接访问 flat 地址仍属于受信任程序模型；所有权登记不提供硬件隔离。其他异步内核访问也必须持有适当引用，不能保存一个裸地址后解除空间锁。

ARMv7-M 装载器负责初始化空间、登记镜像与栈、绑定 task.flat_user。公共模块没有复制 ELF 装载器和异常入口。以后 RV32/其他 NOMMU 端口应遵循同一绑定与退出约定；交叉编译通过不能代替这一步板级集成。 CONFIG_FLAT_USER_MEMORY 为内部内存模型选择器：完整 NOMMU 构建和启用 Native 程序的 MCU 构建选择它；默认精简 MCU 构建不增加 task 字段或代码。

## 可复现验证

```sh
make check-flat-user
make check-execution
make check-mcu-program
make check-mcu-program-rtl
make check-native-sync
```

check-flat-user 用生产 flat_user.c，宿主适配器只提供锁、页常量和分配器依赖。 x86-64、AArch64、RV64 执行同一范围/回滚/pin/销毁测试，包含八线程各 200 次分配与释放；x86 另运行 ASan/UBSan。测试分配器统计最终未释放块为零。若宿主 ptrace 包装阻止 LeakSanitizer，应显式使用 ASAN_OPTIONS=detect_leaks=0，报告该限制；不能将其写成 LeakSanitizer 已通过。工具不声称是内核开机证据。

双 MCU 测试运行实际 Native ARM ELF/SVC，覆盖 20 项堆检查：完整两页清零、尾部原子操作、跨界拒绝、部分/零长释放、非法长度/权限/标志/hint/结构大小、 OOM 后恢复、动态字超时后释放、释放后访问拒绝、32 位 unmap 高位和页数溢出。另一个 CPU 观察任务实际 BLOCKED/off_cpu 后，验证等待中的 unmap/destroy 均返回 BUSY，然后通过公共 atomic32/wake 唤醒；任务继续在另一 CPU 执行。故意遗留的堆块由退出路径回收，仍检查已有镜像、栈、PID 和任务引用回收。成功标记为 `[PROGRAM-HEAP] PASS checks=20 wait-pinned=1 cross-cpu=1`。 RTL 联合仿真还经过共享 RAM 与硬件 gate；没有物理板测试结论。

## 本阶段证据的边界

2026-10-12 已将候选源码完整写回独立 worktree，并重新执行实际整树构建：`.kernel-build/cluster/native-memory-sync/report.json` 记录 RV64、AArch64、x86 双核 QEMU 上的 SDK 分配器、atomic32 和 futex 测试。该报告使用正常 dev-build，已覆盖最小 libc 与 SDK 联合构建；本轮补齐了其 stdint.h 缺少的标准上限宏。之前 `/tmp` 中的模块重链接报告只作为开发记录，不能替代此次整树证据。公共宿主验证在 `flat-user/` 与 `execution/`，MCU 原生应用及 RTL 联合报告在`shared-program/report-rtl.json`。RV32 用户测试仅编译链接，不声明用户态已启动。

默认 MCU kernel.bin 重建后的 SHA256 与前一提交一致：`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。这证明该精简配置的构建产物未改变。其他既有功能以对应回归场景为证据，不将此哈希扩展成所有平台或全量性能测试结论。

动态堆和公共同步为运行时提供基础；用户线程创建、其他 MCU 的板级入口、文件/显示/输入、Java 运行时及 Minecraft Java 客户端仍须继续实现与验证。异构平台可以使用同一作业/服务协议，但直接迁移原生线程仍要求 ISA/ABI、地址布局相容且有真实共享内存。功能优先，不设首轮性能门槛。

## SDK 分配器的共同路径

`a20_malloc.c` 没有板型分支，直接使用同一 vm_alloc。分配头长度向上取整到 16 B，指针通过 uintptr_t/字节指针运算，修复原实现中 32 位指针转换警告与 64 位返回地址只有 8 字节对齐的问题。malloc/realloc 拒绝无法表示或对齐会溢出的大小；calloc 在乘法前通过内建溢出检测拒绝环绕。realloc 失败保留旧块及其内容，成功扩容复制旧内容；free 标记空闲供复用，映射随任务退出回收。该小型分配器仍要求调用者串行化，不宣称已经支持并发 malloc。

公共测试头 `user/tests/native-memory-contract.h` 由 MCU Native 应用与服务器 Native 测试共同编译，验证零长/极大请求、calloc 溢出与清零、16 B 对齐、扩容保留数据、失败保留旧块、缩容、复用以及 realloc(NULL)/realloc(p,0)。服务器新增 `NATIVE_ALLOCATOR: PASS`，MCU新增`[PROGRAM-ALLOCATOR] PASS common SDK malloc/calloc/realloc/free`； MCU测试链接同一生产分配器与编译器支持库，经过真实 SVC 分配用户内存。

最小 libc 回归另修复了 fopen 写/追加模式缺少 VFS 写访问位，以及 fclose 吞掉 flush/close 错误的问题；free-list 验证接受任一容量足够的已释放块，不将合法的 first-fit 选择误判为失败。原有 Native 集群与 Linux futex 回归继续保留。

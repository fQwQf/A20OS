# 跨 QEMU 共享内核中的 Linux 用户线程

在 [54](54-stock-qemu-shared-kernel.md) 的两个系统 QEMU/KVM 进程中，现在可以
运行同一个普通 Linux ABI 程序的 pthread 工作线程。测试程序只使用标准 libc、
pthread、sched、mmap/mprotect、signal 和 clock 接口，没有集群专用 syscall。
共用生产调度器和同一用户地址空间，程序也可以在普通单实例 SMP 上运行。

## 本次补齐的执行机制

原先 `proc_sched_set` 拒绝任何正在执行的任务排除自己当前 CPU。因此普通
`sched_setaffinity(0, ...)` 无法把调用线程迁移到另一 CPU，初次真实 pthread
测试以 EINVAL 失败。

现在允许非 idle 的当前任务在可调度上下文中提出这样的亲和性变更。更新仍在
任务 `park_lock` 下完成，并记录待迁移状态。释放锁后让出 CPU；原 CPU 保留
栈所有权，直至 `proc_switch_complete` 已在替代任务的栈上执行，才按新的有效
亲和性把原任务加入目标运行队列并请求调度。三个“保留当前任务”快速路径
必须跳过待迁移任务，否则 syscall 虽返回成功，线程却还留在旧 CPU。

这条路径位于通用调度器，Linux 和 Native 包装层复用同一实现，没有按板型
新增 API。普通无迁移任务保留原有调度选择，只增加对待迁移标志的检查；本次
没有性能基准，不据功能测试声称调度开销完全不变。单核或普通本地设备路径
不启用共享 QEMU 的轮询和时钟后端。

当前仍拒绝从持有自旋锁、硬中断或关中断上下文主动迁移，也仍拒绝从其他
CPU 强行排除一个正在运行/分派的目标任务的执行 CPU。尚未实现通用远程
运行任务迁移的请求/确认协议。亲和性变更会与正常抢占、park/wake 和退出共用
任务所有权规则，不能提前清掉 `on_cpu` 来让两个 CPU 同时使用一个内核栈。

## 可复现验收

```sh
python3 tools/check-multi-qemu-kernel.py --workload linux \
  --out .kernel-build/cluster/multi-qemu/linux-run

python3 tools/check-multi-qemu-kernel.py --workload linux --mode smp \
  --out .kernel-build/cluster/multi-qemu/linux-smp-run
```

沿用 `RAMFS_USER=1 RAMFS_NATIVE_TESTS=1` 的显式测试资源配置，增加
`/bin/cluster_threads`。程序来源为 `user/cmds/stress/cluster_threads.c`，静态
链接项目 musl。宿主通过正常串口 shell 执行它，只读取串口/QMP/进程映射证据；
不更改 guest 页表、线程状态、结果或信号上下文。

程序创建两个 pthread 工作线程和一个协调主线程：

1. 两个工作线程分别进行 64 轮 CPU 亲和性切换，共 128 次设置。统计实际变化
   次数，不把首次可能已经位于目标 CPU 的设置算作迁移；应有 126–128 次实际
   变化。每轮校验实际 CPU、volatile TLS 内容、栈变量、pthread key，避免编译器
   将 TLS 检查完全替换为寄存器中的常量比较。
2. 协调线程对同一共享页进行 64 轮只读/可写权限切换。两个工作线程在只读阶段
   实际写入，必须收到地址匹配的 SIGSEGV，再通过自己的 TLS `sigjmp_buf` 恢复；
   共 128 次异常。恢复可写权限后，两端原子累加同一页，验证 TLB 更新后的访问。
   这覆盖该共享地址空间的权限切换，不等于完整 MM 压力或所有回收竞争验收。
3. 工作线程解除单 CPU 绑定，在两 CPU 上通过共享原子索引动态领取 1024 个
   计算片段。每片必须恰好执行一次，两个线程都必须有实际工作，总和必须为
   4332748800，执行 CPU 掩码必须为 3。分配数无需相等；没有吞吐或最优均衡要求。
4. 校验线程 join 返回值、两个 pthread key 析构、CLOCK_MONOTONIC 条件变量
   20 ms 绝对超时，以及内存/同步对象清理。shell 返回码必须为 0。

检查器保存分别来自两实例的原始串口日志，再汇总检查标志；目前仍不把两路
串口当作统一终端。页保护异常由 A20OS 正常异常和信号路径处理，不由宿主模拟。

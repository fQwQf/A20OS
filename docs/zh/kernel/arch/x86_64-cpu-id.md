# x86_64 当前 CPU 查询与桌面启动耗时

SMP 内核原先在每次 `arch_current_cpu_id()` 调用时执行 CPUID，再将 APIC ID 转换成逻辑编号。该函数位于锁、调度与内存管理热路径；KVM 下 CPUID 会引起虚拟机退出。

现在每个 CPU 的 GDT 第 7 项保存带标记的逻辑编号，内核用 LSL 读取该项的字节粒度 limit。这个描述符不装载到段寄存器，不改变任务的 GS/FS 或用户 TLS。引导和 AP trampoline 的 GDT 没有该项，LSL 验证失败时仍通过 CPUID/APIC 映射查询；单核构建继续直接返回 0。实现仅位于 `kernel/arch/x86_64`。

## 验证

四核 KVM、`ABI=both CONFIG_KERNEL_PREEMPT=1` 下，`cpuinfo_test 4 bench`先将休眠子进程分别绑定到各 CPU，核验每个核心上连续 200 次 `sched_getcpu()`，再测量 2 万次 `getpid()`。三次系统调用基准如下，单位微秒：

| 内核 | 第一次 | 第二次 | 第三次 | 中位数 |
| --- | ---: | ---: | ---: | ---: |
| CPUID 基线 | 168637 | 159640 | 159792 | 159792 |
| GDT/LSL | 12622 | 12501 | 12889 | 12622 |

新内核的 `sched_stress`、`gs_base_test`、`vm_fault_test` 均通过，包括迁移/IPI、用户 GS、fork/COW、抢占和并发匿名缺页。基线与新内核日志位于本地`build/browser-followup/cpu-lookup-{baseline,fast}.log`。

同一主机、4 CPU/4 GiB、virgl、相同 Minecraft 1.21.11 demo 世界的单次冷启动对比中，从存档检查结束、执行启动器到最后一张启动纹理图集加载完成，从 103.5 秒降到 76.8 秒，约缩短 26%。这是游戏初始化阶段的测量，不表示帧率提高相同比例，也不包含测试脚本启动前固定的 35 秒等待。优化后的串口时间戳见`build/browser-followup/input-desktop-fast-timing.log`；基线临时日志被后续复测覆盖，因此这项仅保留早期单次观察记录，正式性能比较需重新成对测量。

复测构建与客体命令：

```sh
make -j12 ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both NR_CPUS=4 \
  CONFIG_KERNEL_PREEMPT=1 dev-build
# 在对应 dev 镜像的 shell 内执行：
cpuinfo_test 4 bench
sched_stress
gs_base_test
vm_fault_test
```

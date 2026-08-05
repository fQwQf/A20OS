# 混合内核：能力与边界清单

本文档以清单形式说明 A20OS 混合内核**当前具备的能力**与**已知边界**。设计总览见 [00-design.md](00-design.md)，机制语义见 [01-mechanisms.md](01-mechanisms.md)，演进方向见 [02-mainstream-plan.md](02-mainstream-plan.md)。

## 核心架构原则（已贯彻）

```
用户态 ── syscall 线格式 ──> ABI 层（薄包装）── 内部 API ──> 内部实现
```

- 内部实现（`kernel/ipc`、`kernel/mm`、`kernel/proc`、`kernel/drivers`、`kernel/include/core`）独立自包含，对 `abi/` 零依赖（全仓审计）；
- Linux ABI 线格式常量由 `kernel/include/core/` 自持，`abi/linux/*` 再导出；
- 内部 IPC 子系统头位于 `kernel/include/ipc/`；
- 任何 ABI 只需薄包装即可共享内部混合内核机制。

## 能力清单

| 能力 | 状态 | 验证方式 |
|------|------|----------|
| IPC 融合快路径（`channel_call` + 时间片捐赠） | 已实现（捐赠仅 UP） | `smoke-native-ipc` |
| 共享内存 SPSC 环数据面 | 已实现 | 16MiB 跨进程完整性，`smoke-native-shmring` |
| 服务监管者（清单 + 依赖 + 崩溃重启） | 已实现 | 崩溃自愈 + 重启预算，`smoke-native-svc` |
| 服务注册表 + 按名解析 + 崩溃重绑 | 已实现 | 独立进程解析、崩溃后重绑，`smoke-native-registry` |
| 健康探针（ping + 超时强杀） | 已实现 | pong 通道随重启重注册 |
| 资源硬隔离（句柄配额 + 对象计数审计） | 已实现 | 崩溃循环计数零漂移，`smoke-native-isolation`，`/proc/a20/objects` |
| 用户态驱动框架（MMIO 授权 + IRQ→EventQ） | 已实现 | goldfish RTC 整体用户态化，`smoke-native-rtcd` |
| virtio-blk 用户态驱动（零拷贝 DMA） | 已实现 | FAT32 挂载 `/ubd`，`smoke-native-ubd` |
| 驱动崩溃恢复（在飞请求失败传导 + 重挂载） | 已实现 | `ubd_recover` |
| Linux ABI 透明获益（vDSO、唤醒快路径、AF_UNIX 桥接） | 已实现 | `smoke-clock-vdso` + unix 测试 |

## 正确性状态（SMP）

混合内核的 IPC/MM 路径在 SMP 下已收敛的已知问题（供参考，不构成开发记录）：

- **句柄表与 I/O scratch buffer 分离**：Native 句柄表使用独立 `task->a20_ht` 字段，不再与 Linux ABI 的 `proc_scratch_buffer()` 共用存储；
- **VMA 链表统一持锁**：`mm_mmap/munmap/brk/mprotect/mremap` 及 Linux ABI 对应 syscall 全部在 `mm->lock` 下修改 VMA 与页表；
- **远程 TLB 刷新**：IPI-based 远程刷（替代 SBI REMOTE SFENCE，TCG 下全量服务），且所有远程刷在解锁后发布、页释放前完成；
- **buddy 分配器**：脏块（内部帧仍被引用）不再回填空闲链表，`fl_push_clean` 拆解后只回填干净子块；
- **channel 入队**：`ch_try_enqueue` 对 peer 持引用，避免并发释放下的悬空入队。

## 已知边界

- **时间片捐赠仅限 UP**：SMP 捐赠依赖跨核唤醒/IPI 簿记（`PER_CPU_CURRENT_VALIDATION`），未完成前不开放；
- **Native ABI 偶发破坏（已知问题，低优先级）**：SMP=2/8 下 `native-shmring`（跨进程共享 VMO + channel 大块批量）仍有约 30% 概率的偶发内存破坏（页表/页表项交互方向）。**Native ABI 当前不常用**（比赛与日常路径均为 Linux ABI），且 Linux ABI 同负载下实测稳定（`mm_stress` SMP=2 连跑 15 轮零崩溃，`smoke-vfs/futex/sched/abi-linux` 全绿），因此暂不作为优先修复项。若重新启用 Native ABI 作为主要运行时，需先收敛此问题（诊断挂载点：`frame_trace_dump_pfn`、`[VMO-PAGE]`、`[PFA DIRTY-SPLIT]`、`[LOCK-STALL]`）；
- **网络协议栈**仍在内核（lwIP）；netd 外迁是后续最大单项；
- **loongarch64**：内核与 native 测试均构建通过，运行时复测受工具链/镜像条件所限未完整执行；
- **性能数据**全部来自 QEMU TCG 模拟器，真实硬件基准待测；
- **IOMMU/DMA 安全**：DMA 契约是"内核分配 + pin + 物理地址上报"的信任模型，无硬件 IOMMU 强制。

## 复现入口

```bash
make smoke-native-ipc       # IPC 快路径make smoke-native-svc       # 服务崩溃自愈make smoke-native-registry  # 注册表 + 重绑make smoke-native-isolation # 泄漏审计（崩溃循环零漂移）make smoke-native-rtcd      # 用户态 RTC 驱动make smoke-native-ubd       # 用户态 virtio-blk + 文件系统make smoke-native-shmring   # 共享环数据面make smoke-clock-vdso       # vDSOmake smoke-mm-stress smoke-vfs-stress   # Linux ABI 回归
# 驱动崩溃恢复（手动）：QEMU 加 bus.3 scratch 盘，运行 /bin/ubd_recover
```

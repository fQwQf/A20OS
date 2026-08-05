# A20OS 混合内核：业界标准形态状态总结

本文档汇总混合内核工程的全部实现与验收状态（截至本提交）。设计原则见[docs/OS-Design.md](../OS-Design.md)；分阶段实施记录见[01-roadmap.md](01-roadmap.md) 与 [02-mainstream-plan.md](02-mainstream-plan.md)。

## 核心架构原则（已贯彻）

```
用户态 ── syscall 线格式 ──> ABI 层（薄包装）── 内部 API ──> 内部实现
```

- 内部实现（`kernel/ipc`、`kernel/mm`、`kernel/proc`、`kernel/drivers`、
  `kernel/include/core`）独立自包含，对 `abi/` **零依赖**（全仓审计）；
- Linux ABI 线格式常量由 `kernel/include/core/` 自持，`abi/linux/*` 再导出；
- 内部 IPC 子系统头位于 `kernel/include/ipc/`；
- 结论：**任何 ABI 只需薄包装即可共享内部混合内核机制**。

## 业界标准能力清单

| 能力 | 状态 | 验证 |
|------|------|------|
| IPC 融合快路径（channel_call + 时间片捐赠） | ✅ | 4 次陷入/往返（减半），UP RPC 提升 16%（ratio 83），`smoke-native-ipc` |
| 共享内存 SPSC 环数据面 | ✅ | 16MiB 跨进程完整性，与 channel 持平且陷入少两个量级，`smoke-native-shmring` |
| 服务监管者（清单 + 依赖 + 崩溃重启） | ✅ | 两轮崩溃自愈 + 重启预算（flap budget），`smoke-native-svc` |
| 服务注册表 + 按名解析 + 崩溃重绑 | ✅ | 独立进程解析 rtcd、崩溃后自动重绑，`smoke-native-registry` |
| 健康探针（ping + 超时强杀） | ✅ | 0 误报，pong 通道随重启重注册 |
| 资源硬隔离（句柄配额 + 对象计数审计） | ✅ | 100 次崩溃循环六项计数零漂移，`smoke-native-isolation`，`/proc/a20/objects` |
| 用户态驱动框架（MMIO 授权 + IRQ→EventQ） | ✅ | goldfish RTC 整体用户态化，含 100ms 闹钟，`smoke-native-rtcd` |
| virtio-blk 用户态驱动（零拷贝 DMA） | ✅ | FAT32 挂载 `/ubd` 经内核代理 + 共享环 + DMA 直写页缓存，`smoke-native-ubd` |
| **驱动崩溃恢复** | ✅ | 杀死驱动 → 在飞请求 -EIO 传导 → 原地重挂载，挂载与数据完好，`ubd_recover` |
| Linux ABI 透明获益 | ✅ | vDSO 4.3×；AF_UNIX socketpair/connect 数据面走内部 channel，`smoke-clock-vdso` + unix_test/unix_ch_test |
| vDSO（无陷入时钟） | ✅ | musl `clock_gettime` 2457ns vs syscall 10637ns，双路径位级一致 |
| 唤醒优先抢占（两 ABI 共享） | ✅ | park.c priority-preempt，pipe/socket/futex/channel 统一 |
| 分层原则（内部 ↔ ABI 薄包装） | ✅ | 内部 abi 依赖清零，三 ABI 构建 |

## 关键数据（QEMU TCG，smp=1）

| 指标 | 数值 | 说明 |
|------|------|------|
| `clock_gettime`（musl） | 2457 ns（vDSO） vs 10637 ns（syscall） | **4.3×** |
| RPC 往返（channel_call+捐赠） | 66.5 µs vs legacy 79.6 µs | **+16%** |
| 16 MiB 跨进程传输 | ring ≈ channel（39–42 MiB/s 级） | 持平，ring 陷入少两个量级 |
| `/ubd` 文件读（冷→热） | 2442 ms → 3–13 ms（4 MiB FAT32） | 页缓存完全吸收用户驱动路径 |
| 崩溃自愈延迟 | crash → healed → rebound < 2 s（含 50 ms 退避） | svcman v2 |
| 100 次崩溃循环对象计数 | 零漂移 | 无泄漏 |

## 已证明的崩溃隔离不变式

1. 服务死亡 → 句柄表销毁 → 类型化对象回收（句柄/VMO/channel 端点/EventQ/IRQ 注册），六项计数回零；
2. 监管者 EXITED + 退出码检测 → 退避 → 重启 → 客户端按名自动重绑；
3. 驱动死亡 → 在飞 I/O 失败传导（-EIO，内核等待者不挂死）→ 原地重挂载，文件系统与数据完好。

## SMP 正确性修复（2026-08-05 批）

SMP=2/8 下 `native-shmring`（16MiB channel 批量 + 共享 VMO 环）的偶发内存破坏（`[SLAB BUG] kfree`、用户栈/代码页被数据覆盖、buddy 链表损坏）已定位并修复：

1. **handle table 与 Linux I/O scratch buffer 共用存储**：exec/startup 把 Native
   handle table 存入 `task->scratch_buf`，而 `proc_scratch_buffer()`（Linux ABI的 I/O 缓冲）复用同一字段，Native 任务执行 Linux syscall 会 `kfree(ht)` 或把数据写进句柄表。新增 `task->a20_ht` 专用字段，exec 切换 ABI 时释放旧表，退出清理分离（commit `98a1260`）。
2. **VMA 链表无锁修改**：`mm_mmap/munmap/brk/mprotect/mremap` 原先不持
   `mm->lock`，与 fault 的持锁遍历竞争导致链表撕裂。拆 `_locked` 变体 + 持锁wrapper；`proc_brk/mmap/munmap` 与 Linux `sys_mprotect/sys_mremap` 改调locked 版（后两者原本自己持锁，直接调 wrapper 会同锁重入死锁，mm_stress 卡死在 mremap 阶段，已修复）——commit `98a1260`、`933026e`。
3. **远程 TLB flush 死锁与 TCG 不可靠**：SBI REMOTE SFENCE.VMA 在 QEMU TCG 下
   不可靠，且持 `mm->lock`（关中断）时互发远程刷会 ABBA 死锁。改为 IPI-based远程刷（pending + generation，等待时开中断），并把所有远程刷延迟到解锁后（wrapper、trap 返回前、mm_destroy/vmo_destroy 在页释放**之前**刷）——commit `98a1260`、`1af0d02`。
4. **buddy 脏块重入**：释放 order>0 块时若内部帧仍被引用（COW/VMO 分离引用），
   整个脏块会被 push 回空闲链表，之后分配把在用页再次给出（VMO memset 覆盖用户页）。`fl_push_clean()` 现在拆解脏块、只回填干净子块，并有 DIRTY-SPLIT 诊断守卫 —— commit `1af0d02`。
5. **channel enqueue 无 peer 引用**：`ch_try_enqueue()`（send 快路径）未像 park
   路径那样 `refcount_inc_not_zero`，并发 `ep_release()` 可能释放正在入队的 peer；现在入队期间钉住 peer 引用 —— commit `1af0d02`。

验证：IPI TLB flush 全量服务（598/598）；`mm_stress` 全 PASS；RISC-V32 构建修复（`__atomic_exchange_8`/`arch_tlb_flush_page_local` 缺失）；`check-build-matrix-all`、`check-arch-boundary`、`check-abi-boundary`、`check-mm-lock-model`、`check-concurrency-foundation`、`check-final-definition`、`check-doc-test-gates` 全绿。

## 诚实边界（未完成 / 待验证）

- **SMP**：时间片捐赠仅限 UP（SMP 缺跨核唤醒簿记，`current.c` 文档化的未完成项）；SMP=2/8 下 `native-shmring` 仍有极低概率的偶发破坏（页表/页表项交互方向，`frame_trace`/`VMO-PAGE` 诊断已就位，待继续收敛）；
- **网络协议栈**仍在内核（lwIP）；netd 外迁是后续最大单项；
- **loongarch64**：内核与 native 测试均构建通过，运行时复测受工具链/镜像条件所限未完整执行；
- **性能数据**全部来自 QEMU TCG 模拟器；真实硬件基准待测；
- **IOMMU/DMA 安全**：DMA 契约是"内核分配 + pin + 物理地址上报"的信任模型，无硬件 IOMMU 强制。

## 复现入口

```bash
make smoke-native-ipc       # IPC 快路径make smoke-native-svc       # 服务崩溃自愈make smoke-native-registry  # 注册表 + 重绑make smoke-native-isolation # 泄漏审计（100 次崩溃零漂移）make smoke-native-rtcd      # 用户态 RTC 驱动make smoke-native-ubd       # 用户态 virtio-blk + 文件系统make smoke-native-shmring   # 共享环数据面make smoke-clock-vdso       # vDSOmake smoke-mm-stress smoke-vfs-stress   # Linux ABI 回归
# 驱动崩溃恢复（手动）：
#   QEMU 加 bus.3 scratch 盘，运行 /bin/ubd_recover
```

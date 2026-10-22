# 双态部署驱动框架（dual-placement drivers）

“同一源码、双态部署”驱动框架目前只有骨架：当前代码尚未满足“完整驱动同一源码双态运行”的阶段验收。已按 2026-08 的共享头、drvmod 样板与用户服务核对。

## 问题

混合内核的边界若要成为“部署选择而非设计分叉”，驱动必须只写一份，按设备可信度与性能需求决定部署在内核态还是用户态。此前的状态是两种驱动各写各的：内核驱动直接用内核 API，用户驱动（rtcd/ubd）直接用 liba20rt，同一设备两种部署需要两份实现。这正是主流混合内核（NT UMDF、XNU DriverKit）也没能完全解决的分叉。

## 设计

### 三层结构

```
┌─ 设备协议层（共享，唯一源码）────────────────────┐
│  kernel/include/drivers/dual/<device>.h         │
│  寄存器映射 + 寄存器级协议，只调用 drv_* ops      │
├─ 环境层（双后端，唯一定义）──────────────────────┤
│  kernel/include/drivers/dual/drv_env.h          │
│  DRV_ENV_KERNEL → 直映 MMIO / 内核 API          │
│  DRV_ENV_USER   → udriver syscall / liba20rt    │
│  DRV_ENV_DRVMOD → drvmod 导出 API               │
├─ 壳层（部署相关，每侧一层薄壳）──────────────────┤
│  内核壳：init/probe/ISR/子系统挂接               │
│  用户壳：main 循环 + EventQ + 服务协议           │
└─────────────────────────────────────────────────┘
```

划分的语义判据：**只放各部署下语义可对齐的操作为 drv_* ops**。当前环境层覆盖 MMIO 及 DMA helper；IRQ 投递刻意留在壳层。头文件实现 KERNEL/USER/DRVMOD 三后端，但仓库内活跃样板只显式编译 USER 与 DRVMOD，暂没有以 `DRV_ENV_KERNEL` 构建的同源样板。

### ops 扩展的准入规则

一个 op 进入 drv_env 的前提是各部署下语义可定义一致且可测。DMA 分配 helper 已进入环境层；在 RISC-V QEMU 平台上，USER 后端的 `drv_dma` 对已 claim 的 PCI 隔离设备（edu 样板）经 per-device IOMMU domain 走 IOVA 翻译，授权窗口外的访问由硬件拒绝并消费 fault；virtio-mmio 用户驱动（uinputd/ubd）的设备不在 riscv-iommu 管理范围内，仍是内核分配连续 VMO 的信任模型，因此"所有 USER 驱动相同隔离强度"仍未成立。

### 语义一致性验证

同一设备协议层应在多种部署下跑同一套可观察行为检查。当前源码中的 virtio-input probe 契约让 DRVMOD 只读 probe 与 USER 驱动读取相同设备身份；历史 smoke 曾运行该序列（2026-08 核实时未重跑）。完整事件面只由 USER 壳的该契约覆盖。goldfish RTC 的 USER 壳使用共享头，但 DRVMOD probe 仍自行定义寄存器常量，尚不是同源契约测试。

## 明确的非目标与后续

- 内核壳接入 timekeeping/alarm 子系统是后续工作；接入前必须先解决设备所有权（udriver 窗口当前默认 user-owned，见 `udriver_mmio_user_owned`），所有权仲裁本身是框架的一部分。约定：白名单 `user_owned=1` 的设备内核侧只做只读 probe，破坏性初始化与 virtqueue 归用户壳独占；动态 `device_claim/release` 已实现，`smoke-dual-input` 源码会用两次启动检查自动释放，但本次未运行；user-owned 窗口的 MMIO 映射现在强制要求当前任务先 claim，rtcd/ubd/uinputd 已迁移；
- IRQ ops 暂不进 drv_env（线程模型差异是本质的，见上）；
- IOMMU 动态隔离：`riscv_iommu.c` 配置 DDT(1LVL)/CQ/FQ，devid 0 静态 SV39 domain 的 TR_REQ 映射/拒绝探测之上，已实现 per-device domain 的动态 claim/map/unmap/release、fault queue 消费与 fail-closed 阻断、`/proc/a20/iommu` 计数器。**多设备**：每个 claimed device 一个 domain，DDT 条目各占一个 devid；IOTLB 以 `(tag, GSCID, PSCID, IOVA)` 为键，所以每个 domain 分配唯一 TA.PSCID，否则两个设备对同一 IOVA 会互相命中缓存（实测：不设 PSCID 时 02.0 的 DMA 拿到 03.0 的物理页）。fault queue 是共享硬件队列，drain 时按记录里的 devid 归属到对应 domain，一个设备 fault 只阻断它自己。门禁 `smoke-iommu-udriver-isolation`（单设备 fault/recover）与 `smoke-iommu-multidevice`（两个 edu、两个 domain、dev0 fault 期间 dev1 继续 DMA、各自 release）均已 PASS。后续：fault 消费中断化、virtio-mmio 设备接入；
- virtio-input 事件面有 USER uinputd 路径和独立 DRVMOD 完整驱动路径；尚缺同一设备、同一完整协议源码、同一契约套件的双态 A/B；
- virtio-blk 保持内核数据面 + ubd 用户态 scratch 的现状，不作为双态候选（数据面跨边界两次的陷阱，见 03-refactor-plan）。

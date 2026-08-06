# 双态部署驱动框架（dual-placement drivers）

本文档定义"同一源码、双态部署"驱动框架的设计与当前骨架状态。
上位路线见 [03-refactor-plan.md](03-refactor-plan.md) 阶段三。

## 问题

混合内核的边界若要成为"部署选择而非设计分叉"，驱动必须只写一份，按设备可信度与性能需求决定部署在内核态还是用户态。此前的状态是两种驱动各写各的：内核驱动直接用内核 API，用户驱动（rtcd/ubd）直接用 liba20rt，同一设备两种部署需要两份实现——这正是主流混合内核（NT UMDF、XNU DriverKit）也没能完全解决的分叉。

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
├─ 壳层（部署相关，每侧一层薄壳）──────────────────┤
│  内核壳：init/probe/ISR/子系统挂接               │
│  用户壳：main 循环 + EventQ + 服务协议           │
└─────────────────────────────────────────────────┘
```

划分的语义判据：**只放两种部署下语义完全一致的操作为 drv_* ops**。
当前 ops 集（骨架）：MMIO 映射（`drv_mmio_map/unmap`）与 32 位寄存器读写（`drv_mmio_read32/write32`——映射后是等价的 volatile 访存，天然同语义）。IRQ 投递刻意留在壳层：内核壳用 request_irq + handler，用户壳用 IRQ→EventQ + ack 重新武装，两者的**线程模型合法地不同** （回调 vs 拉取），强行统一会扼杀用户态的事件循环结构。

### 后续 ops 扩展的准入规则

一个 op 进入 drv_env 的前提：两种部署下语义可定义一致且可测。
下一个候选是 DMA 缓冲分配（内核帧分配 vs udriver DMA VMO 契约），它必须先完成 IOMMU 强制（03-refactor-plan 阶段三的后半），否则"同语义"是假命题（一侧是信任模型，另一侧是硬件强制）。

### 语义一致性验证

同一设备协议层在两种部署下跑同一套可观察行为检查（当前：读时间单调、alarm 设置/清除寄存器序列一致）。随着骨架扩展，契约测试应能分别以 `-DDRV_ENV_KERNEL` 与 `-DDRV_ENV_USER` 构建并给出同结果——双态语义一致本身是测试资产，不是文档承诺。

## 当前实现状态（2026-08-06）

- `kernel/include/drivers/dual/drv_env.h`：环境层定义，两个后端。
  ops 集为 MMIO 映射、8/32 位寄存器读写、**DMA 缓冲分配** （页粒度，物理地址表在分配时固定）；用户态连续 DMA 由`device_alloc_dma`（预物化连续 VMO）提供，`smoke-native-contract` 的 `dma` 分区验证物理连续与零填充；
- `kernel/include/drivers/dual/goldfish_rtc.h`：goldfish RTC 寄存器
  协议唯一源码（自此 rtcd_proto.h 不再持有寄存器定义）；
- `kernel/include/drivers/dual/virtio_mmio.h` +
  `kernel/include/drivers/dual/virtio_input.h`：virtio-mmio 传输与virtio-input 设备协议唯一源码；
- `kernel/include/drivers/dual/virtq.h`：共享 split-virtqueue 层
  （单所有者破坏性初始化；全部环结构置于一个 DMA 页内）。
  实现过程中修正两个真实 spec 缺陷：`QUEUE_READY` 偏移为 0x044 （非 0x03c）；DRIVER_OK 必须在队列建立之后（拆分为`vinput_dev_init` + `vinput_driver_ok`）；
- 内核壳 `kernel/drvmod/examples/goldfish_rtc.c`：drvmod 模块形式的
  boot probe（qemu-virt-riscv64 下由 `init_kthread` 加载并自动绑定，日志 `[GOLDFISH-RTC] probe ok: epoch=<ns>`；原内建壳`kernel/drivers/char/goldfish_rtc_kdrv.c` 已随 drvmod 迁移删除，见 `docs/drivers/kernel-modules.md`）；
- 内核壳 `kernel/drvmod/examples/vinput_probe.c`：drvmod 模块的
  boot 只读 probe（无设备时静默），日志 `[UINPUT] kernel-placement probe:`；原内建壳 `kernel/drivers/input/virtio_input_kprobe.c` 已随 drvmod 迁移删除；
- 用户壳 `user/svc/rtcd.c`：已重构到共享协议层，
  `make smoke-native-rtcd` PASS；
- 用户壳 `user/svc/uinputd.c`：virtio-input 用户态 probe；
- **首个双态语义一致性验证**：`make smoke-dual-input` 挂
  `virtio-keyboard-device,bus=virtio-mmio-bus.5`，同一共享协议源码在两种部署下读出相同设备身份（内核 `[UINPUT] ... name=QEMU Virtio Keyboard`，用户 `UINPUTD: name=QEMU Virtio Keyboard`）；
- **首个功能态用户驱动**：uinputd 完成设备全权初始化（状态迁移、
  特性协商）、基于 drv_dma 的事件 virtqueue、IRQ→EventQ 投递，smoke 经 QEMU monitor `sendkey` 注入按键并验证解码出`EV_KEY/KEY_A/press` 真实事件——DMA ops、virtq 层、IRQ 链路全部经真实数据流验证；
- **DMA 契约修正**：`vmo_phys` 非物化（peek 语义，未触页报 pa=0），
  drv_dma 用户后端必须先物化再翻译（memset 触页同时提供清零保证），该契约已写入 drv_env.h 注释——否则驱动会把物理页 0 交给设备；
- **构建修复**：native 构建 stamp 的新旧检查此前不含 `user/svc` 与
  共享头目录，导致 svc/共享头修改后镜像内二进制陈旧；已修；
- 构建：riscv64 与 loongarch64 内核均通过；rtcd/uinputd 用户壳以
  `-Ikernel/include` 引入共享头（`NATIVE_RTCD_RECIPE`）。

## 明确的非目标与后续

- 内核壳接入 timekeeping/alarm 子系统是后续工作；接入前必须先解决
  设备所有权（udriver 窗口当前默认 user-owned，见`udriver_mmio_user_owned`），所有权仲裁本身是框架的一部分。
  当前约定（已验证有效）：白名单 `user_owned=1` 的设备内核侧只做只读 probe，破坏性初始化与 virtqueue 归用户壳独占；动态`device_claim/release` 已实现并在 `smoke-dual-input` 两次启动中验证自动释放；user-owned 窗口的 MMIO 映射现在强制要求当前任务先 claim，rtcd/ubd/uinputd 已迁移；
- IRQ ops 暂不进 drv_env（线程模型差异是本质的，见上）；
- **IOMMU 已完成硬件初始化与 per-domain 翻译**：`riscv_iommu.c`
  分配 BAR、读取 capability（version 16 / spec 1.0）、配置DDT(1LVL)/CQ/FQ 并使能；devid 0 配置 SV39 翻译域（3 级页表），经 TR_REQ 验证已映射 IOVA 精确翻译、未映射 IOVA 被硬件拒绝（fault=1, cause=13 RD_FAULT_S）；`smoke-iommu-discovery` 断言。
  devid 1（IOMMU 自身）保持 passthrough DC；fault 队列消费与per-device 页表动态映射仍为后续工作；
- virtio-input 事件面已在用户态跑通；内核壳接入 evdev/输入子系统
  是后续工作；
- virtio-blk 保持内核数据面 + ubd 用户态 scratch 的现状，不作为双态
  候选（数据面跨边界两次的陷阱，见 03-refactor-plan）。

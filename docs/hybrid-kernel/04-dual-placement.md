# 双态部署驱动框架（dual-placement drivers）

本文档定义"同一源码、双态部署"驱动框架的设计与当前骨架状态。
上位路线见 [03-refactor-plan.md](03-refactor-plan.md) 阶段三。

## 问题

混合内核的边界若要成为"部署选择而非设计分叉"，驱动必须只写一份，
按设备可信度与性能需求决定部署在内核态还是用户态。此前的状态是
两种驱动各写各的：内核驱动直接用内核 API，用户驱动（rtcd/ubd）直接
用 liba20rt，同一设备两种部署需要两份实现——这正是主流混合内核
（NT UMDF、XNU DriverKit）也没能完全解决的分叉。

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
当前 ops 集（骨架）：MMIO 映射（`drv_mmio_map/unmap`）与 32 位寄存器
读写（`drv_mmio_read32/write32`——映射后是等价的 volatile 访存，
天然同语义）。IRQ 投递刻意留在壳层：内核壳用 request_irq + handler，
用户壳用 IRQ→EventQ + ack 重新武装，两者的**线程模型合法地不同**
（回调 vs 拉取），强行统一会扼杀用户态的事件循环结构。

### 后续 ops 扩展的准入规则

一个 op 进入 drv_env 的前提：两种部署下语义可定义一致且可测。
下一个候选是 DMA 缓冲分配（内核帧分配 vs udriver DMA VMO 契约），
它必须先完成 IOMMU 强制（03-refactor-plan 阶段三的后半），否则
"同语义"是假命题（一侧是信任模型，另一侧是硬件强制）。

### 语义一致性验证

同一设备协议层在两种部署下跑同一套可观察行为检查（当前：
读时间单调、alarm 设置/清除寄存器序列一致）。随着骨架扩展，
契约测试应能分别以 `-DDRV_ENV_KERNEL` 与 `-DDRV_ENV_USER` 构建
并给出同结果——双态语义一致本身是测试资产，不是文档承诺。

## 当前骨架状态（2026-08-06）

- `kernel/include/drivers/dual/drv_env.h`：环境层定义，两个后端。
  ops 集已扩展为 MMIO 映射、8/32 位寄存器读写、**DMA 缓冲分配**
  （页粒度；信任模型，物理地址表在分配时固定。已知限制：用户态
  VMO 页可能不连续，多页连续 DMA 协议暂限单页，待 DMA-heap/IOMMU）；
- `kernel/include/drivers/dual/goldfish_rtc.h`：goldfish RTC 寄存器
  协议唯一源码（自此 rtcd_proto.h 不再持有寄存器定义）；
- `kernel/include/drivers/dual/virtio_mmio.h` +
  `kernel/include/drivers/dual/virtio_input.h`：virtio-mmio 传输与
  virtio-input 设备协议唯一源码；
- 内核壳 `kernel/drivers/char/goldfish_rtc_kdrv.c`：boot probe
  （qemu-virt-riscv64 下 `kernel_main` 调用，日志
  `[GRTC] kernel-placement probe: now=<ns>`）；
- 内核壳 `kernel/drivers/input/virtio_input_kprobe.c`：boot 只读
  probe（无设备时静默），日志 `[UINPUT] kernel-placement probe:`；
- 用户壳 `user/svc/rtcd.c`：已重构到共享协议层，
  `make smoke-native-rtcd` PASS；
- 用户壳 `user/svc/uinputd.c`：virtio-input 用户态 probe；
- **首个双态语义一致性验证**：`make smoke-dual-input` 挂
  `virtio-keyboard-device,bus=virtio-mmio-bus.5`，同一共享协议源码
  在两种部署下读出相同设备身份（内核 `[UINPUT] ... name=QEMU Virtio
  Keyboard`，用户 `UINPUTD: name=QEMU Virtio Keyboard`），两架构
  内核构建通过；
- 构建：riscv64 与 loongarch64 内核均通过；rtcd/uinputd 用户壳以
  `-Ikernel/include` 引入共享头（`NATIVE_RTCD_RECIPE`）。

## 明确的非目标与后续

- 内核壳接入 timekeeping/alarm 子系统是后续工作；接入前必须先解决
  设备所有权（udriver 窗口当前默认 user-owned，见
  `udriver_mmio_user_owned`），所有权仲裁本身是框架的一部分；
- IRQ ops 暂不进 drv_env（线程模型差异是本质的，见上）；
- DMA ops 已进 drv_env（信任模型）；IOMMU 硬件强制仍是独立工作项，
  完成后 drv_dma 的语义承诺才能从"内核担保"升级为"硬件强制"；
- virtio-input 的 virtqueue 事件面（破坏性初始化，单所有者）待所有权
  仲裁后进入共享层；
- virtio-blk 保持内核数据面 + ubd 用户态 scratch 的现状，不作为双态
  候选（数据面跨边界两次的陷阱，见 03-refactor-plan）。

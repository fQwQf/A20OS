# 双态部署驱动框架（dual-placement drivers）

本文档定义“同一源码、双态部署”驱动框架的设计与当前骨架状态，已按 2026-08 的共享头、drvmod 样板与用户服务核对。当前代码尚未满足“完整驱动同一源码双态运行”的阶段验收；下文明确区分环境后端、共享协议 probe 和完整驱动。

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
│  DRV_ENV_DRVMOD → drvmod 导出 API               │
├─ 壳层（部署相关，每侧一层薄壳）──────────────────┤
│  内核壳：init/probe/ISR/子系统挂接               │
│  用户壳：main 循环 + EventQ + 服务协议           │
└─────────────────────────────────────────────────┘
```

划分的语义判据：**只放各部署下语义可对齐的操作为 drv_* ops**。当前环境层覆盖 MMIO 及 DMA helper；IRQ 投递刻意留在壳层。头文件实现 KERNEL/USER/DRVMOD 三后端，但仓库内活跃样板只显式编译 USER 与 DRVMOD，暂没有以 `DRV_ENV_KERNEL` 构建的同源样板。

### ops 扩展的准入规则

一个 op 进入 drv_env 的前提是各部署下语义可定义一致且可测。DMA 分配 helper 已进入环境层，但 USER 后端当前依赖内核分配连续 VMO 与物理地址上报；RISC-V IOMMU 尚未把该分配动态映射到 per-device domain，因此“相同隔离强度”仍未成立。

### 语义一致性验证

同一设备协议层应在多种部署下跑同一套可观察行为检查。当前源码中的 virtio-input probe 契约让 DRVMOD 只读 probe 与 USER 驱动读取相同设备身份；历史 smoke 曾运行该序列（2026-08 核实时未重跑）。完整事件面只由 USER 壳的该契约覆盖。goldfish RTC 的 USER 壳使用共享头，但 DRVMOD probe 仍自行定义寄存器常量，尚不是同源契约测试。

## 当前实现状态（2026-08 源码）

- `kernel/include/drivers/dual/drv_env.h`：环境层定义三个后端（KERNEL/USER/DRVMOD）。ops 集为 MMIO 映射、8/32 位寄存器读写与 DMA 缓冲分配；当前样板源码使用 USER/DRVMOD，KERNEL 后端只有头文件实现；
- `kernel/include/drivers/dual/goldfish_rtc.h`：USER rtcd 使用的共享寄存器协议头；目标是成为唯一来源，但当前 DRVMOD probe 尚未消费它；
- `kernel/include/drivers/dual/virtio_mmio.h` + `kernel/include/drivers/dual/virtio_input.h`：DRVMOD 只读 probe 与 USER uinputd 共享的配置协议；完整 DRVMOD driver 另用 `drivers/input/virtio_input.h`；
- `kernel/include/drivers/dual/virtq.h`：共享 split-virtqueue 层（单所有者破坏性初始化；全部环结构置于一个 DMA 页内）。注意两处 spec 细节：`QUEUE_READY` 偏移为 0x044（非 0x03c）；DRIVER_OK 必须在队列建立之后（协议层拆分为 `vinput_dev_init` + `vinput_driver_ok`）。
- 内核壳 `kernel/drvmod/examples/goldfish_rtc.c`（rtc.a20drv）：只读 boot probe，但它不 include `drivers/dual/goldfish_rtc.h` 或 `drv_env.h`，而是本地重复 base/register 常量并调用 drvmod API；不能作为 goldfish 完整同源双态证据；
- 内核壳 `kernel/drvmod/examples/vinput_probe.c`（vinput-probe.a20drv）：统一 `driver_t` 模块的 boot 只读 probe（无设备时静默），日志 `[UINPUT] kernel-placement probe:`；原内建 `kernel/drivers/input/virtio_input_kprobe.c` 已随 drvmod 迁移删除；
- 内核完整驱动 `kernel/drvmod/examples/vinput.c`：状态迁移、事件 virtqueue 与 IRQ 以模块形式实现并发布 input class 设备，但使用 `drivers/input/virtio_input.h` 和独立 virtqueue 实现，不与 uinputd 共享完整协议/数据面源码；它绑定非 user-owned 设备，不能与 slot 5 的 USER 驱动做同设备完整 A/B；
- 用户壳 `user/svc/rtcd.c`：产物为 `rtcd-<arch>.a20drv`，已重构到共享协议层；`make smoke-native-rtcd` 是 RISC-V64 验证入口，本次未运行；
- 用户壳 `user/svc/ubd.c`：产物为 `ubd-<arch>.a20drv`，作为 virtio-blk 用户态驱动运行；
- 用户壳 `user/svc/uinputd.c`：产物为 `uinputd-<arch>.a20drv`，是可初始化设备、建立 virtqueue 并处理 IRQ 的功能驱动，不只是 probe；
- **共享协议 probe**：`make smoke-dual-input` 的设计是在 slot 5 上由 DRVMOD 只读 probe 与 USER 驱动通过 `drivers/dual/virtio_input.h` 读出相同身份；这是配置读取一致性，不是完整双态驱动功能一致性；
- **功能态用户驱动入口**：uinputd 实现设备全权初始化、drv_dma 事件 virtqueue 和 IRQ→EventQ；`smoke-dual-input` 会经 QEMU monitor `sendkey` 注入按键并要求解码 `EV_KEY/KEY_A/press`，但本次未重跑；
- **DMA 契约修正**：`vmo_phys` 非物化（peek 语义，未触页报 pa=0），drv_dma 用户后端必须先物化再翻译（memset 触页同时提供清零保证），该契约已写入 drv_env.h 注释——否则驱动会把物理页 0 交给设备；
- **构建依赖**：native 构建 stamp 的依赖清单包含 `user/svc` 与共享头目录（svc/共享头修改会触发镜像内二进制重建）；
- 构建配方支持 riscv64/loongarch64 内核和相应 Native 用户壳；构建结果需在当前提交复验。rtcd/uinputd 以 `-Ikernel/include` 引入共享头。

## 明确的非目标与后续

- 内核壳接入 timekeeping/alarm 子系统是后续工作；接入前必须先解决设备所有权（udriver 窗口当前默认 user-owned，见 `udriver_mmio_user_owned`），所有权仲裁本身是框架的一部分。约定：白名单 `user_owned=1` 的设备内核侧只做只读 probe，破坏性初始化与 virtqueue 归用户壳独占；动态 `device_claim/release` 已实现，`smoke-dual-input` 源码会用两次启动检查自动释放，但本次未运行；user-owned 窗口的 MMIO 映射现在强制要求当前任务先 claim，rtcd/ubd/uinputd 已迁移；
- IRQ ops 暂不进 drv_env（线程模型差异是本质的，见上）；
- **IOMMU bring-up**：`riscv_iommu.c` 配置 DDT(1LVL)/CQ/FQ，并用 devid 0 的静态 SV39 domain 做 TR_REQ 映射/拒绝探测；fault 队列消费、per-device 动态页表及 `drv_dma` 接线仍为后续工作；
- virtio-input 事件面有 USER uinputd 路径和独立 DRVMOD 完整驱动路径；尚缺同一设备、同一完整协议源码、同一契约套件的双态 A/B；
- virtio-blk 保持内核数据面 + ubd 用户态 scratch 的现状，不作为双态候选（数据面跨边界两次的陷阱，见 03-refactor-plan）。

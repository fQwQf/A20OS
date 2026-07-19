# 驱动实现符合性与限制

本页记录当前源码事实和可依赖的运行边界。审查覆盖 driver core、公共总线、STM32F103、VirtualBox ARM64/x86_64 及其设备驱动。

QEMU x86_64 与 RISC-V64 GUI 的 VirtIO GPU、键盘和鼠标分别由
`make smoke-qemu-gui-{x86_64,riscv64,aarch64,arm32,loongarch64}` 做行为验证；测试检查非空 scanout，并向
客户机注入真实按键事件。修改 driver core、IRQ、DMA、PCI/VirtIO transport、
display、input、framebuffer 或 devfs 聚合路径时，该项是必跑回归测试。

## 核心与公共基础设施

| 范围 | 状态 | 说明 |
|---|---|---|
| driver core | 符合 | 动态 registry；操作 mutex 串行化注册、probe/remove 与遍历；数组 spinlock 不跨回调；拒绝重复注册；probe 失败完整解绑 |
| PCI bus | 已扩展 | ECAM、ID/subsystem match、BAR sizing/assignment、BAR 编号查询、modern VirtIO capabilities；已设置 `matched_id` |
| VirtIO-MMIO bus | 符合 | MMIO/IRQ 资源、type match、`matched_id`；静态最多 8 个设备 |
| hwapi MMIO | 符合 | 宽度访问和 barrier；relaxed 版本需谨慎 |
| hwapi IRQ | 明确边界 | 固定 256 线、每线一个 handler；表操作受锁保护，重复注册返回 `-EBUSY`；`free_irq` 校验 owner token 并等待在途 handler |
| hwapi DMA | 基础可用 | coherent helper 返回物理 handle，sync 委托 arch cache hook；尚无 DMA mask/IOMMU/大对齐能力 |
| devfs | 固定节点模型 | display/input 使用类适配器；其他类不自动生成节点 |

## 设备驱动

| 驱动 | 类 | 状态与限制 |
|---|---|---|
| virtio-blk | BLOCK | 类接口；remove 停止设备并释放已注册 IRQ；有限静态实例 |
| virtio-net | NET | 多实例、IRQ/轮询和类接口；remove 释放 IRQ 并复位 transport |
| virtio-input | INPUT | 类接口、PCI/MMIO、多实例槽和 remove；由 `/dev/event0` 聚合 |
| virtio-gpu | DISPLAY | class registry、framebuffer 页释放和 transport reset；单实例、同步 controlq |
| VirtIO-SCSI | BLOCK | VirtualBox ARM 已验证，remove 复位/释放槽；只支持 target/LUN 0、READ/WRITE(10)、512B sector、轮询 |
| AHCI | BLOCK | VirtualBox x86_64；单 controller/单 port/单 slot、LBA48、轮询；probe 回滚和 remove 释放 DMA/IRQ |
| E1000 | NET | VirtualBox 82540EM，轮询 ring，已加 stop/remove；静态单实例 |
| VMSVGA/SVGAv3 | DISPLAY | VirtualBox x86_64/ARM，BAR offset/pitch 边界和 class registry，已加 remove；静态单实例 |
| xHCI HID | INPUT | VBox ARM keyboard/mouse/tablet，class、轮询和 controller stop/remove；只匹配 `8086:1e31`，静态单 controller |
| PS/2 | x86 板级服务 | 提供基础键鼠控制器服务；可复用输入设备使用 `DEV_CLASS_INPUT` |
| STM32 SDIO | BLOCK | 统一类 + MCU bridge；板级 bus 仍用名称匹配 |
| STM32 简单外设 | 允许例外 | 板级轮询轻量 API，不强制统一对象；扩展到多实例/用户 ABI 时必须迁移 |
| StarFive/LS2K GMAC、DW SDIO | 有条件 | 单实例轮询并依赖外部串行化，SMP/IRQ 化前必须增加实例锁 |

## VirtualBox 平台

VirtualBox ARM64 源码现位于 `kernel/platform/virtualbox-aarch64/`，通过 UEFI ACPI RSDP/MCFG 枚举 PCI。VirtIO-SCSI、E1000、SVGAv3、xHCI HID 已进入统一类模型。平台 GIC disable 已实现；generic timer trap 和 PCI interrupt routing 仍使数据面主要轮询。

VirtualBox x86_64 复用 x86_64 平台 PCI、AHCI、VMSVGA、PS/2/E1000/VirtIO 驱动，以 GRUB ISO 启动。运行配置见 [ARM64](virtualbox-aarch64.md) 与 [x86_64](virtualbox-x86_64.md) 手册，驱动架构见 [VirtualBox 驱动栈](virtualbox.md)。

## 强制边界

- 从 `kernel_main` 直接调用硬件 init，而不是 `DRIVER_REGISTER` 加枚举。
- 只暴露 `*_get_dev()` 全局 getter，不实现 class ops。
- 设备发现和功能发布使用 class；块层适配对象不能代替驱动注册。
- probe 获得 DMA/IRQ 后直接 `return -1` 而不回滚。
- 空 remove，即使驱动启用了 DMA、IRQ 或类级全局 registry。
- 在 spinlock 下执行秒级 busy poll。
- 在具体驱动中添加静态 devfs vnode，而不是通用类适配器。

新改动不得扩大表中边界。依赖轮询、固定实例数或固定节点时，提交必须写明适用平台、并发假设和失败行为。

已清理的兼容债务：`kernel_main` 不再直调 VirtIO GPU/input 初始化；block mount
和 network init 不再启动架构私有 VirtIO PCI 扫描。QEMU x86_64 现在只有统一
PCI bus 拥有 BAR 与 transport，VirtIO-MMIO 通过其 bus device ID 匹配
virtio-blk。不得恢复这些双初始化入口。

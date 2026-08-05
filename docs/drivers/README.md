# A20OS 驱动文档

如何为 A20OS 添加一个驱动：从读懂硬件手册，到实现可编译、可枚举、可绑定、能清理的驱动。

通用实现位于 `kernel/drivers/`，板级适配位于 `kernel/platform/`，公共头文件位于 `kernel/include/drivers/`。

## 推荐阅读顺序

1. [从零开发第一个驱动](getting-started.md)：从 PCI 网卡模板入手，走通创建、注册、构建和调试。
2. [核心模型](core-model.md)：`device_t`、`driver_t`、`bus_type_t` 和完整的 probe/remove 模板。
3. 根据总线继续读：
   - [总线与平台](bus-and-platform.md)：platform device、板级资源和 MMIO/IRQ。
   - [PCI 与 VirtIO](pci-and-virtio.md)：PCI ID、BAR、ECAM、VirtIO transport。
4. [设备类](device-classes.md)：各类操作、单位、返回值和阻塞语义。
5. [运行时契约](runtime-contracts.md) + [锁顺序](lock-order.md)：IRQ、标准完成模型、DMA、屏障、并发。
6. [用户接口与 devfs](userspace-and-devfs.md)：从 class 到 `/dev`；显示设备另读 [Display/Framebuffer](display.md)，音频设备另读 [音频子系统](audio.md)。
7. [构建、测试与提交](testing-and-submission.md)：编译矩阵、失败清理和提交证据。

## 文档列表

| 文档 | 说明 |
|---|---|
| [getting-started](getting-started.md) | 第一个驱动的完整流程 |
| [core-model](core-model.md) | 核心对象与生命周期 |
| [device-classes](device-classes.md) | 功能类的接口约定 |
| [bus-and-platform](bus-and-platform.md) | 平台与总线枚举 |
| [pci-and-virtio](pci-and-virtio.md) | PCI/VirtIO 接入 |
| [runtime-contracts](runtime-contracts.md) | MMIO、IRQ、DMA、标准完成模型、锁、超时 |
| [lock-order](lock-order.md) | 锁的嵌套规则 |
| [userspace-and-devfs](userspace-and-devfs.md) | `/dev` 与 class 桥接 |
| [display](display.md) | framebuffer 与 GPU 驱动 |
| [audio](audio.md) | 音频类、UAPI 与 Intel HDA PCM |
| [virtualbox](../platforms/virtualbox.md) | VirtualBox 驱动发现链 |
| [testing-and-submission](testing-and-submission.md) | 驱动提交清单与命令 |
| [implementation-status](implementation-status.md) | 当前实现矩阵 |

## 相关文档

- VirtualBox 运行手册：ARM64 见 [../platforms/virtualbox-aarch64.md](../platforms/virtualbox-aarch64.md)，x86_64 见 [../platforms/virtualbox-x86_64.md](../platforms/virtualbox-x86_64.md)。
- 测试门禁说明：见 [../testing/testing-gates.md](../testing/testing-gates.md)。

# A20OS 驱动文档

如何为 A20OS 添加一个驱动：从读懂硬件手册，到实现可编译、可枚举、可绑定、能清理的驱动；以及 generic 的模块优先与 embedded 的全内建部署策略。

通用实现位于 `kernel/drivers/`，板级适配位于 `kernel/platform/`，公共头文件位于 `kernel/include/drivers/`，可加载驱动模块位于 `kernel/drvmod/examples/`。

## 分类导航

### 一、入门与机制（怎么写驱动）

| 文档 | 说明 |
|---|---|
| [从零开发第一个驱动](guide/getting-started.md) | 从 PCI 网卡模板入手，走通创建、注册、构建和调试 |
| [可安装内核驱动](guide/kernel-modules.md) | 可加载内核模块（drvmod）、框架导出 API、DriverStore 与迁移状态 |
| [驱动部署 Profile](guide/deployment-profiles.md) | generic 模块优先、embedded 全内建、Early/Runtime DriverStore |
| [核心设备与驱动模型](guide/core-model.md) | `device_t`、`driver_t`、`bus_type_t` 与 probe/remove 生命周期 |
| [设备类接口](guide/device-classes.md) | 各类操作、单位、返回值和阻塞语义 |
| [总线与平台](guide/bus-and-platform.md) | platform device、板级资源、MMIO/IRQ |
| [PCI 与 VirtIO](guide/pci-and-virtio.md) | PCI ID、BAR、ECAM、VirtIO transport |
| [运行时契约](guide/runtime-contracts.md) | MMIO、IRQ、DMA、锁、超时 |
| [驱动锁顺序契约](guide/lock-order.md) | 锁的嵌套规则 |

### 二、设备类专章（按硬件类）

| 文档 | 说明 |
|---|---|
| [输入子系统](classes/input.md) | `/dev/event0` mux、vinput.a20drv 模块、evdev ioctl 面、双驻留协调 |
| [Display/Framebuffer](classes/display.md) | framebuffer 与 GPU 驱动 |
| [音频子系统](classes/audio.md) | 音频类、UAPI 与 Intel HDA PCM |
| [USB 子系统设计](classes/usb-design.md) | USB 协议栈与 HID/存储类 |
| [用户接口与 devfs](classes/userspace-and-devfs.md) | 从 class 到 `/dev` 的桥接 |

### 三、状态与流程

| 文档 | 说明 |
|---|---|
| [驱动实现符合性与限制](meta/implementation-status.md) | 当前实现矩阵：已迁移模块、不可迁移边界、驱动类状态 |
| [构建、测试与提交](meta/testing-and-submission.md) | 编译矩阵、失败清理和提交证据 |

### 四、平台与混合内核参考

| 文档 | 说明 |
|---|---|
| [VirtualBox 驱动发现链](../platforms/virtualbox.md) | VirtualBox x86/ARM 平台驱动 |
| [双态部署驱动框架](../hybrid-kernel/04-dual-placement.md) | 内核/用户双驻留驱动与所有权仲裁 |
| [混合内核机制](../hybrid-kernel/01-mechanisms.md) | 混合内核核心机制 |

## 推荐阅读顺序

1. [从零开发第一个驱动](guide/getting-started.md)：从 PCI 网卡模板入手，走通创建、注册、构建和调试。
2. [可安装内核驱动](guide/kernel-modules.md)：可加载内核模块、框架 API 与 DriverStore；新驱动优先按模块编写。
3. [核心模型](guide/core-model.md)：`device_t`、`driver_t`、`bus_type_t` 和完整的 probe/remove 模板。
4. 根据总线继续读：
   - [总线与平台](guide/bus-and-platform.md)：platform device、板级资源和 MMIO/IRQ。
   - [PCI 与 VirtIO](guide/pci-and-virtio.md)：PCI ID、BAR、ECAM、VirtIO transport。
5. [设备类](guide/device-classes.md)：各类操作、单位、返回值和阻塞语义；写输入/显示/音频驱动前先读对应专章。
6. [运行时契约](guide/runtime-contracts.md) + [锁顺序](guide/lock-order.md)：IRQ、DMA、屏障、并发。
7. [用户接口与 devfs](classes/userspace-and-devfs.md)：从 class 到 `/dev`。
8. [构建、测试与提交](meta/testing-and-submission.md)：编译矩阵、失败清理和提交证据。

## 相关文档

- VirtualBox 运行手册：ARM64 见 [../platforms/virtualbox-aarch64.md](../platforms/virtualbox-aarch64.md)，x86_64 见 [../platforms/virtualbox-x86_64.md](../platforms/virtualbox-x86_64.md)。
- 测试门禁说明：见 [../testing/testing-gates.md](../testing/testing-gates.md)。

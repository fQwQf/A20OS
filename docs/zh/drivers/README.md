# A20OS 驱动文档

这里为两类开发者提供入口：实现一个具体驱动，或维护通用驱动框架。先建立对象与生命周期的共同认识，再进入所需的总线、设备类和执行路径。

## 从这里开始

| 你的任务 | 阅读与实施路线 |
|---|---|
| 开发具体驱动 | [架构总览](guide/core-model.md#架构与开发入口) → [第一个驱动](guide/getting-started.md) → 所用总线/设备类 → [测试与提交](meta/testing-and-submission.md) |
| 开发异步 DMA/GPU/加速器后端 | 上述路线，再读[请求与队列](guide/request-queues.md)，复用 native memory、fence 和可失败停止 |
| 完善接口或核心机制 | [核心模型](guide/core-model.md) → [框架维护与扩展](guide/framework-development.md) → [运行时契约](guide/runtime-contracts.md)与[锁顺序](guide/lock-order.md) |
| 查阅驱动支持范围 | 参阅各设备类专章与 [1.0 常见 PC 与 QEMU 驱动范围](meta/v1-pc-qemu-scope.md) |

框架按发现、绑定、协议、资源、执行和部署组织。传统设备类与无 class 服务共用原生绑定模型；Linux ABI 与 Native ABI 位于消费适配边界。完整同步骨架只维护在核心模型中，异步骨架只维护在请求队列指南中，入门页按步骤引用它们。

通用实现位于 `kernel/drivers/`，公共声明分布在 `kernel/include/drivers/` 与既有 `kernel/drivers/core/` 核心头；板级适配位于 `kernel/arch/<arch>/boards/`。generic 设备模块包装在 `kernel/drvmod/examples/`，构建账本见 `tools/mk/driver-sources.mk` 与 `driver-modules.mk`；新设备文件不会只因放入类目录就自动接入。

## 分类导航

### 一、入门与机制（怎么写驱动）

| 文档 | 说明 |
|---|---|
| [从零开发第一个驱动](guide/getting-started.md) | 从 PCI 网卡模板入手，走通创建、注册、构建和调试 |
| [可安装内核驱动](guide/kernel-modules.md) | 可加载内核模块（drvmod）、框架导出 API、DriverStore 与迁移状态 |
| [驱动部署 Profile](guide/deployment-profiles.md) | generic 架构模块清单、embedded 静态账本、Early/Runtime DriverStore |
| [启动与硬件发现](../dev/boot-model.md) | 交接阶段、DT/ACPI/静态 provider、设备图、资源和依赖 |
| [x86 实机驱动复用](guide/x86-hardware-reuse.md) | 代码归属、硬件匹配、上游服务边界与移植验证 |
| [核心设备与驱动模型](guide/core-model.md) | 完整架构、原生 endpoint/protocol、所有权与 probe/remove 生命周期 |
| [驱动框架维护与扩展](guide/framework-development.md) | 源码地图、扩展落点、公共契约、协议/模块 ABI 与验证选择 |
| [热插拔与即插即用](guide/hotplug-and-pnp.md) | USB/PCI 运行期发现、ADD/REMOVE 生命周期与驱动编写要求 |
| [设备类接口](guide/device-classes.md) | 各类操作、单位、返回值和阻塞语义 |
| [总线与平台](guide/bus-and-platform.md) | platform device、板级资源、MMIO/IRQ |
| [PCI 与 VirtIO](guide/pci-and-virtio.md) | PCI ID、BAR、ECAM、VirtIO transport |
| [运行时契约](guide/runtime-contracts.md) | MMIO、IRQ、DMA、锁、超时 |
| [原生请求与队列](guide/request-queues.md) | admission、依赖、取消、IRQ 完成、任务退休、共享 worker 和可失败停止 |
| [原生驱动运行时](guide/native-runtime.md) | execution stop、资源占用、页级 DMA、fence、工作队列、固件及验证边界 |
| [驱动锁顺序契约](guide/lock-order.md) | 锁的嵌套规则 |

### 二、设备类专章（按硬件类）

| 文档 | 说明 |
|---|---|
| [输入子系统](classes/input.md) | `/dev/event0` mux、vinput.a20drv 模块、evdev ioctl 面、双驻留协调 |
| [Virtio-console 内存配置](classes/virtio-console.md) | 吞吐/小内存预设、队列和缓冲区配置、DMA 内存预算 |
| [Display/Framebuffer](classes/display.md) | framebuffer 与 GPU 驱动 |
| [3D 图形加速栈](../graphics/3d-graphics.md) | virtio-gpu virgl 3D：原理、接口、实现与用户态对接 |
| [真机 GPU 开发](../graphics/real-hardware-gpu.md) | Phoenix 首个目标、原生基础设施、上游复用与分阶段验收 |
| [音频子系统](classes/audio.md) | 音频类、UAPI 与 Intel HDA PCM |
| [USB 子系统设计](classes/usb-design.md) | USB 协议栈与 HID/存储类 |
| [用户接口与 devfs](classes/userspace-and-devfs.md) | 从 class 到 `/dev` 的桥接 |

### 三、状态与流程

| 文档 | 说明 |
|---|---|
| 驱动实现符合性与限制 | 当前实现矩阵：已迁移模块、不可迁移边界、驱动类状态 |
| [文档范围与一致性](meta/documentation-consistency.md) | 跨页面共同口径、权威入口和历史记录边界 |
| [构建、测试与提交](meta/testing-and-submission.md) | 编译矩阵、失败清理和提交证据 |

### 四、平台与混合内核参考

| 文档 | 说明 |
|---|---|
| [VirtualBox 驱动发现链](../platforms/virtualbox.md) | VirtualBox x86/ARM 平台驱动 |
| [双态部署驱动框架](../hybrid-kernel/04-dual-placement.md) | 内核/用户双驻留驱动与所有权仲裁 |
| [混合内核机制](../hybrid-kernel/01-mechanisms.md) | 混合内核核心机制 |

## 文档一致性

现行指南、设备专章及架构/启动/图形/状态页使用同一套绑定、请求和退出术语。文档分工、共同契约以及如何区分历史验证记录，集中在[文档范围与一致性](meta/documentation-consistency.md)。各类不再维护独立的旧式 probe/remove 模板。

## 阅读时的约定

核心模型解释“对象是什么、为什么这样退出”；入门页说明“先实现哪一步、怎样接到构建和消费者”；运行时与请求指南提供精确契约；设备类专章解释数据单位和子系统语义。无需先读完全部文档再写第一个驱动。

声明与结构以当前源码为准，文档中的骨架省略具体硬件协议。未实现的示例 I/O 不报告成功；可调用接口、完成验证的平台和后续硬件工作分别记录在状态文档中。

## 相关文档

- VirtualBox 运行手册：ARM64 见 [../platforms/virtualbox-aarch64.md](../platforms/virtualbox-aarch64.md)，x86_64 见 [../platforms/virtualbox-x86_64.md](../platforms/virtualbox-x86_64.md)。
- 测试门禁说明：见 [../testing-gates.md](../testing-gates.md)。

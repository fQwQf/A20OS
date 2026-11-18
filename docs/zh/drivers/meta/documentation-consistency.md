# 驱动文档范围与一致性

驱动文档共同描述一套原生设备与执行模型。新增硬件、修改框架或修订旧页面时，按本页核对跨文档口径，避免入口已更新而设备专章仍保留另一套生命周期。入门路线见[文档总入口](../README.md)。

## 共同口径

| 主题 | 当前约定 | 权威入口 |
|---|---|---|
| 发现与身份 | 固件/原生总线唯一发布硬件；功能驱动不重复扫描或自行抢配 BAR | [启动模型](../../dev/boot-model.md)、[总线](../guide/bus-and-platform.md) |
| 绑定与协议 | endpoint 属于当次绑定，传统六类提供 primary；扩展按 ID/version/实际 ops_size/功能位协商 | [核心模型](../guide/core-model.md)、[设备类](../guide/device-classes.md) |
| 调用与存储 | device、publication、active call、backing/mapping 各自持有引用；旧引用不随重绑，access 不保留返回数据 | [运行时契约](../guide/runtime-contracts.md) |
| 完成与退休 | native queue 的 IRQ complete 记录硬件交还，任务 retire 成功后发布最终 fence；超时不授权释放 DMA | [请求与队列](../guide/request-queues.md) |
| 退出与失败 | 可选 prepare → consumer/child → offline → 可失败 execution stop → quiesce → call drain → remove → devres；失败保留、显式重试 | [核心生命周期](../guide/core-model.md#生命周期的精确语义)、[热插拔](../guide/hotplug-and-pnp.md) |
| 部署与权限 | generic/embedded 共用内核模型；用户服务另有 broker/rights/隔离后端，功能 capability 不等于授权 | [部署配置](../guide/deployment-profiles.md)、[模块](../guide/kernel-modules.md)、[用户接口](../classes/userspace-and-devfs.md) |
| 能力与证据 | 通用队列已接入 VirtIO GPU；其他专用数据路径不自动迁移，平台/后端与实机证据分别声明 | 实现状态、已知缺口 |

## 文档分工

| 文档范围 | 应维护的内容 |
|---|---|
| [第一个驱动](../guide/getting-started.md)、[框架维护](../guide/framework-development.md) | 两条开发路线、扩展落点和源码地图 |
| [核心模型](../guide/core-model.md)、[请求与队列](../guide/request-queues.md) | 集中维护完整同步/异步 C 骨架，其他页面引用它们 |
| [设备类](../guide/device-classes.md) | 单位、成功值、借用寿命与可选操作；避免复制会过期的完整 vtable 或各类 probe/remove |
| [PCI/VirtIO](../guide/pci-and-virtio.md)、[总线](../guide/bus-and-platform.md)、[热插拔](../guide/hotplug-and-pnp.md) | 地址、transport、枚举与设备事件，使用共同生命周期 |
| [运行时契约](../guide/runtime-contracts.md)、[原生运行时](../guide/native-runtime.md)、[锁顺序](../guide/lock-order.md) | MMIO/IRQ/DMA/VMO/fence/work 的精确条件与锁上下文 |
| [内核模块](../guide/kernel-modules.md)、[部署配置](../guide/deployment-profiles.md) | descriptor、imports、源码/包账本、Early/Runtime、适用配置 |
| [Display](../classes/display.md)、[Input](../classes/input.md)、[Audio](../classes/audio.md)、[USB](../classes/usb-design.md) | 子系统/硬件专有协议与现状，不重定义通用对象寿命 |
| [VirtIO console](../classes/virtio-console.md)、[TUN](../classes/tun.md)、[x86 复用](../guide/x86-hardware-reuse.md)及用户服务专章 | 保留合理的专用数据路径、权限与容量规则；不把“有 native queue”写成“所有后端已迁移” |
| [测试与提交](testing-and-submission.md) | 按改动范围选择验证，区分语法、主机模型、QEMU I/O 和实机证据 |

架构总览、启动、图形、混合内核状态页引用同一原生模型；其中 Linux DRM/virgl 是消费适配和特定后端，不是通用内核驱动 ABI。模块 ABI 从 A20_DRIVER_ABI 读取；协议版本独立协商，历史引入版本不冒充当前版本。

## 当前规则与历史记录

原生运行时的“历史验证记录”、x86 复用的旧镜像证据和图形文档带提交号的排查，保留当时的测试数、候选方案与限制。当前约定看相应页面的前部契约和实现状态；不能把旧阶段的“尚未实现多实例”“后续统一请求队列”重新写成当前待办，也不能把旧证据改写为新硬件已通过。

旧 get_fb 物理视图、IOMMU 翻译/失效、GPU BO/GPUVM、master/auth、驱动服务恢复等仍按实际支持范围记录。更新通用架构不抹掉真实后端缺口，也不把模型停止成功当成实体硬件已验证。

## 修订检查

1. 搜索所有相关指南、类专章、架构/启动/状态页，核对同一对象和阶段是否使用一致的名称与所有权。
2. 删除或修订重复的旧模板，特别是直接解引用物理 BAR、无停止证明就 free、在 remove 才唤醒请求、保存裸 vtable、假成功或不存在的 API。
3. 对完整 C 骨架使用当前头文件作语法检查；输入事件等架构相关线格式引用权威头，不在文档中复制错误宽度。
4. 校验链接、锚点和代码行引用；新增当前指南进入导航。文档改动不增加“实机已验证”的声明。
5. 只修改叙述/API 注释时按文档验证交付；改变运行行为则按[框架维护指南](../guide/framework-development.md#7-按改动选择验证)选择生产工作流和平台测试。

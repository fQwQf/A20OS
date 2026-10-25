# 启动与硬件发现模型

A20OS 将启动交接、硬件描述、总线枚举和驱动绑定分开。UEFI、Multiboot 是交接协议；DT、ACPI、静态描述提供硬件事实；PCI、USB、VirtIO 等总线发布运行时设备。驱动通过同一组节点、资源和生命周期接口消费这些信息。

## 代码组织

| 目录 | 职责 |
|---|---|
| `kernel/arch/<arch>/boot/` | 启动寄存器、入口汇编、交接适配；UEFI loader 位于其 `uefi/` 子目录 |
| `kernel/arch/<arch>/boards/<board>/` | 板级早期控制台、启动窗口、中断控制器、供电和必要的固件勘误 |
| `kernel/arch/<arch>/platform/` | MMIO 映射属性、PCI 配置访问、固件物理映射、IRQ 路由等硬件机制 |
| `kernel/boot/` | 架构无关的启动上下文和阶段检查 |
| `kernel/firmware/` | 节点抽象、DT/ACPI/静态 provider、资源转换、IRQ domain 和设备图构建 |
| `kernel/drivers/bus/` | 原生总线枚举、transport 和子设备发布 |
| `kernel/drivers/core/` | 匹配、依赖、probe/remove、引用、受管资源和 class 发布 |

公共代码可以调用 `arch_*` 能力接口；CPU 指令、地址空间约定、页表格式和板级地址只能在 `arch/` 实现。设备协议驱动不能包含特定架构或板级实现头文件。

## 启动阶段

`boot_context_t` 定义在 `kernel/include/boot/context.h`。架构入口通过 `arch_boot_context_init()` 发布一次交接信息：协议、命令行、已映射且具有独立可访问长度的 DTB，以及 ACPI RSDP 的物理地址。公共解析器不自行推导指针的物理地址。

```mermaid
flowchart LR
    RESET --> HANDOFF --> EARLY --> MEMORY --> DEVICES --> READY
```

| 阶段 | 完成条件 |
|---|---|
| HANDOFF | 架构交接数据已发布 |
| EARLY | 异常入口、早期控制台、板级早期准备和时基可用 |
| MEMORY | RAM 与保留区已交给内存管理器 |
| DEVICES | 固件设备图、原生总线和板级策略已发布，可以完成驱动绑定 |
| READY | 调度与系统初始化达到正常运行入口 |

`boot_context_advance(expected, next)` 只接受相邻阶段，重复或越级返回错误。硬件根节点必须在进入 DEVICES 前确定。小型 MCU profile 使用自身启动路径，但发布相同的上下文和有序阶段。

当前 21 个活跃操作系统板型的 Make 源图均包含公共启动上下文与对应架构适配器。20 个 hosted 板型使用公共设备图；STM32 的小容量 MCU profile 保留架构目录内的直接外围服务，裁剪掉完整驱动核心。BL702 是独立 SRAM 探针，不能计作完整操作系统适配；LoongArch32/PPC64LE 已退出活跃构建。`tools.tests.test_arch_layout` 遍历实际板目录并检查每个活跃目标的源图，新增板型会自动纳入。

早期串口、中断控制器和页分配前的内存发现属于启动依赖，允许由架构适配器直接准备。普通设备应在设备图发布后绑定，不能在公共启动代码中增加某块板的驱动调用。

## 硬件描述与设备图

`fw_node_t` 是 `{ops, context, id}` 构成的借用句柄，接口位于 `kernel/include/firmware/node.h`。provider 提供父子关系、可用性、属性、硬件 ID、物理资源和带参数的 provider 引用。属性显式携带字节序；通用读取函数检查长度、字符串终止和整数范围。

| 来源 | 实现与语义 |
|---|---|
| DT | 有界 FDT 视图；处理地址/长度 cell、多级 `ranges`、`reg-names`、状态继承、phandle 和 IRQ nexus |
| ACPI | 有界表读取器与 uACPI 解释器；执行 namespace 初始化及 `_HID/_CID/_STA/_CRS/_ADR/_UID`，通过 `_PRT` 和中断链接设备路由 PCI INTx |
| static | 调用者提供不可变节点、属性、资源和引用数组；使用相同接口，无需伪造 DTB |
| 其他来源 | 实现 `fw_node_ops_t`，保持稳定身份和描述存储的生命周期，然后调用统一枚举器 |

DT 和 ACPI 可同时存在于交接信息中。当前启动策略优先使用已发布的 DT 硬件根；无 DT 时使用 ACPI namespace；两者均无有效硬件根时才调用板级静态 fallback。存在 DT 时，ACPI 仍可提供表信息及电源服务。不会把两个来源的同一设备盲目注册两次。

`firmware_device_enumerate(root)` 在 discovery 事务中递归构建设备图，**不要求驱动预先注册**。未知设备保留为未绑定节点；禁用设备不绑定，ACPI 中允许工作的子节点按 `_STA` 语义处理。重复枚举同一节点具有幂等性。解析或注册失败回滚本次新增设备和自动 IRQ domain；不把损坏的固件当成“缺少固件”而猜测地址。

PCI 和 USB 继续使用原生总线模型。VirtIO-MMIO 的固件节点绑定 transport bridge，再发布 VirtIO 子设备；静态 slot 扫描只用于明确声明的无固件 fallback。板级 `configure_devices` 可标记用户态拥有的设备，或附加已验证的 `software_id` 勘误；它应复用已有 canonical 节点。

固件 provider 使用不可变的启动快照；运行期 PCI/USB 变化由原生总线的重扫和撤销路径处理。DT overlay 和 ACPI 动态 `_CRS` 刷新不属于当前 provider 接口。

provider 的原始描述、属性和名称必须活到所有引用它们的设备释放。DTB 因此属于保留内存；ACPI namespace 快照和静态表也不能在枚举返回时销毁。

## 资源、IRQ 与依赖

所有 `RES_MMIO` 的 `start/end` 都是 CPU **物理地址**，范围两端包含。驱动通过 `device_map_mmio()`、`device_map_resource()` 或 `pci_map_bar()` 获取内核可访问映射；架构层验证窗口及设备内存属性。驱动不能自行加减 `PAGE_OFFSET`，也不能把 MMIO 当 RAM 做 DMA 地址转换。

IRQ 描述首先保留为“控制器节点 + binding 参数”。`fw_irq_domain_t` 将硬件中断译成驱动 IRQ，并通过控制器的架构路由实现极性和触发设置。控制器未准备好返回 `-EPROBE_DEFER`；无中断与错误的中断描述具有不同语义。仅确实支持轮询的驱动可声明 `irq_optional`，跳过尚不可解析的 domain；损坏的中断描述仍然失败。

时钟、复位、电源等依赖使用 `fw_node_reference()` 获取 provider 和参数。驱动使用 `device_dependency_get()` 建立依赖关系：provider 尚未发现或绑定时延后 probe；核心拒绝依赖环，并在 provider 撤销前先解除 consumer 绑定。引用机制不等于已经实现所有厂商的 clock/reset 驱动。

## 驱动绑定与对象生命周期

platform 驱动使用 `driver_t.firmware_ids` 声明稳定硬件 ID；DT compatible、ACPI HID/CID 与静态 hardware-id 通过同一匹配接口处理。原生 PCI ID 匹配保持在 PCI bus。驱动不应按节点名称猜测型号。

核心在 discovery 完成、驱动到达或显式重试时，对 deferred 设备执行有界的进展重试。永久 probe 错误保存在 `probe_error`，不会当成依赖未就绪而无限循环。

注册表、父子设备关系、类设备、依赖和进度快照分别持有对象引用。`device_unregister()` 撤销注册与绑定；最终 `device_put()` 才调用动态设备的 `release()`。lookup 中名字带 `find_get` 的接口返回引用，调用者必须归还。

块类索引和借用的 block adapter 各有 256 个启动期身份预算，绑定撤销后不将旧索引分配给另一块盘。旧句柄永久返回 `-ENODEV`，离线 adapter 立即释放其设备引用；耗尽预算明确拒绝新绑定。

probe 可以通过 `device_add_resource_action()` 登记回收动作。映射 helper 已自动登记；probe 失败或 remove 后，核心逆序释放受管资源和依赖。驱动仍须停止 DMA、撤销 IRQ、释放未登记的分配；不能在硬件仍活动时依赖回收器直接释放内存。

内核模块驱动 ABI 为 **6**，包含显式 framebuffer 布局接口；旧 ABI 包会被拒绝，必须与当前内核一起重建。没有保留按旧 compatible 查找驱动再反向创建设备的路径。

设备发现完全由固件和原生总线发布。旧的 `arch_virtio_blk_probe`、`arch_virtio_net_probe` 以及驱动内自行扫描并发布已初始化实例的入口均已移除。

## 当前边界与验证入口

页分配器最多保留 64 段 RAM，描述超过容量时明确失败，不截断。当前驱动 IRQ registry 为 256 项；自动 domain 支持单个主 GIC/PLIC 或 ACPI GSI，多个主控制器须显式注册互不冲突的 domain。x86 当前 GSI 路由只覆盖现有向量分区容纳的 0..23。DT `interrupt-map-pass-thru`、需要固件分配的动态 reserved-memory 尚不支持，会明确返回错误；PCI 三 cell 地址资源交给原生 PCI bus，不作为普通 platform MMIO 猜测转换。新增 provider 不会自动补齐这些硬件能力。

宿主验证直接链接生产解析器、设备核心及 uACPI，替换分配器和真实硬件访问：

```sh
make check-firmware-model
python3 tools/gates.py check-arch-boundary
```

`make smoke-uefi-x86_64` 使用 OVMF 从临时 ESP 启动独立 UEFI loader，验证内存图、ACPI namespace、bring-up 和 S5 退出；可通过 `OVMF_CODE`、`OVMF_VARS` 指定固件文件。

QEMU 启动与每块板的部署说明分别见 [实例系统](instances.md) 和 [平台文档](../platforms/README.md)。构建通过、QEMU 运行和实机运行是不同的验证等级；固件兼容层的存在不代表任意机器上的全部设备已经可用。

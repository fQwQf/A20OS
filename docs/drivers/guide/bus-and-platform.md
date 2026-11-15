# 总线、平台与硬件资源

普通设备由固件 provider 或原生总线创建，驱动只消费身份、资源及依赖。交接协议、启动阶段、描述来源选择和失败回滚见 [启动与硬件发现](../../dev/boot-model.md)；驱动生命周期见 [核心模型](core-model.md)。

## 组织边界

| 位置 | 职责 |
|---|---|
| `kernel/arch/<arch>/boards/<board>/` | 早期控制台、启动窗口、中断控制器、供电、静态描述与板级勘误 |
| `kernel/firmware/` | DT、ACPI、静态描述的通用解析、节点和设备图 |
| `kernel/drivers/bus/` | PCI/USB 等原生枚举与 transport 子设备 |
| `kernel/drivers/<class>/` | 可复用设备协议，只消费公共资源、属性和硬件能力接口 |

CPU 指令、页表格式、端口指令和板级地址全部属于 `arch/`。驱动不能包含板级 `platform.h`，板级代码不能复制 AHCI、E1000 或 VirtIO 协议。

## 板级启动接口

每个板定义唯一 `current_board`，类型为 `board_config_t`，由构建系统选择。其设备相关 hooks 按下列顺序使用：

1. `early_init`：内存分配器和 driver core 之前的准备，不分配普通运行时设备。
2. `enumerate_devices`：仅在没有有效固件硬件根时调用，发布静态 provider fallback。
3. `enumerate_buses`：发布 PCI 等自枚举总线，补充固件设备图。
4. `configure_devices`：在 probe 前设置用户态所有权或已验证的设备勘误，复用已有设备。

有 DT/ACPI 时不得用固定地址表重新创建同一设备。静态资源必须是该板已知事实；缺少描述、无驱动和损坏的描述不能混为一类。

## 固件设备与匹配

DT compatible、ACPI HID/CID、静态 hardware-id 都通过 `fw_node_t` 处理。platform 驱动声明：

```c
static const firmware_device_id_t timer_ids[] = {
    { .id = "vendor,my-timer" },
    { .id = "VND0001", .source = "acpi" },
    { 0 }
};

static driver_t timer_driver = {
    .name = "my-timer",
    .bus = &platform_bus,
    .firmware_ids = timer_ids,
    .probe = timer_probe,
    .remove = timer_remove,
};
```

`firmware_match->driver_data` 可区分寄存器协议变体。硬件身份不能从驱动注册状态反向推导；即使没有匹配驱动，设备仍应出现在设备图中。

没有固件的板使用 `firmware/static.h` 中的不可变 `fw_static_tree_t`，通过 `fw_static_init()` 和 `firmware_device_enumerate()` 发布。节点属性、资源、名称及 provider 引用存储必须覆盖设备的整个生命周期。不要在枚举栈上分配这些数据，也不要为静态表构造假的 DTB。

## MMIO 与 PCI BAR

`resource_t.start/end` 是包含两端的物理范围。先检查范围和最小 aperture，再映射：

```c
resource_t *r = device_get_resource(dev, RES_MMIO, 0);
if (!r || r->end < r->start || r->end - r->start < 0xfff)
    return -EINVAL;
void *regs = device_map_mmio(dev, 0);
if (!regs)
    return -ENODEV;
```

映射 helper 为设备登记回收动作，在已安全清理的 probe 失败或成功 remove 后逆序回收；停止/失效失败则随绑定保留。架构层验证映射属性与可达性；驱动不加减 `PAGE_OFFSET`。

PCI BAR 使用 `pci_get_bar_resource(dev, physical_bar_number)` 和 `pci_map_bar()`，不能按资源数组下标猜 BAR 编号。host 配置访问、可达性及分配窗口由架构接口提供；普通驱动不扫描 ECAM 或修改其他设备的 BAR。具体规则见 [PCI 与 VirtIO](pci-and-virtio.md)。

## IRQ 与 provider 依赖

固件 IRQ 首先是控制器节点和 binding 参数，经 `fw_irq_domain_t` 解析后才成为 `RES_IRQ`。`platform_device_irq(dev)` 返回第一个已解析中断：

| 返回值 | 语义 |
|---|---|
| `>= 0` | 可交给 `request_irq()` 的驱动中断编号 |
| `-ENODEV` | 没有发布中断资源 |
| `-EINVAL` | 非 platform 设备或中断资源不是单点 |
| `-ERANGE` | 超过当前 256 项驱动 IRQ registry |

provider 未就绪在进入 probe 前返回 `-EPROBE_DEFER`；有轮询实现的驱动可显式声明 `irq_optional`。错误的 IRQ 编码、无支持的触发方式和越界不能静默当成“没有中断”。驱动不得猜 GSI、GIC SPI 或 PLIC 编号。

时钟、复位等依赖通过 `device_dependency_get()` 获取并建立 consumer/provider 关系，调用者归还返回的设备引用。核心在 provider 到达后重试，在 provider 撤销前先解除 consumer 绑定，并拒绝依赖环。

架构 IRQ 入口负责 ack/EOI，普通 handler 不直接写 GIC 或 APIC。只有共享线上的所有 handler 都使用 `IRQF_SHARED` 时才允许共享。请求 IRQ 后，退出路径必须先按设备协议处理生产者和在途请求，再排空/撤销 handler；新异步驱动把可失败停止登记到 call drain 前的 execution 阶段。只有确认不再使用 priv、ring 和 MMIO，才能在 remove/devres 交还存储。

## 时间与早期控制台

`timer_ops.read_ticks` 必须单调且回绕安全，`ticks_per_sec` 返回与计数器一致的真实频率。轮询同时设置时间和迭代上限。不得在 IRQ handler 或自旋锁内执行长延时。

早期 UART 通过架构 console adapter 提供，在普通设备模型就绪前服务启动日志。终端缓冲与行规程属于通用代码；寄存器布局、引脚、时钟和实际 IRQ 由架构或板级适配器提供。

STM32F103 的板级外设服务、构建与部署状态见 [平台文档](../../platforms/stm32f103-port.md)。需要多实例、通用 `/dev`、热插拔或跨板复用的外设应使用 device/driver 模型。

## 新平台接入

在 `arch/<arch>/boards/<board>/` 实现交接适配、早期控制台、内存保留、中断和电源机制，优先消费 DT 或 ACPI；仅为缺少固件描述的硬件提供静态 provider。已有 PCI、USB、VirtIO 设备使用公共枚举器和驱动。移植步骤与验收要求见 [平台移植指南](../../platforms/porting-guide.md)。

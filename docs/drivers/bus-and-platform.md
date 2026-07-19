# 总线、平台与硬件资源

本文说明硬件如何变成 `device_t`。先确定设备挂在已有总线上，还是固定在某块板上。已有 PCI/VirtIO 设备直接使用公共枚举器；只有新硬件拓扑才需要写平台或总线代码。涉及 MMIO/IRQ/DMA 的运行时规则见 [运行时契约](runtime-contracts.md)，锁规则见 [锁顺序](lock-order.md)，PCI 与 VirtIO 细节见 [PCI 与 VirtIO](pci-and-virtio.md)。

## 分层边界

```text
kernel/platform/<board>/       板级事实：RAM、固定地址、ACPI/FDT、时钟、引脚
kernel/drivers/bus/             总线协议：扫描、匹配、BAR/slot、子设备
kernel/drivers/<class>/         可复用功能驱动：只消费 resource/plat_data
kernel/include/drivers/         跨模块公共接口
```

可复用驱动不得包含某块板的寄存器地址、GIC 编号或引脚号。平台代码不得复制 AHCI、E1000、VirtIO、VMSVGA 等设备协议。

## `board_config_t`

每个目标板定义唯一的：

```c
const board_config_t *const current_board = &my_board;
```

字段包括 RAM 范围、`irqchip_ops_t`、`timer_ops_t`、`early_init`、电源操作和 `enumerate_devices`。虽然存在 `BOARD_REGISTER` 宏，当前各平台实际通过 `current_board` 编译期选择，外部移植应遵循现有平台做法。

启动顺序中 `early_init` 在内存和 driver core 之前，只能做无需动态分配的早期控制台/硬件准备。`enumerate_devices` 在 driver core 初始化后运行，可以调用 `bus_register()`、`device_register()`、`pci_enumerate()` 或 `virtio_mmio_enumerate()`。

## 固定平台设备模板

设备和资源必须是静态或动态长生命周期对象：

```c
static resource_t timer_resources[] = {
    { .type = RES_MMIO, .start = MY_TIMER_VA,
      .end = MY_TIMER_VA + 0xfff, .flags = IORESOURCE_MMIO_32BIT,
      .name = "registers" },
    { .type = RES_IRQ, .start = MY_TIMER_IRQ,
      .end = MY_TIMER_IRQ, .flags = IORESOURCE_IRQ_LEVEL,
      .name = "irq" },
};

static device_t timer_device = {
    .name = "my-timer0",
    .bus = &platform_bus,
    .res = timer_resources,
    .res_count = ARRAY_SIZE(timer_resources),
};

static void my_enumerate_devices(void) {
    int ret = bus_register(&platform_bus);
    if (ret < 0)
        return;
    ret = device_register(&timer_device);
    if (ret < 0)
        bus_unregister(&platform_bus);
}
```

平台 bus 的 `match` 应使用稳定的 ID 或 `plat_data` compatible 值，不要只比较驱动名称。若只有一个板内设备且暂时使用名称匹配，必须把它标为平台私有且禁止通配其他驱动。

## MMIO 资源中的地址

`resource_t.start/end` 存放驱动可直接访问的内核虚拟地址，不一定是裸物理地址。板级映射负责建立地址属性。PCI BAR 通过 `arch_pci_bar_to_resource()` 转换；AArch64 VirtualBox 会加 `PAGE_OFFSET`，VMSVGA 则在向用户态报告显存时再保存/计算物理地址。

驱动不得自行无条件加 `PAGE_OFFSET`。只有确实需要 DMA 地址或用户映射物理地址时使用 `va_to_pa()`，并先确认该地址属于线性映射 RAM；MMIO BAR 不是普通 RAM，不能交给 `va_to_pa()` 当 DMA buffer。

## IRQ controller 契约

板级 irqchip 必须实现使能、禁用和 EOI。`driver_irq_dispatch()` 在架构入口已读取中断号后调用 handler，最后 EOI；handler 不自行调用 GIC EOI。`request_irq` 当前最多支持 IRQ 0..255，且每条线只有一个 handler 槽。`IRQF_SHARED` 标志尚未提供通用 handler 链；共享设备必须由一个聚合 handler 分发，VirtIO input 是现有实例。

## Timer 契约

`timer_ops.read_ticks` 必须单调回绕安全，`ticks_per_sec` 返回真实频率。`udelay/mdelay` 是 busy wait，不能在持有自旋锁、IRQ handler 或长数据路径中大量使用。VirtualBox ARM 的 generic timer 当前被 trap，平台使用软件 fallback，所以其超时还必须增加迭代上限，不能只依赖 tick 前进。

## STM32F103 例外

STM32F103 的 SDIO 是持久块设备，已使用 `DEV_CLASS_BLOCK`。显示、触摸、UART、Wi-Fi/蓝牙由 `stm32_peripherals_service()` 轮询，属于 MCU profile 的板级服务模型。LED、背光、风扇、蜂鸣器、DHT11、光敏传感器和按键可保留轻量 API；但不得在 IRQ 中执行长延时协议。需要多实例、通用 `/dev`、热插拔或跨板复用时必须转换为本章的 device/driver 模型。STM32F103 移植细节见 [STM32F103 移植](../platforms/stm32f103-port.md)。

## 移植新平台的步骤

1. 创建 `kernel/platform/<board>/` 和平台头，定义 RAM、启动地址、早期 MMIO 映射。
2. 提供链接脚本，包含 `.driver_init` 的 start/end 与 `KEEP`。
3. 实现 `current_board`、irqchip、timer、early console 和 power/reboot。
4. 从 ACPI/FDT/固件或固定资源枚举设备；优先调用已有总线枚举器。
5. 在 Makefile 选择正确 `BOARD_INCLUDE_DIR`、平台源文件和链接脚本。
6. 先构建 `kernel-only`，再验证串口日志中的 driver core、总线和设备绑定。
7. 最后才接用户镜像、网络和 GUI，避免把文件系统/桌面问题误判为平台驱动问题。

> ⚠️ 注意
> 不要把板的寄存器地址、GIC 编号或引脚号直接写进可复用驱动。不要在 IRQ handler 或持有自旋锁时调用 `udelay/mdelay`。不要给 MMIO 地址无条件加 `PAGE_OFFSET`，也不要把 MMIO BAR 当成普通 RAM 传给 `va_to_pa()`。可复用驱动只消费 `resource_t` 和 `plat_data`；VirtualBox 相关内容见 [VirtualBox](../platforms/virtualbox.md)。

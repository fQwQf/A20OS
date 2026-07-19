# VirtualBox 驱动栈与运行入口

> ❌ 不要这样做：不要为 VirtualBox 创建“VBox 专用 AHCI/E1000”等重复驱动。优先使用标准 PCI ID 表和通用 class 驱动；同一协议在其他虚拟机或真机上应复用同一驱动。

在 VirtualBox 上运行 A20OS 时，内核通过标准平台发现与通用 PCI 驱动枚举设备。ARM64 与 x86_64 启动路径不同，但 PCI 设备最终进入同一 device/driver/class 模型。

## ARM64 发现链

```text
UEFI BOOTAA64.EFI
 -> loader 传递 ACPI RSDP，装载 kernel at 0x08080000
 -> virtualbox-aarch64 early page tables / PL011 / GICv3
 -> board enumerate: RSDP -> XSDT/RSDT -> MCFG
 -> pci_enumerate(ECAM)
 -> PCI device_register
 -> VirtIO-SCSI / E1000 / VMSVGA / xHCI HID probe
 -> block/net/display/input class consumers
```

平台源码位于 `kernel/platform/virtualbox-aarch64/`。物理 RAM 为 `0x08000000..0x28000000`，kernel entry 为 `0x08080000`；PL011、GICD/GICR 和 PCI windows 在早期页表中映射。ECAM 物理地址由 MCFG 提供，驱动看到的是加 `PAGE_OFFSET` 的内核地址。

GICv3 架构 trap 层负责 distributor/CPU interface 初始化；board irqchip 提供 enable/disable/eoi。当前 PCI MSI/INTx 路由尚不完整，所以 VirtIO-SCSI、VMSVGA、E1000 与 xHCI 主要轮询。ARM generic timer 在 VirtualBox EL1 被 trap，软件 fallback 不可作为唯一超时来源。

## ARM64 设备矩阵

| VirtualBox 设备 | PCI ID | 驱动 | 类 | 当前模式 |
|---|---|---|---|---|
| VirtIO-SCSI | `1af4:1048` | `block/virtio_scsi.c` | BLOCK | 三个 split queue，requestq 轮询 |
| Intel E1000 82540EM | `8086:100e` | `net/e1000.c` | NET | RX/TX descriptor ring 轮询 |
| SVGAv3/VMSVGA | `15ad:0406` | `gpu/vmsvga.c` | DISPLAY | BAR0 regs、BAR2 VRAM、update command |
| Intel Panther Point xHCI | `8086:1e31` | `input/xhci_hid.c` | INPUT | USB boot keyboard/mouse/tablet，轮询 event ring |

VirtIO-SCSI 使用 LUN 0、READ/WRITE(10) 和 512 字节扇区；启动盘首先解析 GPT 第一个 partition，再尝试 FAT32 `/bin`。E1000 由 lwIP 按 `DEV_CLASS_NET` 自动挂接，ARM board bootargs 为 VBox NAT 默认地址。VMSVGA probe 调用 display registry，`/dev/fb0` 映射可见 scanout。xHCI HID 事件由 `/dev/event0` 聚合。

## x86_64 发现链

x86_64 使用 GRUB Multiboot ISO 启动，平台 PCI host 枚举 VirtualBox 设备。典型驱动为：VMSVGA `15ad:0405`、Intel AHCI `8086:2922/2829`、PS/2 键鼠，以及可配置的 VirtIO network。E1000 驱动现已存在，也可匹配 `8086:100e`。

AHCI driver 当前只管理首个可用 SATA port，使用 LBA48、512 字节扇区和单 command slot。PS/2 是 x86 平台的板级控制器服务；可复用输入驱动统一使用 `DEV_CLASS_INPUT`。

## 新增 VirtualBox 设备支持

1. 从串口 `[BUS] pci` 日志记录 vendor/device/subsystem/class/BAR。
2. 优先确认是否已有标准驱动；同一 PCI ID/协议不得新建 VBox 分叉。
3. 新 PCI 驱动使用 `pci_bus`、ID 表和 `pci_get_bar_resource`。
4. 无可靠 IRQ 时可以先轮询，但 poll 必须有预算，数据面不得无限阻塞；文档记录解除轮询的条件。
5. 在 ARM64 与 x86_64 都暴露相同硬件协议时，驱动不得包含 `CONFIG_BOARD_VIRTUALBOX_*`，架构差异留给 PCI HAL、DMA 和映射层。
6. 更新本页设备矩阵、[实现状态](../drivers/implementation-status.md) 和目标平台运行文档。

## 平台运行手册

创建虚拟机、制作/转换镜像、配置串口和逐项验收分别见：

- [VirtualBox ARM64 运行手册](virtualbox-aarch64.md)
- [VirtualBox x86_64 运行手册](virtualbox-x86_64.md)

运行配置只决定 VirtualBox 暴露哪些标准设备，不改变驱动接口；同一个 PCI ID/协议在其他虚拟机或物理机上仍应复用同一驱动。

## 构建与验证

ARM64 kernel：

```sh
make ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both kernel-only
make vbox-image-aarch64
```

x86_64：

```sh
make ARCH=x86_64 ABI=both kernel-only
make vbox-iso-x86_64
```

硬件日志至少应包含 ACPI MCFG/ECAM、目标 PCI ID、BAR、driver ready、对应类消费者（mount/lwIP/framebuffer/input）的成功信息。不能只以桌面出现或 shell 启动作为驱动成功证据，因为兼容设备或恢复路径可能掩盖目标驱动未绑定。

## 已知限制

- ARM64 只支持 ACPI segment 0 的首个、低于 4 GiB 且已映射 ECAM allocation。
- PCI interrupt routing/MSI 尚未完整，多个设备使用轮询。
- driver/device registry 运行时热拔插引用模型未完成。
- VirtIO-SCSI、E1000、VMSVGA、xHCI 当前各使用静态单实例或有限实例存储。
- default display 移除后不会自动提升备用 GPU；input devfs 是聚合节点。

# VirtualBox 公共设备模型

本页是 **VirtualBox 两种架构共用的设备发现与驱动参考**，不是镜像安装手册。
ARM64 与 x86_64 使用不同 CPU、固件入口和平台地址，不能互换镜像。
各自的构建依赖、镜像制作、虚拟机配置、启动与故障定位完整写在
[ARM64 操作手册](virtualbox-aarch64.md) 和 [x86_64 操作手册](virtualbox-x86_64.md)；
选择对应架构的一页即可执行，无需先按本页操作。

## 支持方式

VirtualBox 是标准虚拟设备的提供方。A20OS 通过通用 PCI/VirtIO、block、net、display、
input 驱动访问设备，不为 VirtualBox 创建重复的 AHCI 或 E1000 协议实现。
平台代码只负责固件入口、RAM/MMIO 映射、ACPI 与中断控制器。

| 项目 | ARM64 | x86_64 |
| --- | --- | --- |
| 实例 | `vbox-aarch64` / `vbox-aarch64-text` | `vbox-x86_64` / `vbox-iso-x86_64` / `vbox-disk-x86_64` |
| 固件入口 | UEFI `BOOTAA64.EFI` | UEFI `BOOTX64.EFI`，或 GRUB Multiboot ISO |
| 平台 | `virtualbox-aarch64` | `qemu-virt-x86_64` 的 PC 平台接口 |
| 中断/时钟 | GICv3；协作式软件时钟路径 | APIC/IOAPIC、架构平台时钟 |
| PCI 发现 | UEFI RSDP → ACPI MCFG → ECAM | ACPI MCFG；无可用表时尝试兼容配置空间 |
| 常用存储 | VirtIO-SCSI `1af4:1048` | AHCI `8086:2922/2829` |
| 常用显示 | SVGAv3 `15ad:0406` | VMSVGA `15ad:0405` |
| 常用网络 | E1000 `8086:100e` | E1000 或受支持的 VirtIO net |
| 输入 | xHCI + USB HID | PS/2；其他协议按实际驱动匹配 |

这些是已实现的接口和目标设备，不构成对任意 VirtualBox 版本、宿主或全部设备组合的
运行承诺。构建完成只说明产物生成；每次运行需要在对应架构的虚拟机收集启动和 I/O 证据。
ARM64 的 PCI MSI/INTx 路由仍有限制，若驱动退回轮询，不能把设备可用解释为中断已验收。

## 驱动部署与验收

`generic` 构建依赖镜像中的 `.a20drv` 模块及模块加载路径；`embedded` 将所选驱动链接进
内核。两种架构的操作手册均给出显式 `DRIVER_DEPLOYMENT=embedded` 的设备验收入口，
避免把模块未包含或未加载当成 PCI 发现失败。两者都只匹配驱动 ID 表覆盖的设备。

驱动验收应保存以下连续信息，而不是只记录桌面或 shell 出现：

1. 固件入口、RAM 和 ACPI/PCI 发现结果。
2. 目标 PCI vendor/device、BAR 与驱动绑定。
3. 容量、接口、模式等设备 ready 信息。
4. 对应类消费者：文件系统挂载/读写、网络收发、framebuffer 更新或输入事件。
5. 使用的轮询/IRQ 模式及未验证功能。

没有目标 PCI ID 时先检查虚拟机设备选择；已有 ID 却无驱动绑定时再检查部署方式与
ID 表。增加设备支持时复用标准协议驱动，将宿主/架构差异留在平台和 PCI HAL。

参考：[Oracle VirtualBox 宿主与客户机架构说明](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/Introduction.html)。

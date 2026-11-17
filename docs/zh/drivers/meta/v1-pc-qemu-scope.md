# 1.0 常见 PC 与 QEMU 驱动范围

按项目负责人要求，1.0 的驱动收尾限定为已有支持的常见 PC 和 QEMU 设备，不要求完善所有板级外设。候选启用哪些设备，就验证对应的现有功能与资源安全，不把设备类型、架构和板型做穷尽组合。

## 纳入范围

| 类别 | 现有设备与路径 |
| --- | --- |
| 存储 | VirtIO block/SCSI、NVMe、AHCI、USB mass storage |
| 网络 | VirtIO net、E1000、RTL8139、R8169 |
| 显示 | VirtIO GPU、VMSVGA/SVGA-II、固件 framebuffer 与既有 fbdev 接口 |
| 输入与 USB | xHCI、USB hub/HID、PS/2、VirtIO input |
| 音频 | Intel HDA、VirtIO sound、PC speaker |
| 基础设备 | PC/QEMU 的 RTC、TPM TIS、VirtIO RNG/console，以及已支持的控制台 UART |
| 已接入的 PC 外设 | DesignWare I2C、I2C-HID 与 RTW89 broker 的既有接口；不新增笔记本能力或扩展整机认证 |

PCI、USB、driver core、class、DMA/IOMMU 和显示映射的公共机制，按上述实际使用路径维护其权限、并发和存储寿命。DRM/GPU 新功能、MSI-X 扩展和同源双态仍按既有范围决定退出 1.0。

## 关闭条件

- 候选启用的设备完成其已有正常功能，包括初始化、实际 I/O、退出与关机。
- 对已知的超时、取消、失败 probe 或停止失败缺陷，修复后用直接相关的生产代码检查证明资源不提前释放，必要时支持显式重试。
- 正常运行优先复用已有 QEMU 或 PC 实测记录；按实际覆盖注明设备与配置。无需为所有厂商和板卡逐个构造实体失联、停止超时或复位失败。
- 新改动只做相关验证，完整支持矩阵留给 N18/N20 的正式候选，不重复扩展全量回归或任意故障组合。

## 不再纳入本轮

Hi3861 的 GPIO/I2C/SPI/PWM/ADC、StarFive/Loongson 专用 GMAC、Sunxi/SG2002/RK817 等 SoC 专用音频与开发板存储等路径保持当前支持水平，不再逐项安排完善或实板验收。已经完成的改进保留；取消这些收尾要求不记为新增功能完成，也不自动新增后续版本任务。

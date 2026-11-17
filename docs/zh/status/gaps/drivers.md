# DRIVER 缺口

对应 V1-DRIVER。1.0 收尾已按项目负责人要求限定为[已有支持的常见 PC 与 QEMU 设备](../../drivers/meta/v1-pc-qemu-scope.md)，现状见[驱动状态](../current/drivers-implementation-status.md)。

### 常见 PC/QEMU 设备的剩余正确性问题

| 待完成范围 | 关闭条件 |
| --- | --- |
| 已支持 PC/QEMU 路径的失败清理与资源回收 | 修复尚未关闭的失败 probe、超时/取消、活动调用及停止失败问题，证明受影响存储和绑定按真实硬件停止或复位确认后退休；完成对应设备的既有正常使用与关机路径。使用直接相关的生产代码检查及已有 QEMU/PC 记录，不要求全部厂商和实板的故障认证。 |

通用 request/queue、DMA backing 保留、可失败 stop、关机/kexec 错误传递、GEM 与物理 framebuffer VMA 的软件保留已有实现。已完成的各设备修复与证据统一见[当前状态](../current/drivers-implementation-status.md)，不重复列为待办。

Hi3861 等开发板外设及其它 SoC 专用驱动不再作为本轮逐项完善对象；已经完成的板级改进保留。DRM/GPU、PCI/MSI-X 与同源双态功能强化维持既定范围，见[后续路线](../../roadmap/next-horizon.md#后续版本功能强化)。

VirtIO input 的 DMA 范围、共享 IRQ 与可失败停止软件缺口已关闭，证据与边界见[当前状态](../current/drivers-implementation-status.md#virtio-input-队列范围共享-irq-与可失败停止2026-11-17)，不再作为剩余旧驱动迁移项。

PC speaker 的端口归属、停声读回与失败清理软件缺口已关闭，见[当前状态](../current/drivers-implementation-status.md#pc-speaker-端口归属与可失败停声2026-11-17)，不再作为剩余迁移项。

CMOS RTC 的稳定快照、格式回退与端口归属软件缺口已关闭，见[当前状态](../current/drivers-implementation-status.md#cmos-rtc-稳定读取与端口归属2026-11-17)。

Goldfish RTC 只读探针的 MMIO 边界与临时映射已修复，见[当前状态](../current/drivers-implementation-status.md#goldfish-rtc-只读探针边界2026-11-17)。

用户驱动输入诊断探针的 MMIO 窗口及无副作用退出缺口已关闭，见[当前状态](../current/drivers-implementation-status.md#用户驱动输入探针窗口与退出2026-11-17)。

HDA 生产者暂停后重放旧 ring 数据的软件缺陷已修复，见[当前状态](../current/drivers-implementation-status.md#hda-生产者暂停后的进度回收2026-11-17)；新的候选播放链路验证仍归 N18/N20。

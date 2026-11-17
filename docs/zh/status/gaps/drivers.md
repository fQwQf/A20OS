# DRIVER 缺口

对应 V1-DRIVER。范围限定为[已有支持的常见 PC 与 QEMU 设备](../../drivers/meta/v1-pc-qemu-scope.md)。
已关闭的软件修复及验证边界统一见[驱动状态](../current/drivers-implementation-status.md)。

### 常见 PC/QEMU 冻结候选的集成验收

| 待完成范围 | 关闭条件 |
| --- | --- |
| 同一候选中的驱动正常 I/O 与关机 | 将新 HDA 进度回收，以及近期修改的 VirtIO input、PC speaker 和用户驱动输入探针纳入冻结候选，完成对应既有正常功能、实际 I/O、退出和关机；复用其它设备的已有 QEMU/PC 记录，按候选差异补跑。真实错误继续修复，失败/跳过不计通过。完整支持矩阵与关键并发归 N18/N20，当前状态见[候选记录](../current/v1.0-candidate.md)。 |

Goldfish 与 CMOS RTC 已在本候选实际读取 QEMU 时钟并自然关机，见[候选记录](../current/v1.0-candidate.md)；不重复安排同一读取/关机检查。

已记录关闭的失败 probe、停止失败保留、活动调用取消、DMA/IRQ 回收和显示 backing
不再笼统列作“其它旧驱动尚未迁移”。Hi3861 等板级外设、全部厂商实体故障认证不
属于本轮；DRM/GPU、MSI-X 和同源双态功能强化保持既定[后续范围](../../roadmap/next-horizon.md#后续版本功能强化)。

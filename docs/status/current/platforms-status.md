# 平台当前状态

各板级的当前验证状态与限制。

<!-- 以下内容从 docs/platforms/virtualbox.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 已知限制

- ARM64 只支持 ACPI segment 0 的首个、低于 4 GiB 且已映射 ECAM allocation。
- PCI interrupt routing/MSI 尚未完整，多个设备使用轮询。
- class remove 已有下线与在途调用排空；模块签名、运行时 unload 和完整热拔插流程尚未完成。
- VirtIO-SCSI、E1000、VMSVGA、xHCI 当前各使用静态单实例或有限实例存储。
- default display 移除后不会自动提升备用 GPU；input devfs 是聚合节点。

<!-- 以下内容从 docs/platforms/visionfive2-boot.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 当前状态

- 启动链固件、FIT、四分区 SD 镜像及 FAT32/extra userspace 已完成源码构建；之前的板上镜像已经从 TF 卡启动并进入串口 mksh。每次修改网络驱动后仍需用新镜像重复串口启动记录，不能拿旧镜像日志替代当前提交。
- GMAC1 IRQ 线号 78、DTB 中的 YT8531 PHY 地址 0 和 RGMII RX 300 ps 延迟已由随镜像构建的 VF2 DTB 核对；当前驱动采用轮询数据面。PHY link-up、DMA 收发、`ping` 和公网 `git clone` 必须在接入网线的目标板上按本页 6.8 节逐项确认。

<!-- 以下内容从 docs/platforms/x86_64-pc.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 验证状态

**验证到构建与模拟器；未验证到硬件。** `make check-x86_64-pc-build` 在每次提交上重建本板，`smoke-x86_64`
覆盖启动与两条 bootargs 探测。

已运行并观察到的内容，与仅有论证的内容要分开看：

| 范围 | 状态 | 可复用门禁 |
|---|---|---|
| 构建 | 已验证 | `check-x86_64-pc-build` |
| 启动、命令行、RAM 地图 | QEMU 下已验证 | `smoke-x86_64` |
| 经 MCFG 的 PCI 枚举 | QEMU 下已验证 | `smoke-pci-bridge` |
| 无 ECAM 时的 PCI 枚举 | QEMU 下已验证 | `smoke-pci-i440fx` |
| ACPI S5 关机 | 仅 QEMU 下验证 | QEMU 的 PM1a 处理与硅片不同，见上 |
| INTx `_PRT` 求值 | **从未执行** | QEMU 的 DSDT 极简且拒绝它；q35 走 swizzle |
| 真实硅片上的 INTx 投递 | **未测试** | 没有真机 |
| 多处理器 bring-up | **未测试** | 没有真机 |

最后两行才是真机声明真正需要的。`_PRT` 解释器一次都没有在真实固件上跑过，也没有任何证据表明某个物理
芯片组在这颗内核路由出的线上投递过哪怕一个中断。真机声明需要什么，见 `porting-guide.md` 的验收表。

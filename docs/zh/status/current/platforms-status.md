# 平台当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

各板级的当前验证状态与限制。2026-11-16 范围更新：笔记本新增设备能力与完整实机认证安排到 [后续版本](../../roadmap/next-horizon.md#后续版本功能强化)，已有实测结果和限制保留；不将移出认证范围计作验证通过。

<!-- 以下内容从 docs/zh/platforms/virtualbox.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 已知限制

- ARM64 只支持 ACPI segment 0 的首个、低于 4 GiB 且已映射 ECAM allocation。
- PCI interrupt routing/MSI 尚未完整，多个设备使用轮询。
- class remove 已有下线与在途调用排空；模块签名、运行时 unload 和完整热拔插流程尚未完成。
- VirtIO-SCSI、E1000、VMSVGA、xHCI 当前各使用静态单实例或有限实例存储。
- default display 移除后不会自动提升备用 GPU；input devfs 是聚合节点。

<!-- 以下内容从 docs/zh/platforms/visionfive2-boot.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 当前状态

- 启动链固件、FIT、四分区 SD 镜像及 FAT32/extra userspace 已完成源码构建；之前的板上镜像已经从 TF 卡启动并进入串口 mksh。每次修改网络驱动后仍需用新镜像重复串口启动记录，不能拿旧镜像日志替代当前提交。
- GMAC1 IRQ 线号 78、DTB 中的 YT8531 PHY 地址 0 和 RGMII RX 300 ps 延迟已由随镜像构建的 VF2 DTB 核对；当前驱动采用轮询数据面。PHY link-up、DMA 收发、`ping` 和公网 `git clone` 必须在接入网线的目标板上按本页 6.8 节逐项确认。

<!-- 以下内容从 docs/zh/platforms/x86_64-pc.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 验证状态

**本节通用 PC 配置记录以构建与模拟器为主；笔记本实机结果单列于下节。** `make check-x86_64-pc-build` 在每次提交上重建本板，`smoke-x86_64`覆盖启动与两条 bootargs 探测。

已运行并观察到的内容，与仅有论证的内容要分开看：

| 范围 | 状态 | 可复用门禁 |
|---|---|---|
| 构建 | 已验证 | `check-x86_64-pc-build` |
| 启动、命令行、RAM 地图 | QEMU 下已验证 | `smoke-x86_64` |
| 经 MCFG 的 PCI 枚举 | QEMU 下已验证 | `smoke-pci-bridge` |
| 无 ECAM 时的 PCI 枚举 | QEMU 下已验证 | `smoke-pci-i440fx` |
| ACPI S5 关机 | 仅 QEMU 下验证 | QEMU 的 PM1a 处理与硅片不同，见上 |
| INTx `_PRT` 求值（含桥后总线） | **已实现，QEMU 下仍从未执行** | `-kernel`/multiboot 不提供 ACPI 表；真实固件未跑 |
| i440fx PIRQ 读写与 INTx 投递 | QEMU 下已验证 | `smoke-pirq-i440fx`、`smoke-net-e1000-i440fx-intx` |
| 本节通用 PC 配置的实机 INTx 投递 | **未认证** | 不从 QEMU 结果外推；新增认证后续 |
| 多处理器 bring-up | 笔记本已有 16/16 CPU 记录 | 仅覆盖对应实机配置，见下节 |

真实硬件的 IRQ/PCI 行为按各平台证据单独说明；笔记本的部分实测不能自动认证其他 PC 配置，完整实机认证已安排到后续版本。

### 无 COM1 的笔记本形态

`instances/x86_64-laptop-uefi.toml` 是默认裸机入口：U 盘上同时放 ESP 和"A20OS distro" ext4 分区，内核重新枚举 USB 存储、挂载 world 并启动 XFCE/labwc；`x86_64-laptop-shell.toml` 是文本 shell 排障入口。

[第 19 轮实机记录](../../platforms/x86_64-laptop-handoff.md#第19轮实机检查2026-11-14) 已确认 16/16 CPU、有效 I²C-HID 报告、WLAN 关联/DHCP、连续 ESP 日志及只读 fsck 干净；仍记录 libinput 跳点、Firefox IBus 超时、LibreOffice SIGABRT、udev 辅助进程超时和约 101 秒最终 sync。门户认证与完整桌面认证尚未通过。上述是既有记录，不是本次新增实机复验。

下表中“QEMU/OVMF 已验证”仍不代表真机通过：

| 范围 | 状态 | 可复用门禁 |
|---|---|---|
| UEFI/OVMF 从 USB 盘加载 `BOOTX64.EFI` | QEMU/OVMF 下已验证 | `smoke-laptop-gui`、`smoke-laptop-shell` |
| xHCI 在 ExitBootServices 后重新枚举启动 U 盘 | QEMU/OVMF 下已验证 | `smoke-laptop-gui` |
| GPT "A20OS distro" 分区挂载并 chroot | QEMU/OVMF 下已验证 | `smoke-laptop-gui` |
| labwc 经 simplefb `/dev/dri/card0` 软件渲染出桌面 | QEMU/OVMF 下已验证 | `smoke-laptop-gui` 的 screendump 多色判定 |
| ACPI MADT AP bring-up（4 vCPU） | QEMU/OVMF 下已验证 | `smoke-laptop-gui` 的 `[SMP] 4/4` |
| 高于 4 GiB 的 PCI BAR 动态映射 + NVMe identify | QEMU/OVMF 下已验证 | `smoke-laptop-shell` |
| PS/2 键盘到达 shell | QEMU/OVMF 下已验证 | `smoke-laptop-shell` 的 `echo ok` |
| 内置 ESP `A20LOG/` 持久化（真机） | 真机已验证 | `/boot/efi/A20LOG`，451+ 条 JSONL，`state=ready` |
| xHCI USB 键盘到达 shell | QEMU/OVMF 下已验证 | `check_laptop_uefi.py --usb-keyboard` 禁用 i8042 |
| hub 下游 USB 键盘与桌面启动盘 | QEMU/OVMF 下已验证 | `check_laptop_uefi.py --usb-hub`、`check_laptop_gui.py --storage usb-hub` |
| 内置键盘 / GUI（真机）及 hub 下游 USB | 已有键盘/桌面记录；完整认证后续 | 按第 19 轮记录保留实际范围，不能据旧版完成码继续概括为“未运行”；hub/具体拓扑单独验收 |
| NVMe 持续数据面 + 内置盘桌面 | QEMU/OVMF 下已验证 | `nvstress` 约 96 MiB 无 panic；`smoke-laptop-shell` 回归步骤；`check_laptop_gui.py --storage nvme` 桌面截图 |
| 无专用日志分区时内置 ESP `A20LOG/` 断电持久化 | QEMU/OVMF 下已验证 | `smoke-laptop-esp-log`（重命名日志分区并强制 `SIGKILL` 后读回 JSONL） |
| I2C-HID 触摸板 / RTL8168 有线网卡 | 触摸板已有有效报告，仍有 libinput 跳点；有线网卡单独验收 | 第 19 轮 I²C 传输错误/丢弃为零不等于输入端到端无异常 |
| 真实笔记本的 GOP/MCFG/IOMMU/键盘总线 | 已有观测与部分实机运行；完整认证后续 | 见 `platforms/x86_64-laptop-handoff.md` |


## ARM32 QEMU 用户态

C/Rust 内存接口统一采用原生地址和计数宽度，异常处理进入任务自己的内核栈后允许调度；trap 帧保存 `d0-d15`/FPSCR，调度帧保存 `d8-d15`/FPSCR，信号附加帧保存/恢复 VFP。

QEMU virt、64 MiB、`ABI=both`、RAMFS 连续 10 轮，每轮 40 次顺序 fork/exec，共 400 次通过；覆盖父子全部 16 个 VFP 寄存器隔离、抢占、空闲唤醒、文件读写与正常关机，无 SIGSEGV、内核异常或内存耗尽。

回归入口：以 `ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both BRINGUP=0 RAMFS_USER=1 RAMFS_FPU_TESTS=1 DRIVER_DEPLOYMENT=embedded` 构建，执行 `python3 tools/qemu/check_serial_shell.py --arch arm32 --kernel <kernel.elf> --memory-mb 64 --fpu --fp-signals --timers --stress-forks 40`。

2026-11-16 补齐异步信号验收：`arm_fp_signal_test` 在真实用户进程中连续完成 1000 次 SIGUSR1，核对全部 `d0-d15` 与 FPSCR；裸汇编处理器故意覆盖全部寄存器并两次让出 CPU，返回后仍恢复原值。该轮同时通过定时器抢占、空闲唤醒、40 次 fork/exec、文件操作及正常关机，无内核异常。独立负控禁用信号帧的 VFP 恢复后，第一轮信号即报告 d0 与 FPSCR 不匹配，被相同门禁拒绝；恢复源码、重建后再次通过。CORE-07 已关闭。

本轮还修复 ARM `_llseek` 的用户结果指针/零返回契约及负偏移组合，避免 BusyBox 写历史记录时退出；备用裸机 GCC 显式使用 32 位枚举，物理地址范围与 r8169 页内偏移保留正确宽度。最终正例内核 SHA-256 为 `953600f20d5e94f5da94dd8ac6d9ff1b5fa8d90078e197124d04f43674de2ba9`，证据在 `build/evidence/v1-closeout-20261116/arm-fp-signal-final*`，负控在 `arm-fp-signal-negative*`。

该记录仅覆盖上述 QEMU 配置；V3s 板级 UART、DDR、SD、真实中断时序与长期负载尚待独立验收。

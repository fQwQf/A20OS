# 驱动实现符合性与限制

> 不要这样做：不要恢复已清理的双初始化入口；不要扩大本页记录的已知边界却不同时更新说明和文档；不要把“符合”当成“所有平台所有配置都成立”。

本页记录当前源码事实和可依赖的运行边界。审查覆盖 driver core、公共总线、STM32F103、VirtualBox ARM64/x86_64 及其设备驱动。

## 如何阅读矩阵

每行矩阵的格式是：范围、状态、说明。

- **符合**：源码和文档一致，当前可以依赖。
- **已扩展/基础可用**：已实现，但有明确限制（限制写在说明栏）。
- **有条件**：只在特定平台或特定序列化假设下工作，SMP/IRQ 化前要先加锁。
- **动态类视图**：char/block/audio 自动发布 devfs 节点，所有 class 发布 sysfs 目录；display/input 的旧聚合节点仍保留。

## 如何更新矩阵

1. 先在目标平台或 QEMU 或 VirtualBox 上复现实验，记录 commit、构建命令和日志。
2. 如果是新增行，说明里写清平台、版本、观察到的运行边界和失败行为。
3. 如果改动了现有项的状态或限制，必须同步更新相关平台文档和 [drivers/testing-and-submission.md](testing-and-submission.md) 中的测试矩阵。
4. 不要只改状态不改说明。新改动不得扩大已知边界，除非文档里已经解释了原因。

QEMU x86_64 与 RISC-V64 GUI 的 VirtIO GPU、键盘和鼠标分别由 `make smoke-qemu-gui-{x86_64,riscv64,aarch64,arm32,loongarch64}` 做行为验证；测试检查非空 scanout，并向客户机注入真实按键事件。修改 driver core、IRQ、DMA、PCI/VirtIO transport、display、input、framebuffer 或 devfs 聚合路径时，该项是必跑回归测试。门禁入口见 [testing/testing-gates.md](../testing/testing-gates.md)。

## 核心与公共基础设施

| 范围 | 状态 | 说明 |
|---|---|---|
| driver core | 符合 | 动态 registry；bus ID 后可执行无副作用的 driver protocol match；操作 mutex 串行化注册、probe/remove 与遍历；数组 spinlock 不跨回调；probe 失败完整解绑 |
| PCI bus | 已扩展 | ECAM、ID/subsystem match、零值未分配 BAR sizing/assignment、BAR 编号查询、modern VirtIO capabilities；已设置 `matched_id` |
| VirtIO-MMIO bus | 符合 | MMIO/IRQ 资源、type match、`matched_id`；静态最多 8 个设备 |
| hwapi MMIO | 符合 | 宽度访问和 barrier；relaxed 版本需谨慎 |
| hwapi IRQ | 明确边界 | 固定 256 线、每线一个 handler；表操作受锁保护，重复注册返回 `-EBUSY`；`free_irq` 校验 owner token 并等待在途 handler |
| hwapi DMA | 基础可用 | coherent helper 返回物理 handle，sync 委托 arch cache hook；尚无 DMA mask/IOMMU/大对齐能力 |
| class publication | 已扩展 | probe 成功自动建立带引用的 class device；remove 前先下线并排空在途调用 |
| devfs/sysfs | 动态类视图 | char/block/audio 自动生成 `/dev/charN`、`diskN`、`audioN`；所有 class 动态暴露于 `/sys/class`；旧 display/input 聚合节点暂时保留 |
| drvmod loader | 基础可用 | 四架构（riscv64/x86_64/aarch64/loongarch64）ELF `ET_REL`、框架符号白名单、全量 ELF 边界校验（section/symbol/strtab/relocation）、按 `e_machine` 选择重定位解码器（RISC-V CALL、x86-64 large 模型 ABS64、AArch64 CALL26+veneer、LoongArch CALL36/pcalau12i）、自动 match→probe 绑定、统一驱动核心桥接（模块可注册标准 `driver_t`）、drv_env 双驻留共享代码的模块后端（DRV_ENV_DRVMOD）；goldfish RTC（riscv64/aarch64/loongarch64）、PC speaker（x86_64）、virtio-input 内核探针（riscv64/aarch64/loongarch64）已迁移为模块；签名、运行时 unload 尚未完成 |

## drvmod 迁移矩阵

| 驱动 | 迁移状态 | 说明 |
|---|---|---|
| goldfish RTC | 已迁移 | 内建 `goldfish_rtc_kdrv.c` 已删除；`rtc.drv` 在 riscv64/aarch64/loongarch64 QEMU virt 上由 `smoke-drvmod-*` 验证绑定 |
| PC speaker | 已迁移 | 内建 `pc_speaker.c` 已删除；`pc-spkr.drv`（x86_64）经统一驱动核心桥接注册 AUDIO 类并绑定 platform 设备 |
| UART | 不可迁移 | 启动早期串口，模块加载前必须可用 |
| virtio-blk | 不可迁移 | 根文件系统挂载依赖它，模块自身从该文件系统读取 |
| virtio-net、virtio-gpu、USB core、PCI bus、virtio-mmio bus | 不可迁移 | 初始化顺序在 `init_kthread`（模块加载点）之前 |
| NVMe、AHCI、HDA、E1000、virtio-scsi、vmsvga、virtio-gpu | 待框架扩展 | 需要 DMA/PCI BAR/class ops 框架 API |
| virtio-input 内核探针 | 已迁移 | 内建 `virtio_input_kprobe.c` 已删除；`vinput-probe.drv` 用 drv_env 模块后端复用双驻留共享协议，`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| virtio-input 完整事件投递 | 待框架扩展 | 需要 virtq 进入共享层并作为框架 API |
| PS/2、TPM、GMAC、SDIO | 待框架扩展 | PS/2 耦合 irqchip 路由与 devfs/proc；TPM 依赖 ACPI；GMAC/SDIO 为单实例轮询 |

迁移细节与顺序见 [kernel-modules.md](kernel-modules.md)。

## 设备驱动

| 驱动 | 类 | 状态与限制 |
|---|---|---|
| virtio-blk | BLOCK | 类接口；remove 停止设备并释放已注册 IRQ；有限静态实例 |
| virtio-net | NET | 多实例、IRQ/轮询和类接口；remove 释放 IRQ 并复位 transport |
| virtio-input | INPUT | 类接口、PCI/MMIO、多实例槽和 remove；由 `/dev/event0` 聚合 |
| virtio-gpu | DISPLAY | class registry、framebuffer 页释放和 transport reset；单实例、同步 controlq |
| VirtIO-SCSI | BLOCK | VirtualBox ARM 已验证，remove 复位/释放槽；只支持 target/LUN 0、READ/WRITE(10)、512B sector、轮询 |
| AHCI | BLOCK | VirtualBox x86_64；单 controller/单 port/单 slot、LBA48、轮询；probe 回滚和 remove 释放 DMA/IRQ |
| NVMe | BLOCK | 架构无关 PCI class 驱动；x86_64 与 LoongArch64 构建，LoongArch QEMU 已验证 BAR、CAP、admin/I/O queue、Identify，以及跨 8 KiB bounce chunk 的写入/flush/读回比较；要求 NVM command set 和兼容 4 KiB memory page，首个活动 namespace、轮询、每 controller 只发布一个 namespace |
| E1000 | NET | VirtualBox 82540EM，轮询 ring，已加 stop/remove；静态单实例 |
| VMSVGA/SVGAv3 | DISPLAY | VirtualBox x86_64/ARM，BAR offset/pitch 边界和 class registry，已加 remove；静态单实例 |
| xHCI HID | INPUT | VBox ARM keyboard/mouse/tablet，class、轮询和 controller stop/remove；只匹配 `8086:1e31`，静态单 controller |
| USB Storage (BOT) | BLOCK | `kernel/drivers/usb/class/usb_storage.c`：xHCI bulk 传输 + Bulk-Only Transport（CBW/CSW）+ SCSI READ(10)/WRITE(10)/READ_CAPACITY(10)。QEMU x86_64 `qemu-xhci + usb-storage` 已验证，挂载为 `/dev/diskN` 并可直接 mount FAT32。只支持单 LUN、512/2048/4096 扇区、每命令 4 KiB 数据块 |
| TPM 2.0 (TIS) | x86 安全 | `kernel/drivers/security/tpm.c`：ACPI TPM2 表发现 + TIS FIFO 状态机 + Startup/GetRandom。现代 x86_64 固件常见；无 swtpm 时未做运行时验证 |
| PS/2 | x86 板级服务 | 提供基础键鼠控制器服务；可复用输入设备使用 `DEV_CLASS_INPUT` |
| PC Speaker | AUDIO | drvmod 模块（`pc-spkr.drv`，x86_64），动态 `/dev/audioN`；支持 19 Hz–20 kHz 有界 tone/stop ABI，不冒充 PCM |
| Intel HDA | AUDIO | 架构无关 PCI class 驱动；x86_64 与 LoongArch64 QEMU 已完成 BDL DMA smoke，x86_64 已完成用户态 tone 到 QEMU WAV 验证，RISC-V64 已完成完整 Wayland/FFmpeg/PulseAudio 播放；三个 `run-gui` 目标连接宿主音频；支持 48 kHz 双声道 S16_LE、环形 DMA、stop/drain、完整 remove 和用户态 WAV/raw/tone 播放器 |
| STM32 SDIO | BLOCK | 统一类 + MCU bridge；板级 bus 仍用名称匹配 |
| STM32 简单外设 | 允许例外 | 板级轮询轻量 API，不强制统一对象；扩展到多实例/用户 ABI 时必须迁移 |
| StarFive/LS2K GMAC、DW SDIO | 有条件 | 单实例轮询并依赖外部串行化，SMP/IRQ 化前必须增加实例锁 |

## VirtualBox 平台

VirtualBox ARM64 源码位于 `kernel/platform/virtualbox-aarch64/`，通过 UEFI ACPI RSDP/MCFG 枚举 PCI。VirtIO-SCSI、E1000、SVGAv3、xHCI HID 已进入统一类模型。平台 GIC disable 已实现；generic timer trap 和 PCI interrupt routing 仍使数据面主要轮询。运行配置见 [VirtualBox ARM64 运行手册](../platforms/virtualbox-aarch64.md) 与 [VirtualBox x86_64 运行手册](../platforms/virtualbox-x86_64.md)，驱动架构见 [VirtualBox 驱动栈](../platforms/virtualbox.md)。

VirtualBox x86_64 复用 x86_64 平台 PCI、AHCI、VMSVGA、PS/2/E1000/VirtIO 驱动，以 GRUB ISO 启动。

## 跨架构 PCI 协议验证

NVMe/HDA 源码不含 CPU 架构门禁，它们的运行条件来自下层 PCI/MMIO/DMA 能力。`make smoke-pci-portability` 在 QEMU LoongArch64 virt 上联合挂载 `intel-hda`、`hda-duplex` 和 NVMe，验证零值 BAR sizing/assignment、HDA codec topology、PCM BDL DMA，以及 NVMe queue/Identify 和 17 个扇区的写入、flush、读回比较。该结果证明协议驱动并非 x86 专用，但不代表所有已构建架构都具备 PCI host。

QEMU RISC-V64 board 已提供 ECAM、PCI MMIO BAR 窗口和实际 HDA PCM DMA；QEMU AArch64 board 当前仍只枚举 VirtIO-MMIO。VirtualBox AArch64 有 ACPI MCFG PCI 枚举，但 BAR 依赖固件预分配且目前没有 HDA/NVMe 运行日志。状态表必须继续按“构建”“绑定”“实际 I/O”三个等级记录证据。

## 强制边界

- 从 `kernel_main` 直接调用硬件 init，而不是 `DRIVER_REGISTER` 加枚举。
- 只暴露 `*_get_dev()` 全局 getter，不实现 class ops。
- 设备发现和功能发布使用 class；块层适配对象不能代替驱动注册。
- probe 获得 DMA/IRQ 后直接 `return -1` 而不回滚。
- 空 remove，即使驱动启用了 DMA、IRQ 或类级全局 registry。
- 在 spinlock 下执行秒级 busy poll。
- 在具体驱动中添加静态 devfs vnode，而不是通用类适配器。

新改动不得扩大表中边界。依赖轮询、固定实例数或固定节点时，提交必须写明适用平台、并发假设和失败行为。

已清理的兼容债务：`kernel_main` 不再直调 VirtIO GPU/input 初始化；block mount 和 network init 不再启动架构私有 VirtIO PCI 扫描。QEMU x86_64 现在只有统一 PCI bus 拥有 BAR 与 transport，VirtIO-MMIO 通过其 bus device ID 匹配 virtio-blk。不得恢复这些双初始化入口。

> 注意：状态矩阵描述的是当前代码事实，不是未来计划。任何“扩大限制”或“降低状态”的改动都要同步更新平台文档和提交清单。

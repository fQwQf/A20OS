# VirtualBox x86_64 运行与验收手册

> 不要这样做：不要同时变更多个设备控制器。先用上表中的已知组合拿到基线，再一次只替换一个目标设备，否则无法判断失败来自启动、总线还是功能驱动。

这份手册说明如何在 x86_64 VirtualBox 中配置 A20OS 并逐项验证驱动。它是 [VirtualBox 驱动栈](virtualbox.md) 的运行入口；通用驱动接口和提交流程见 [构建、测试与提交](../drivers/testing-and-submission.md)。

## 支持矩阵

| 功能 | VirtualBox 设备 | A20OS 实现 | 当前限制 |
|---|---|---|---|
| 启动 | BIOS + GRUB Multiboot | `make vbox-iso-x86_64` | ISO 启动，非 ARM UEFI 镜像 |
| 显示 | VMSVGA `15ad:0405` | `gpu/vmsvga.c`、`/dev/fb0` | 固定 1024x768x32，2D framebuffer |
| 磁盘 | Intel AHCI `8086:2922/2829` | `block/ahci.c` | 首个可用 port、LBA48、512B sector、单 slot |
| 网络 | E1000 82540EM `8086:100e` | `net/e1000.c` + lwIP | 静态单实例、轮询 ring |
| 网络 | VirtIO network | `net/virtio_net.c` | 设备类型/transport 必须与当前 ID 表匹配 |
| 输入 | PS/2 键盘鼠标 | `drvmod` 模块 `ps2.drv` | x86 板级控制器服务 |
| 输入 | 可选 VirtIO input | `input/virtio_input.c` | 通过 `/dev/event0` 聚合 |

E1000 使用 `DEV_CLASS_NET`，通用输入节点是 `/dev/event0`。PS/2 只提供 x86 平台基础输入；新增可复用输入设备使用 `DEV_CLASS_INPUT`。

## 构建 ISO

Debian/Ubuntu 安装依赖：

```sh
sudo apt-get install gcc binutils grub-common grub-pc-bin xorriso mtools
```

构建：

```sh
make ARCH=x86_64 ABI=both kernel-only -j4make vbox-iso-x86_64
```

第二条命令会递归用 `ARCH=x86_64 ABI=both BRINGUP=0` 执行完整 `dev-build`，再由 `tools/mk_grub_iso.sh` 生成：

```text
.kernel-build/x86_64-qemu-virt-x86_64-both-dev/a20os-x86_64.iso
```

如果路径因自定义构建变量变化，以 Make 输出的 `BUILD_DIR` 为准。`grub-mkrescue not found` 表示宿主缺少 GRUB 工具，不是内核或驱动编译失败。

## 创建虚拟机

在 VM 关机状态完成以下配置：

1. 类型选 `Other/Unknown (64-bit)` 或 `Other Linux 64-bit`，内存至少 512 MiB，建议 1 GiB。
2. 保留传统 BIOS/GRUB 启动；把生成的 ISO 连接到光驱并置于首次启动顺序。
3. 显示控制器必须选 `VMSVGA`，显存至少 16 MiB；不需要 Guest Additions。
4. 添加 SATA/AHCI 控制器，把可丢弃的测试磁盘连接到 port 0。
5. 网络首选 `Intel PRO/1000 MT Desktop (82540EM)`，便于验证 E1000；测试 VirtIO 驱动时再显式改为 VirtIO。
6. 基础输入保留 PS/2 键盘/鼠标；验证 input class 时使用驱动明确支持的 VirtIO/USB 设备。
7. 启用串口日志或保留完整 VM 控制台输出；驱动验收需要从 PCI 枚举到类消费者的连续日志。

> 注意：每次修改后必须重新生成 ISO，并确认 VM 光驱挂载的是新 ISO。VirtualBox 可能缓存旧介质 UUID，导致“改了代码行为没变”。

## 显示验证

VirtualBox 的 VMSVGA 向 guest 暴露 VMware SVGA II PCI 设备 `15ad:0405`。成功路径应依次出现：

```text
[BUS] pci ... id=15ad:0405 ...[GPU] ... ready ...[DRIVER] device ... bound to driver 'vmsvga'
```

用户态随后通过 `/dev/fb0` 获取模式和映射 framebuffer。黑屏时先检查 PCI 枚举和驱动 ready 日志，再检查 VMSVGA 选择；不要只通过桌面是否启动判断 probe。VBoxVGA/VBoxSVGA 是不同协议，当前驱动不保证支持。

驱动支持的边界、BAR 选择、可见 backing 和 flush 语义见 [Display/Framebuffer](../drivers/display.md)。

## 磁盘准备与 AHCI 验证

完整 `dev-build` 会在同一构建目录生成 `fat32.img` 和 `ext4.img`。先复制一份测试镜像，避免对唯一数据做写测试。然后转换成新 VDI：

```sh
VBoxManage convertfromraw \.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img \a20os-fat32-test.vdi --format VDI

VBoxManage storageattach "A20OS" --storagectl "SATA" \--port 0 --device 0 --type hdd --medium a20os-fat32-test.vdi
```

控制器名可能不是 `SATA`；用以下命令查实际名称后替换：

```sh
VBoxManage showvminfo "A20OS" --machinereadable
```

成功日志至少包含 AHCI PCI ID、BAR、controller/port、容量和挂载结果。当前驱动只管理第一个可用 SATA port，使用 512 字节扇区和 LBA48 ATA 命令；未承诺 ATAPI/光驱、NCQ、多 slot 或热拔插。块驱动功能测试必须覆盖首尾 LBA、越界、跨 128-sector 内部分块、读回和 flush/错误恢复。

若 ISO 能启动但 `/bin/init` 不存在，按以下顺序定位：

1. 是否出现目标 AHCI PCI ID；没有则检查 VM 控制器类型。
2. 是否出现 `[AHCI] controller found`；没有则检查 BAR enable/assignment。
3. 是否出现 port 和容量；没有则检查磁盘确实连接在首个控制器/可用 port。
4. 容量存在但挂载失败时再检查镜像文件系统，不要改 AHCI probe 掩盖上层错误。

## 网络验证

### E1000

VM 网络适配器选择 `Intel PRO/1000 MT Desktop (82540EM)`。A20OS 应枚举 `8086:100e`，E1000 probe 打印 MAC 和 link，随后 lwIP 按 `DEV_CLASS_NET` 自动挂接。该类没有 `/dev` 节点。

最小证据：

```text
[BUS] pci ... id=8086:100e ...[E1000] ready: mac=... link=up[LWIP] ... attached ...
```

只有 E1000 ready、没有 lwIP attach 时检查 class 注册和网络配置；没有 E1000 ready 时检查 PCI ID/BAR。网络测试至少包括收发、无包 poll、ring wrap、队列满和最大帧。当前 E1000 只实现 82540EM，不能用宽泛 Intel 网卡 ID 表假定其他型号兼容。

### VirtIO network

把 adapter type 改为 VirtIO 后重新启动，记录 VirtualBox 实际提供的 PCI ID 和 VirtIO capability。当前驱动要求其 ID 表、modern/legacy transport 和协商 feature 全部匹配。出现 `incomplete capabilities` 时检查 VirtualBox 控制器模式；出现 feature rejection 时检查驱动是否接受了未实现能力。详细队列规则见 [PCI 与 VirtIO](../drivers/pci-and-virtio.md)。

## 输入验证

PS/2 键盘鼠标用于确认 VM 基础交互，但不能证明目标 input class 驱动有效。验证 input 驱动时必须看到对应设备绑定，并从 `/dev/event0` 读取完整 `struct input_event`：按键按下/释放、鼠标移动/按键以及每批 `EV_SYN/SYN_REPORT` 都应完整。

`/dev/event0` 是聚合节点，不保证一个节点对应一个硬件。为了证明目标驱动在工作，可暂时从 VM 移除其他输入设备或利用驱动身份日志，不要只凭“鼠标能动”作结论。

## 驱动开发时的复现流程

1. 保存基线 VM 配置、VirtualBox 版本、宿主架构和一次成功串口日志。
2. 从 `[BUS] pci` 行记录 vendor/device、subsystem、class 和每个 BAR。
3. 将功能驱动放在通用 class 目录，使用 `pci_bus` 和 ID 表，不增加 VirtualBox 板名条件。
4. 先构建 `kernel-only`，再制作 ISO；每次确认 VM 挂载的是新 ISO/磁盘。
5. 分别保留“设备不存在”“设备存在但 ID 不匹配”“成功 probe”“成功 I/O”的日志。
6. 更新 [VirtualBox 驱动栈](virtualbox.md) 的矩阵和 [实现状态](../drivers/implementation-status.md) 中的限制。

## 常见故障

| 现象 | 定位 |
|---|---|
| `grub-mkrescue not found` | 安装 GRUB/xorriso/mtools，未进入 guest 驱动阶段 |
| GRUB 后黑屏 | 先查日志；显示控制器必须为 VMSVGA `15ad:0405` |
| 没有目标 PCI 行 | VM 未暴露设备或 PCI host/枚举问题 |
| 有 PCI 行但未绑定 | ID/subsystem、`.bus`、`.driver_init` 或 probe 返回错误 |
| `Cannot open /bin/init` | 按 AHCI 容量与挂载分界定位控制器、磁盘或文件系统 |
| E1000 ready 但网络不可用 | 检查 lwIP attach、IP 配置和 VirtualBox NAT/桥接，而非重复 probe |
| 有输入但目标 driver 无日志 | 很可能只有 PS/2 板级输入，不能视为 class 驱动验证 |
| 修改后行为不变 | 确认重新生成 ISO，VM 光驱挂载路径和文件时间/hash |

## 验收记录模板

```text
VirtualBox version:Host architecture:A20OS commit/diff:Build command:ISO path and checksum:VM graphics/storage/network/input configuration:Enumerated PCI ID and BARs:Bound driver and ready line:Class consumer line:I/O performed and result:Known untested features:
```

完整提交清单和跨平台构建矩阵见 [构建、测试与提交](../drivers/testing-and-submission.md)。

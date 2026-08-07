# VirtualBox ARM64 运行与验收手册

> 不要这样做：不要只以“出现桌面”或“shell 启动”作为驱动验收证据。必须保留从 `[BUS] pci` 到类消费者 mount、lwIP、framebuffer、input 的连续日志。

这份手册说明如何在 VirtualBox ARM64 上制作镜像、配置虚拟机并收集从 ACPI/PCI 到类消费者的完整证据。通用驱动接口和平台规范见 [VirtualBox 驱动栈](virtualbox.md)，驱动开发流程见 [构建、测试与提交](../drivers/meta/testing-and-submission.md)。

## 当前支持状态

| 组件 | 状态 | 说明 |
|---|---|---|
| ARM64 UEFI boot disk | 已实现 | `make vbox-image-aarch64` 生成带 `EFI/BOOT/BOOTAA64.EFI` 的 FAT 磁盘。 |
| UEFI loader | 已实现 | 把 flat kernel 装载到 `0x08080000`，退出 boot services，关闭 firmware MMU，进入 A20OS。 |
| Early page tables | 已实现 | 把 VirtualBox RAM 映射到 `0x08000000`，高地址平台 MMIO 映射为 device memory。 |
| Serial console | 已实现，需硬件验证 | PL011 配置在实测地址 `0xFFDDF000`。 |
| GIC | 已实现，需硬件验证 | GICv3 distributor 和 redistributor 支持，替换掉原来无效的 GICv2 CPU interface 代码。 |
| Timer | 软件 fallback | VirtualBox 在 EL1 trap 两个 ARM generic timer 接口；早期启动使用非抢占软件计数器。 |
| ACPI/PCIe discovery | 已实现 | UEFI loader 传递 ACPI RSDP；board 解析 MCFG 并枚举 PCIe ECAM。 |
| VirtIO-SCSI disk | 已目标验证 | VBox `1af4:1048` 控制器发现 boot LUN，挂载 GPT FAT32 partition 的 `/bin`。 |
| Network | 已目标检测，持续验证 | VBox 暴露 Intel E1000 `8086:100e`；驱动已集成 lwIP 和 DHCP。 |
| VMSVGA display | 已目标验证，桌面验证中 | SVGAv3 `15ad:0406` 在 VBox ARM 上被检测到；驱动使用设备报告的 framebuffer offset 和 pitch，VRAM 映射为 Device memory。 |
| USB HID input | 已实现，需目标验证 | Intel `8086:1e31` xHCI controller 轮询；USB 键盘、鼠标、Tablet 事件进入 `/dev/event0`。 |
| Serial recovery console | 已实现 | 磁盘或图形路径失败时仍可用。 |
| Userspace and remote shell | 已实现 | MMU userspace 到达 musl `init`/`mksh`；`telnetd` 监听 TCP 2323。启动会打印 `/init` 大小、ELF entry 和 FNV-1a hash，用于识别 stale disk。 |

UEFI loader、PCI discovery、VirtIO-SCSI 和 SVGAv3 probe 已在 VirtualBox ARM64 上跑通。同一个 AArch64 MMU userspace 镜像也在 QEMU 上通过 `init`、`fork`/`exec` 和交互式 `mksh` 做回归测试。

## 构建镜像

安装 AArch64 交叉编译器和 `mtools`。Debian/Ubuntu：

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu mtools partedmake vbox-image-aarch64
```

输出图形镜像：

```text
.kernel-build/aarch64-virtualbox-aarch64-both-dev/a20os-vbox-aarch64.img
```

这是 256 MiB 的 GPT 磁盘，FAT32 EFI System Partition 里同时包含 `EFI/BOOT/BOOTAA64.EFI` 和 A20OS `/bin` root filesystem。它不是 x86 GRUB ISO。

## 在 VirtualBox 中运行

需要 VirtualBox 7.2 或更新版本，运行在 ARM64 宿主上。创建至少 1 GiB RAM、启用 EFI 的 ARM64 VM，把生成的镜像作为第一块硬盘挂上去。不要启用 Secure Boot。

为能同时输入和输出，把 UART1 配置成 TCP server，而不是文件。文件模式只能记录输出，不能接收键盘输入。VM 关机后，在 Windows PowerShell 里执行：

```powershell
$vm = "A20"& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm $vm `--uart1 0x03f8 4 `--uartmode1 tcpserver 5555
```

启动 VM，然后用 `telnet 127.0.0.1 5555` 或 PuTTY Raw/TCP 连接。第一次启动保持串口连接：日志会报告 ACPI MCFG window、PCI 设备、VirtIO-SCSI 容量和文件系统挂载结果。

### 转换并附加新磁盘

更新镜像后，必须重新生成并替换 VM 挂载的 VDI。直接转换旧 raw 镜像不会得到新的 GPT partition。不要对已经挂载或正在使用的 VDI 执行 `convertfromraw`；每次用新的输出文件名，避免 VirtualBox 保留旧的 medium UUID：

```powershell
$raw = "C:\Users\super\Downloads\a20os-vbox-aarch64.img"$vdi = "C:\Users\super\Downloads\a20os-vbox-aarch64-20260717.vdi"& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" convertfromraw `$raw $vdi --format VDI
```

> 注意：每次生成新镜像后都要用新的输出文件名执行 `convertfromraw`。不要对当前已经挂载到 VM 的 VDI 再次转换，也不要让 VirtualBox 继续保留旧的 medium UUID。

构建还会生成 `a20os-vbox-aarch64.img.sha256` 并在 ESP 中写入 `A20OS.MANIFEST`。如果镜像是当前构建的，串口日志中的 `[INIT] image` 行和 `file-entry` 必须与刚构建的 `/init` 匹配；出现旧构建的 entry 说明 VM 还挂在一个 stale VDI 上。

VirtualBox ARM 默认使用 VirtIO-SCSI 控制器。除非你的 VirtualBox 版本明确要求其他控制器，否则保持默认。

## 网络与远程 shell

保持 VM 的 Intel PRO/1000 MT Desktop 适配器接在 NAT 上，并转发宿主端口到客户机 TCP 2323。VM 关机时执行：

```powershell
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm $vm `--nic1 nat `--nictype1 82540EM `--natpf1 "a20-telnet,tcp,127.0.0.1,2323,,2323"
```

串口出现 `[telnetd] listening on port 2323` 后，用 `telnet 127.0.0.1 2323` 或 PuTTY Raw/TCP 连接。该服务无认证，因此只转发到 loopback。

## GUI 与文本镜像

显式构建图形镜像：

```bash
make vbox-gui-image-aarch64
```

输出：

```text
.kernel-build/aarch64-virtualbox-aarch64-both-dev/a20os-vbox-aarch64-gui.img
```

两个图形目标都包含 `/etc/a20-gui` 标记，并保留 UART1 串口 shell。文本-only 恢复盘用：

```bash
make vbox-text-image-aarch64
```

输出：

```text
.kernel-build/aarch64-virtualbox-aarch64-both-dev/a20os-vbox-aarch64-text.img
```

保持 VirtualBox 显示控制器选择 VMSVGA。SVGAv3 使用显式 update command，成功的图形 probe 会在串口报告 `[GPU] SVGAv3 ready`。红、绿、蓝、白四条 bar 是驱动 scanout 自测，不是 desktop。它们会在用户态报告以下全部信息后被替换：

```text
[init] desktop queued: pid=2[desktop] entered mainFramebuffer mapped: va=0x30000000 size=3145728 stride=4096[desktop] framebuffer readyMission Control initialized, entering loop...
```

图形镜像用普通 `fork`/`exec` 启动 desktop。它不会用 `vfork` 挂起 PID 1，因为 VirtualBox ARM timer fallback 是协作式的。发布新子进程后，协作 clone 路径会主动 yield 一次，让子进程无需等待 VirtualBox ARM 不提供的中断即可进入 `exec`。

镜像目标在打包前还会把 staged `/init` 与当前 MMU user build 做比较。这能拦截被打断/增量构建留在 FAT 里的 NOMMU `init`：那种二进制会把 `fork()` 改成 `vfork()`，日志里表现为 `clone begin: ... flags=0x4111`，然后 PID 1 永远等一个还没被调度到的子进程。当前图形 MMU 镜像报告 `flags=0x11`，随后是 `desktop queued` 和 `[desktop] entered main`。

## 附加磁盘到控制器

用 `VBoxManage` 把磁盘挂到 ARM VM 选中的控制器。VirtualBox 7.2 通常创建 `VirtioSCSI` 控制器，Windows PowerShell 示例：

```powershell
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" storageattach "A20OS ARM64" `--storagectl "VirtioSCSI" `--port 0 --device 0 --type hdd `--medium "C:\Users\super\Downloads\a20os-vbox-aarch64-gpt.vdi"
```

如果控制器名不同，用下面命令查看实际名称并替换：

```powershell
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" showvminfo "A20OS ARM64" --machinereadable
```

firmware 只需要看到 EFI System Partition 和标准的 `EFI/BOOT/BOOTAA64.EFI` 路径。

## 第一阶段预期输出

```text
A20OS: loading kernel
======================================
    A20OS Kernel
======================================
```

如果看到 UEFI 消息但之后没有 kernel 串口输出，firmware handoff 已经成功了，剩下的问题通常是 VirtualBox PL011 地址或串口配置。如果连 UEFI 消息都没有，检查 EFI 是否启用、Secure Boot 是否关闭、镜像是否排在启动顺序第一。

## 驱动验收时需要保留的证据

PCI transport 当前使用轮询；在 ACPI interrupt controller 解析完成前，这能让 storage、display 和 xHCI HID 不依赖 VirtualBox 专用 MSI/INTx 路由。PCI enumerator 会打印每个设备及其 BAR；验收时把这些行连同串口日志一起提交。如果日志出现 `ACPI MCFG unavailable`，请一并提交 `VBox.log`：这表示 firmware 没有通过标准 UEFI configuration table 发布 ACPI。

有效验收必须保留：

- `[VBOX] ACPI RSDP`
- MCFG/ECAM bus 范围
- 目标 `[BUS] pci` ID 和 BAR
- 驱动 ready 行
- `[DRIVER] ... bound`
- block mount、lwIP attach、`/dev/fb0` 或 `/dev/event0` 的实际 I/O

没有目标设备时，还要验证驱动不会误绑定。

## 已验证的开发路径

loader 可以独立在 QEMU AAVMF 上测试。在 QEMU RAM 地址构建 QEMU board：

```bash
make ARCH=aarch64 BOARD=qemu-virt-aarch64 BRINGUP=1 kernel-onlymake ARCH=aarch64 BOARD=qemu-virt-aarch64 BRINGUP=1 \
    VBOX_AARCH64_LOAD_ADDRESS=0x40080000ULL \
    .kernel-build/aarch64-qemu-virt-aarch64-both-bringup/a20os-vbox-aarch64.img
```

该测试在 AAVMF 下已达到 `System ready (bringup, no userspace)`。

## 新增 VirtualBox ARM64 设备

1. 先确认协议是否已有通用 PCI/VirtIO 驱动。
2. 功能驱动不得包含 `CONFIG_BOARD_VIRTUALBOX_AARCH64` 或本平台的 GIC/PL011/ECAM 常量；这些事实属于 `kernel/platform/virtualbox-aarch64/`。
3. 当前 PCI 中断路由不完整，暂时使用轮询的驱动必须有单次预算和双重超时，并在 [实现状态](../drivers/meta/implementation-status.md) 记录解除轮询的条件。

## 常见故障

| 现象 | 定位 |
|---|---|
| 看不到 UEFI 消息 | 检查 EFI 启用、Secure Boot 关闭、镜像启动顺序第一。 |
| UEFI 后无 kernel 串口输出 | 检查 PL011 地址和串口配置（文件 vs TCP server）。 |
| 串口有日志但 `/bin/init` 不存在 | 检查 VirtIO-SCSI controller、磁盘附加和 FAT32 `/bin`。 |
| 桌面不出现 | 先看 `[GPU] SVGAv3 ready` 和 framebuffer 日志，不要只依赖屏幕。 |
| 网络不通 | 检查 E1000 ready、lwIP attach、NAT 和端口转发。 |
| 修改后行为不变 | 确认重新生成了镜像，VM 挂载的是新 VDI/UUID。 |

## 验收记录模板

```text
VirtualBox version:Host architecture:A20OS commit/diff:Build command:Image path and checksum:VM graphics/storage/network/input configuration:Enumerated PCI ID and BARs:Bound driver and ready line:Class consumer line:I/O performed and result:Known untested features:
```

完整提交清单和跨平台构建矩阵见 [构建、测试与提交](../drivers/meta/testing-and-submission.md)。

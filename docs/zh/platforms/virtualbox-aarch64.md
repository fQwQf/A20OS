# VirtualBox ARM64

本页从源码构建可直接挂载的 ARM64 UEFI 磁盘，并在 **ARM64 宿主的 VirtualBox** 中启动。实例是 `instances/vbox-aarch64.toml`，平台为 `virtualbox-aarch64`。这是虚拟机平台，不适用于 ARM 开发板，也不能在 x86_64 VirtualBox 中运行。

## 支持状态

已实现 UEFI loader、MMU、PL011 串口、GICv3、ACPI MCFG/PCI 枚举、VirtIO-SCSI、 E1000、VMSVGA 和 USB HID 路径。默认镜像包含 MMU 用户态与 FAT32 rootfs；实际 VirtualBox 启动及每个外设的可用性须按本页在目标 VM 验收，构建成功不等于运行通过。

| 约束 | 行为 |
| --- | --- |
| 内核入口 | `BOOTAA64.EFI` 按 ELF 完整内存跨度分配（含 BSS），将内核放在 `0x08080000`；交接 RSDP、命令行及版本化 UEFI 内存表 |
| RAM | 在 `0x08000000..0x28000000` 内按 UEFI 内存表选择可回收页；保留 runtime、ACPI 表、交接区及命令行；增加 VM 内存不会扩大当前映射窗口 |
| 串口 | PL011 `0xffddf000`，VirtualBox UART1；可通过宿主 TCP 转发交互 |
| 中断 | GICv3，GICD `0xfcd30000`、GICR `0xfcd40000` |
| 时钟/调度 | 平台强制协作式软件时钟路径；不能据此验收硬件定时器精度或抢占延迟 |
| 存储 | VirtIO-SCSI；PCI 中断路由不完整，设备可能使用轮询 |
| 网络/显示 | 需要对应驱动，推荐使用下方 embedded 构建；不保证自动启动桌面 |

内核要求配套版本的 `BOOTAA64.EFI`，旧加载器缺少版本化内存表会明确停止启动。加载器保留完整 ELF `PT_LOAD.p_memsz` 跨度并清零文件末尾；固件内存表中的 LoaderCode、LoaderData、BootServicesCode、BootServicesData、ConventionalMemory 只有在未标记 runtime 时才可能交给页分配器。RSDP、根表、全部 SDT，以及 FADT 引用的 DSDT/FACS 会验证并按页保留，即使固件将它们放在启动服务内存中也不会回收。内存表截断、描述符重叠、版本不匹配或保留区超出有界容量都会停止启动。 UEFI LoadOptions 最大为 1023 个字符，超长输入会明确失败，不会截断配置。这些规则遵循 [UEFI 内存分配及 ExitBootServices 约定](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)。

当前 `vbox-aarch64-text` 只选择另一镜像文件名，使用同一 FAT rootfs；它不是独立的驱动裁剪配置，也不会通过实例字段自动关闭 GUI。不存在 `vbox-gui-image-aarch64` 目标。

## 宿主依赖

构建可在 Linux x86_64 交叉编译环境执行；运行需要支持 ARM VM 的 VirtualBox ARM64 发行版，例如 Apple Silicon macOS。VirtualBox 7.2 的 Windows on Arm 入口属于厂商的实验功能，不能用 x86_64 包替代。具体宿主支持范围见[Oracle 安装说明](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/installation.html)。准备至少 8 GiB 构建主机内存和约 20 GiB 可用磁盘；VM 起步使用 1 vCPU、1 GiB RAM。

Linux 构建依赖示例：

```sh
sudo apt install build-essential git curl ca-certificates python3 xz-utils file \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu libc6-dev-arm64-cross \
  dosfstools e2fsprogs mtools parted util-linux
```

Python 需 3.11 或更新版本。安装 rustup 后，在仓库根目录执行：

```sh
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 aarch64-unknown-none
rustup component add --toolchain 1.96.0 rust-src
git submodule update --init --recursive
```

Rust 版本由仓库 `rust-toolchain.toml` 选择；Makefile 使用 `rust-src` 自动构建内核软件浮点 core。尚无 rustup 时，可从[官方安装入口](https://rustup.rs/) 安装；源码必须含子模块。运行端还需 `VBoxManage`，交互串口示例使用 `nc`（Debian/Ubuntu：`netcat-openbsd`）。无需 Guest Additions 或 GRUB。

## 构建与定位产物

推荐显式使用 embedded 驱动，命令均在仓库根目录执行：

```sh
tools/a20 check instances/vbox-aarch64.toml
tools/a20 package vbox-aarch64 -- DRIVER_DEPLOYMENT=embedded
a20_vbox_build=$(make -s ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both \
  DRIVER_DEPLOYMENT=embedded print-build-dir)
ls -l "$a20_vbox_build/a20os-vbox-aarch64.img"
sha256sum "$a20_vbox_build/a20os-vbox-aarch64.img"
```

镜像为 **256 MiB GPT 磁盘**，FAT32 ESP 从 1 MiB 起，含`EFI/BOOT/BOOTAA64.EFI`、`/init`、`/mksh`、`/etc` 和运行库。旁边的 `.img.sha256`与 ESP 中 `A20OS.MANIFEST` 可用于核对介质。构建目录由配置生成，不应照抄固定哈希。`package` 会校验 staged `/init` 与本次用户态产物相符。

若使用原实例的默认 generic 配置，命令为 `tools/a20 package vbox-aarch64`，可用 `tools/a20 ledger vbox-aarch64` 查询其路径。ledger 不接受 make 覆盖参数，不能用该默认记录定位上方 embedded 变体；应使用同一组选项的 `print-build-dir`。

需要 text 文件名时运行 `tools/a20 package vbox-aarch64-text -- DRIVER_DEPLOYMENT=embedded`，其输出在同一配置的构建目录，名为 `a20os-vbox-aarch64-text.img`。

## 创建 VM 并附加磁盘

1. 在 ARM64 VirtualBox 中创建 ARM64 Linux/Other VM，使用 1 CPU、1 GiB RAM、UEFI，关闭 Secure Boot；不要选择 x86 来宾架构。
2. 存储控制器选择 VirtIO-SCSI，启动盘接 port 0；先不附加其他磁盘。
3. 显示选择 VMSVGA；如需验收网络，选择 Intel PRO/1000 MT Desktop（82540EM）、NAT。
4. 串口 UART1 选择 TCP Server，端口 5555；文件模式只能记录输出，不能输入 shell 命令。

将构建生成的 `.img` 复制到运行端工作目录，命名为 `a20os-arm64.img`。在运行端转换为一个**尚不存在的新 VDI**：

```sh
VBoxManage convertfromraw a20os-arm64.img a20os-arm64.vdi --format VDI
VBoxManage showvminfo "A20OS ARM64" --machinereadable
```

把 `A20OS ARM64` 换为实际 VM 名。使用上条输出中的控制器名附加，以下假设为`VirtioSCSI`；也可在管理器的存储页面选择该 VDI：

```sh
VBoxManage storageattach "A20OS ARM64" --storagectl "VirtioSCSI" \
  --port 0 --device 0 --type hdd --medium a20os-arm64.vdi
VBoxManage modifyvm "A20OS ARM64" --uart1 0x03f8 4 --uartmode1 tcpserver 5555
VBoxManage startvm "A20OS ARM64" --type gui
nc 127.0.0.1 5555 | tee a20os-arm64-serial.log
```

这里 `--uart1` 是 VirtualBox 的端口配置接口，内核仍使用上述 PL011 地址。 TCP 字节流不需要给 `nc` 配置波特率。Windows 可用 `VBoxManage.exe` 执行相同参数，并用 PuTTY Raw/TCP 连接 `127.0.0.1:5555`。连接后重启一次 VM，以收集完整启动日志。

镜像更新后关闭 VM，转换为新的 VDI 文件名并重新附加；不要覆盖正在使用的 VDI。虚拟机流程不需要 `a20 deploy --media-device`，该命令是写物理介质，不是附加 VM 磁盘。

## 验收

预期依次观察到 UEFI loader、A20OS banner、`[VBOX] ACPI RSDP`、MCFG/ECAM、PCI ID/BAR、 VirtIO-SCSI 容量和 FAT32 挂载，随后 `[INIT] System ready` 与用户态 shell。`System ready` 单独出现仅证明内核初始化，用户态还须通过串口实际执行：

```sh
cat /etc/os-release
ls /bin
echo VBOX_ARM64_OK
```

保存输出及镜像哈希。无认证 `telnetd` 的启用以实际日志为准；只有出现`[telnetd] listening on port 2323` 才配置 NAT 转发。VM 关机时执行：

```sh
VBoxManage modifyvm "A20OS ARM64" --nic1 nat --nictype1 82540EM \
  --natpf1 "a20-shell,tcp,127.0.0.1,2323,,2323"
```

启动后可用 `nc 127.0.0.1 2323`；此路径还需要 E1000 ready、lwIP 接口及来宾地址日志。转发绑定 loopback。没有这些条件时使用 UART shell，不把 TCP 失败解释为内核启动失败。

显示验收要求看到 `15ad:0406` 的 PCI 绑定、GPU ready、framebuffer 模式与实际更新； USB 输入需 xHCI/HID ready 及真实按键/指针事件。彩条、shell 或桌面单一现象不能替代驱动链验收。PCI 轮询、图形/输入组合和长时间时钟行为均应单独记录。

## 失败定位

| 现象 | 检查 |
| --- | --- |
| 无 UEFI loader 输出 | ARM64 VM、EFI、Secure Boot、启动顺序和新 VDI 是否附加正确 |
| `incompatible AArch64 UEFI loader` / `invalid AArch64 UEFI memory handoff` | 重建并重新附加配套 EFI 镜像；记录固件内存表与异常日志，禁止退回整段静态 RAM |
| loader 后无内核日志 | UART1 TCP 是否连接；平台地址是否与当前 VBox 机器模型一致 |
| `ACPI MCFG unavailable` | 记录 VBox 版本、配置和 VBox.log；PCI 发现未完成，先勿排查文件系统 |
| 无 `/bin/init` 或无 shell | VirtIO-SCSI PCI ID/绑定、容量、ESP rootfs 和挂载日志 |
| 网络或显示无设备 | embedded 配置是否与所附磁盘相符、E1000/VMSVGA 型号是否正确 |
| 代码改动未反映 | 比较 raw 哈希、重新转换并附加新 VDI；不要复用旧介质 |

此平台地址窗口固定；其他 VirtualBox 机器模型、硬件定时器、抢占和完整 PCI IRQ 路由不在本页的保证范围。QEMU AAVMF 的启动结果不能替代 ARM64 VirtualBox 的运行验收。

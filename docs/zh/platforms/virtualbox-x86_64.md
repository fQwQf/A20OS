# VirtualBox x86_64

本页面向 **x86_64 宿主的 VirtualBox**，使用 PC 兼容平台 `qemu-virt-x86_64`。给出两条独立可执行的启动路径：自带 rootfs 的 UEFI 磁盘，以及 BIOS GRUB ISO 加单独 rootfs 磁盘。ARM64 VirtualBox 镜像不能用于本平台。

## 支持状态与介质选择

实现包含 64 位 MMU、COM1、APIC/IOAPIC、固件内存地图、ACPI/PCI、AHCI、E1000、 VMSVGA 和 PS/2 接口，用户态从 FAT32 rootfs 启动。实际 VM、VirtualBox 版本和外设组合须按本页验收，编译与镜像生成不能代替运行证据。

| 实例 | 实际产物和固件要求 |
| --- | --- |
| `vbox-x86_64` | 推荐：256 MiB GPT/FAT32 磁盘，`EFI/BOOT/BOOTX64.EFI`，自带 rootfs；开启 UEFI |
| `vbox-iso-x86_64` | GRUB rescue ISO，含 Multiboot 内核；可用 BIOS，需另接 rootfs 磁盘才能运行完整用户态 |
| `vbox-disk-x86_64` | GRUB **UEFI** GPT/FAT32 磁盘，默认 512 MiB；ESP 含 GRUB、内核和 rootfs；不是 BIOS 可启动硬盘 |

最后一项由 `mk_grub_disk_image.sh` 只生成 `x86_64-efi` GRUB，不能因为文件名是 disk 就关闭 EFI。BIOS VM 应使用 ISO 路径。UEFI 自带 loader 会把固件 RSDP 传入内核，有利于标准 ACPI/MCFG 发现；GRUB Multiboot 路径可能缺少这些固件信息。

## 宿主依赖

运行端使用支持硬件虚拟化的 x86_64 主机和 VirtualBox，先从 1 vCPU、1 GiB RAM 开始。构建端以下示例为 Linux；准备约 8 GiB 主机内存和 20 GiB 可用磁盘：

```sh
sudo apt install build-essential git curl ca-certificates python3 xz-utils file \
  gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu libc6-dev-amd64-cross \
  dosfstools e2fsprogs mtools parted util-linux \
  grub-common grub-pc-bin grub-efi-amd64-bin xorriso
```

Python 需 3.11 或更新版本。安装 [rustup](https://rustup.rs/) 后，在仓库根目录执行：

```sh
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 x86_64-unknown-none
git submodule update --init --recursive
```

Rust 版本由 `rust-toolchain.toml` 选择。UEFI 磁盘组装使用 parted/mtools；GRUB 和 xorriso 仅对应 GRUB 介质路径。运行端还需 `VBoxManage`，TCP 串口示例使用`nc`（Debian/Ubuntu 包 `netcat-openbsd`）。不需要 Guest Additions。

## 推荐路径：UEFI 自包含磁盘

显式 embedded 构建将所选设备驱动编入内核，避免验收依赖额外模块加载：

```sh
tools/a20 check instances/vbox-x86_64.toml
tools/a20 package vbox-x86_64 -- DRIVER_DEPLOYMENT=embedded
ls -l build/x86_64-pc/a20os-uefi-disk.img
sha256sum build/x86_64-pc/a20os-uefi-disk.img
```

整盘镜像包含 GPT、1 MiB 起的 FAT32 ESP、`BOOTX64.EFI`、`/init`、shell、`/etc`和运行库，无需再挂一个 rootfs 盘。路径由 `x86_64-uefi-disk` 目标稳定导出。 UEFI loader 不经过 GRUB，进入内核的 64 位入口并传递内存地图/RSDP。

内核默认物理地址为 **32 MiB（`0x02000000`）**；完整 ELF 内存跨度包含 BSS 和启动栈。 loader 在退出 Boot Services 前不写内核目标，退出成功后切换到独立的 2 MiB 页表与临时栈再复制、清零。目标范围必须全部属于可回收 RAM，不能覆盖 runtime、ACPI NVS、实际 ACPI 表、loader 自身或交接缓冲区；失败时 EFI 控制台打印阶段与十六进制状态码。它不要求可选的 1 GiB 大页，当前不接受固件启用的五级页表。

若固件保留区与默认完整内核范围冲突，确认另一完整可用区后重新构建并打包，例如：

```sh
tools/a20 package vbox-x86_64 -- DRIVER_DEPLOYMENT=embedded X86_KERNEL_LOAD_ADDRESS=0x04000000
```

`X86_KERNEL_LOAD_ADDRESS` 必须 2 MiB 对齐且位于 `0x00200000..0x3e000000`，完整内核仍须处于初始 1 GiB 映射内。参数同步汇编、链接和 EFI 载入位置，并使用独立构建目录。不能靠修改磁盘文件中的加载地址迁移已有内核。

在 VM 关机状态创建或设置：

1. x86_64 Linux/Other 64-bit，1 CPU、1 GiB RAM，启用 I/O APIC。
2. 开启 EFI，关闭 Secure Boot；启动顺序先硬盘。
3. SATA/AHCI 控制器，启动盘接 port 0；先不要附加其他硬盘。
4. VMSVGA 显示、至少 16 MiB 显存；网络选择 Intel PRO/1000 MT Desktop（82540EM）和 NAT。
5. 基础输入使用 PS/2 键盘/鼠标，串口 UART1 使用 TCP Server 5555。

把 `.img` 复制到运行端工作目录，命名 `a20os-x86_64.img`。使用尚不存在的 VDI 文件名：

```sh
VBoxManage convertfromraw a20os-x86_64.img a20os-x86_64.vdi --format VDI
VBoxManage showvminfo "A20OS x86_64" --machinereadable
VBoxManage modifyvm "A20OS x86_64" --firmware efi --ioapic on \
  --graphicscontroller vmsvga --uart1 0x03f8 4 --uartmode1 tcpserver 5555
VBoxManage storageattach "A20OS x86_64" --storagectl "SATA" \
  --port 0 --device 0 --type hdd --medium a20os-x86_64.vdi
VBoxManage startvm "A20OS x86_64" --type gui
nc 127.0.0.1 5555 | tee a20os-x86_64-serial.log
```

VM 名与 `SATA` 控制器名按 `showvminfo` 实际输出替换；也可用管理器完成这些设置。 Windows 用 `VBoxManage.exe`，并用 PuTTY Raw/TCP 连接 `127.0.0.1:5555`。 TCP 流不需要给 `nc` 配波特率；内核虚拟 COM1 的设置为 **38400、8N1**。连接后重新启动一次 VM，以保留完整启动日志。

更新 raw 后关闭 VM，转换成新的 VDI 文件名并重新附加；不要覆盖正在使用的磁盘。无需运行 `a20 deploy`：向 VM 附加 VDI 和向物理块设备写镜像是不同操作。

## BIOS 路径：GRUB ISO 与 rootfs 盘

此路径完整命令如下，不依赖 UEFI 盘：

```sh
tools/a20 check instances/vbox-iso-x86_64.toml
tools/a20 package vbox-iso-x86_64 -- DRIVER_DEPLOYMENT=embedded
a20_vbox_build=$(make -s ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both \
  DRIVER_DEPLOYMENT=embedded print-build-dir)
ls -l "$a20_vbox_build/a20os-x86_64.iso" "$a20_vbox_build/fat32.img"
cp "$a20_vbox_build/fat32.img" a20os-x86_64-root.img
```

`mk_grub_iso.sh` 只向 ISO 放入内核和 GRUB 配置，没有把 `fat32.img` 放进去。将 ISO 与复制的 rootfs 镜像传到运行端，将 rootfs 转为新 VDI：

```sh
VBoxManage convertfromraw a20os-x86_64-root.img a20os-x86_64-root.vdi --format VDI
VBoxManage modifyvm "A20OS x86_64" --firmware bios --ioapic on
VBoxManage storageattach "A20OS x86_64" --storagectl "SATA" \
  --port 0 --device 0 --type hdd --medium a20os-x86_64-root.vdi
```

在管理器中增加光驱并附加本次 `a20os-x86_64.iso`，将光驱置于首次启动顺序前面； rootfs 盘留在 AHCI port 0。沿用本页的 UART1 TCP、VMSVGA 和网卡设置后启动。不要把 ISO 转换成硬盘 VDI，也不要期待 AHCI 驱动把 ATAPI 光驱当作 rootfs 盘。

如需要 GRUB 的 UEFI 硬盘形态，运行`tools/a20 package vbox-disk-x86_64 -- DRIVER_DEPLOYMENT=embedded`，得到`build/x86_64-pc/a20os-rescue-disk.img`。转换/附加方式与推荐路径相同，但必须开启 EFI。

## 可观察验收

保存镜像哈希、VirtualBox 版本/VM 配置和完整串口日志。启动验收要求：

1. 内核 banner、`[RAM] usable` 与内存初始化。
2. ACPI/PCI 发现、存储控制器 PCI ID/BAR/绑定、容量和 FAT32 挂载。
3. `[INIT] System ready` 后实际出现 shell，并执行：

```sh
cat /etc/os-release
ls /bin
echo VBOX_X86_64_OK
```

`System ready` 本身不证明用户态已执行。设备分别验收：

| 设备 | 所需证据 |
| --- | --- |
| AHCI | PCI ID、BAR、port、容量、rootfs 挂载及可丢弃文件的读写/读回 |
| E1000 | `8086:100e` 绑定、网卡 ready、lwIP 接口/地址和真实收发 |
| VMSVGA | `15ad:0405` 绑定、GPU ready、framebuffer 模式和实际更新 |
| PS/2 | 输入设备 ready 后实际键盘/鼠标事件；串口输入不能证明 PS/2 正常 |

AHCI 当前仅首个可用 port、512 字节扇区和单 command slot；不保证 ATAPI、NCQ、多端口或热插拔。测试盘必须是可丢弃副本。不同虚拟网卡必须命中对应驱动的 ID 表。不承诺开机自动出现 GUI；GPU probe、彩条、桌面应用需要分别验证。

只有来宾实际打印 `[telnetd] listening on port 2323` 且网络就绪时，才使用远程 shell。关机状态配置 loopback NAT 转发：

```sh
VBoxManage modifyvm "A20OS x86_64" --nic1 nat --nictype1 82540EM \
  --natpf1 "a20-shell,tcp,127.0.0.1,2323,,2323"
```

启动后连接 `nc 127.0.0.1 2323`。这项服务无认证，转发保持在 loopback。

## 故障与限制

| 现象 | 检查 |
| --- | --- |
| EFI 找不到启动文件 | 选 UEFI 磁盘、关闭 Secure Boot、确认 VDI 和 `BOOTX64.EFI` |
| EFI loader 报 reserved/missing 或 ACPI overlap | 保存阶段/状态与描述符日志，核对完整内核范围；不能把 NVS 改成可用 RAM来绕过 |
| BIOS 不认 GRUB disk | 当前 GRUB disk 实际仅 UEFI；改用 ISO 或开启 EFI |
| 有内核无 init | ISO 路径是否另接 FAT32 rootfs 盘，AHCI port 0 是否正确、驱动是否绑定 |
| 缺少 MCFG | 区分 EFI loader 与 GRUB 入口；保存 RSDP/PCI 发现日志，不直接宣称 PCI 已初始化 |
| 无目标驱动 | 确认 embedded 变体、设备型号、PCI ID 和 BAR |
| 修改后仍旧行为 | 对照 raw 哈希，关闭 VM 后转换并附加新 VDI；检查旧 ISO 是否仍在光驱 |

平台 PCI 中断、ACPI 表与电源行为受具体 VM 固件影响。QEMU/OVMF 运行结果不能代替 VirtualBox 目标的设备验收。VBoxManage 选项说明见[Oracle 官方手册](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/vboxmanage.html)。

# 华为 CT5100

面向 Intel Bay Trail CT5100，复用 `ARCH=x86_64 BOARD=x86_64-pc`。
主实例 `huawei-ct5100` 从 64 位 UEFI 启动，在独立 U 盘分区上运行现有 apk
`xfce` world；使用 labwc、XFCE 终端和软件播放器。没有单独的 CT5100 用户态分支。
尚无 CT5100 实机验收；固件位数、实际 PCI ID 和外设连接必须以本机采集结果为准。

## 硬件与系统范围

华为 FusionAccess 资料将 CT5100 描述为 Bay Trail 双核 2.41 GHz、2 GB DDR3L、
8 GB SATA、千兆以太网、DVI-I、一个 USB 3.0 与五个 USB 2.0 接口的终端。
资料中带 DP、更多 USB 或四串口的 ST5110/CT6100 不能当作 CT5100 的接口清单。
具体 CPU 型号以本机采集为准；本移植不依据商品名称写死 PCI 设备、RAM、显存和 IRQ。

| 功能 | 当前实现与范围 |
| --- | --- |
| 启动 | 原生 x64 UEFI，GOP 与 ACPI RSDP 交接；另有串口 BIOS 恢复镜像。32 位 UEFI 未实现 |
| CPU/时钟 | baseline x86-64、2 MiB 启动页；裸机 Intel family 6/model 0x37 使用 MSR 比率计算 TSC，不要求 AVX 或 1 GiB 页 |
| 设备枚举 | 现有 ACPI/uACPI、MCFG/传统 PCI 配置访问、中断路由；不建立华为专用枚举层 |
| SATA | 增加 Bay Trail `8086:0f22/0f23` AHCI 模式、PCS 端口使能、固件所有权交接及真实 DMA 位数限制；不把 IDE 模式当 AHCI |
| USB | 现有 xHCI/HID/存储栈；补充固件所有权交接、AC64 DMA 限制和 Bay Trail `8086:0f35` 的 USB2/3 路由 |
| 显示 | GOP 固件模式经 simplefb 接入公共 DRM；支持实际 stride、16/24/32 位及 RGB/BGR/BitMask 转换；XFCE 选择 pixman |
| 输入 | xHCI USB 键鼠经 evdev、eudev、libinput、seatd 进入现有 Wayland 会话 |
| 音频 | 现有 PCI HDA 按 codec/DAC/pin 路径探测，ALSA 48 kHz 双声道 S16；XFCE SDL 默认 ALSA。实际插孔/codec 待验收 |
| 网络 | 原厂资料未公布芯片 PCI ID；现有 E1000/RTL8139 不等于支持 RTL8111/8168。未承诺板载网络或 SSH |
| 系统盘 | 仅自动选择唯一的 GPT Linux-data、精确分区名 `A20OS distro`；该分区读写挂到 `/extra`，然后现有发行版启动路径 chroot 并执行 stage-2 init |

没有 Intel GPU 原生 modesetting、热插拔、多屏、GPU 加速或硬件视频解码。
GOP 需提供可直接写入的显存，PixelBltOnly 模式不可用。软件桌面和视频依赖机器实际
内存与 CPU 性能；4 GB world 的大小是磁盘容量，并非运行时 RAM 要求或性能保证。
默认先使用一个 CPU，避免将多核实机验收混入首次启动。
当前 x86 MMIO 驱动映射要求固件已将 BAR 放在 4 GiB 以下的 UC 窗口；
不对物理 PC 猜测可重分配地址。BAR 超出此范围时拒绝探测并记录日志。
ALSA 使用 HDA 的真实队列状态和非阻塞提交，当前仅提供 S16 交错播放；
不承诺 PCM mmap、录音、暂停或混音。原生 `/dev/audioN` PCM 接口保持可用。

## 从 Windows 采集机器信息

保留 Windows。将仓库的 `tools/collect_pc_windows.ps1` 复制到 CT5100，
在 Windows PowerShell 5.1 或更新版本运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\collect_pc_windows.ps1 -OutputPath .\ct5100-hardware.json
msinfo32
```

脚本只读取 CIM 和固件类型，写入指定的新 JSON 文件；不会覆盖已有报告，不更改
注册表、固件或磁盘分区。报告包含 CPU、固件版本、PCI/HDAUDIO 完整 HardwareID
（含 SUBSYS/REV）和系统发现的串口；采集失败的项目写入 `issues`。
Windows 显示适配器名称、网卡名称与 `PCI\VEN_...&DEV_...` 均应保留。
UEFI/Legacy 类型和 Windows 位数是分别记录的，不能用 64 位 CPU 推导出 64 位 UEFI。
也可在设备管理器的“详细信息 → 硬件 ID”查看网卡、SATA、USB 和音频控制器。

串口仅在本机确有标准 COM1 `0x3f8/IRQ4` 时可使用；公开 CT5100 资料不保证它存在。
目标机插入 USB 转串口不能代替原生 COM1。UEFI 桌面镜像的交互入口是显示器与 USB
键鼠，实例里的 `[target].serial` 仅供已经确认原生串口的调试接线使用。

## Linux 宿主准备

以下仓库命令在源码根目录执行，Python 需 3.11 或更新版本：

```sh
sudo apt-get install build-essential git curl ca-certificates python3 \
  gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu \
  parted mtools e2fsprogs util-linux xz-utils openssl fakeroot ccache
git submodule update --init --recursive
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 x86_64-unknown-none
```

宿主还需仓库 apk world 构建工具所要求的依赖；第一次构建会联网获取 Alpine 上游
软件包，并使用 `build/cache/apk`。本地开发包按现有 package recipes 构建和签名。
构建 world 采用 usermode，不要求以 root 运行构建器。为完整镜像和缓存预留足够磁盘。

## 桌面镜像

```sh
tools/a20 check huawei-ct5100
tools/a20 build huawei-ct5100 -- -j2 GUI_MEDIA=
tools/a20 package huawei-ct5100 -- -j2 GUI_MEDIA=
tools/a20 ledger huawei-ct5100
```

与 `tools/a20 run xfce-x86_64` 使用同一 `packages/world/xfce.world` 和
`packages/overlay/xfce`。`GUI_MEDIA=` 避免自动加入本机缓存中的 Minecraft 数据，
不影响 XFCE 或播放器。完整镜像为：

```text
build/huawei-ct5100/a20os-uefi.img
```

GPT 第一个分区为 1–255 MiB FAT32 ESP，包含 `EFI/BOOT/BOOTX64.EFI`；
第二个分区从 256 MiB 开始，名称严格为 `A20OS distro`，Linux filesystem data
类型，包含原样复制的 4096 MiB ext4 world。其后留有备份 GPT 空间。
使用容量足够的独立 U 盘（通常至少 8 GB）；镜像不安装到内置 Windows 盘。
不要把多个同名 A20OS 发行版盘同时接入目标，自动选择遇到歧义会拒绝挂载。

将完整镜像写入专用 U 盘前，核对整盘身份并卸载其分区：

```sh
lsblk -o NAME,SIZE,MODEL,SERIAL,MOUNTPOINTS
tools/a20 deploy huawei-ct5100 --no-build --no-console \
  --media-device /dev/disk/by-id/实际USB整盘设备 --dry-run
```

核对 dry-run 后去掉 `--dry-run` 执行写入，必要时仅对此步骤提权。
这会覆盖所选 U 盘；不要选择内置 Windows 磁盘。也可在 Windows 将这个 raw IMG
按整盘镜像写入专用 U 盘，不能只复制 IMG 文件到 FAT 分区。

显示器先接好 DVI-I，键鼠先插好。通过本机固件的一次性启动菜单选择 U 盘的 x64
UEFI 入口；具体按键以本机固件为准。固件需允许未签名 EFI 应用。
本流程不升级 BIOS，不改变内置 Windows 引导项。恢复 Windows 时拔下 A20 U 盘，
从原启动项进入 Windows。不要为了试镜像随意改变现有 Windows 的 SATA 模式。

## 启动和功能验收

应进入现有 XFCE Wayland 会话。打开 XFCE 终端，执行：

```sh
uname -a
cat /etc/os-release
apk info -e xfce4-terminal
ls /dev/dri /dev/input /dev/snd
```

实际输入命令并看到结果，才证明键盘、终端和 shell 已联通。
软件包在宿主组装进入镜像，首次启动不依赖板载网络能够联网。
在显示器上确认桌面和终端字形/颜色，再通过镜像自带 ffplay、Parole 或 mpv
打开本地媒体；首次采用短片和较低分辨率，逐项检查画面和模拟音频输出。
可在构建主机预装自己的媒体，首次播放不依赖网卡或 Windows 的 ext4 支持：

```sh
tools/a20 package huawei-ct5100 -- -j2 GUI_MEDIA=/绝对路径/clip.mp4
```

重写这份新镜像后，在 XFCE 终端打开 `/usr/share/a20-media/clip.mp4`。
重新打包会重建发行版分区，先保存 U 盘内需要保留的数据。
播放器存在不代表 codec、音画同步或全屏性能已在本机验收。
桌面关机可用面板的电源菜单；终端使用 `poweroff -f` 调用内核的有序关机路径，
BusyBox 的普通 `poweroff` 所期待的 init 信号协议不适用于当前 stage-2 supervisor。

若有已确认的原生 COM1，可在另一台主机打开唯一的 38400 8N1 控制台：

```sh
tools/a20 console huawei-ct5100 --interactive \
  --serial /dev/serial/by-id/实际调试适配器
```

RS-232 与 TTL 电平不得混接。日志应包含 `[DISTRO]` 的唯一分区挂载结果、
`[INIT] Distro rootfs detected`、`[INIT] Chrooted to /extra` 及 XFCE 的 pixman 后端选择。
无串口的机器不能借此命令获得控制台。

## 恢复实例与限制

`huawei-ct5100-recovery` 是独立的 x64 UEFI RAM shell 镜像，不接管发行版分区，
包含已有 videoplay/audioplay 诊断程序。`huawei-ct5100-bios` 是 GRUB BIOS RAM
shell ISO，不交接 GOP。两者交互均要求已确认的原生 COM1：

```sh
tools/a20 package huawei-ct5100-recovery -- -j2
# BIOS 路径另需：sudo apt-get install grub-pc-bin grub-common xorriso
tools/a20 package huawei-ct5100-bios -- -j2
```

分别生成 `build/huawei-ct5100/a20os-recovery.img` 和
`build/huawei-ct5100/a20os-bios.iso`，写盘步骤与主实例相同。
恢复 shell 可执行 `videoplay --test 320 240 30`（仅 UEFI）与
`audioplay --tone 1000 --duration 500`。FAT 媒体只读挂载到 `/media`。

主实例缺少/损坏/歧义 GPT 或 ext4 挂载失败时保留 RAM shell；没有串口时仍需重新
启动修复介质。识别为发行版后，启动器会切换根目录并加载 stage-2 init；其解释器或 ELF 损坏仍可能停止启动，不能承诺自动恢复。
32 位 UEFI、仅 EHCI 控制的 USB 接口、未覆盖的网卡/存储、主板特定功放、
HDMI/DP 音频、休眠和 Secure Boot 信任链尚未构成实机支持承诺。

## 公开参考实现与文档

- [华为 FusionAccess 技术资料，原厂文档镜像](https://www.actfornet.com/ueditor/php/upload/file/20190326/1553565775936407.pdf)：CT5100/ST5110/CT6100 的配置区别。
- [香港中文大学（深圳）实际采购清单](https://tencentlab.cuhk.edu.cn/zh-hans/node/1046)：CT5100 Bay Trail、2 GB/8 GB 与外设配置。
- [Linux v6.17 tsc_msr.c](https://github.com/torvalds/linux/blob/v6.17/arch/x86/kernel/tsc_msr.c)：Bay Trail TSC 比率与参考时钟。
- [Linux v6.17 AHCI](https://github.com/torvalds/linux/blob/v6.17/drivers/ata/ahci.c)：Bay Trail AHCI ID 与 Intel PCS quirk。
- [Linux v6.17 USB PCI quirks](https://github.com/torvalds/linux/blob/v6.17/drivers/usb/host/pci-quirks.c)、[coreboot Bay Trail xHCI](https://github.com/coreboot/coreboot/blob/4.22/src/soc/intel/baytrail/xhci.c)：固件交接与 USB 路由。
- [UEFI GOP 协议](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html#graphics-output-protocol)：像素布局、实际行距和帧缓冲交接。
- 软件包、XFCE 会话与许可证继续沿用仓库现有 apk world；本轮没有将 Linux GPL 驱动源码复制进内核。

# Quartz64-Zero（RK3566T）

本平台对应 **PINE64 Quartz64-Zero / Quartz64 Model “Zero”、RK3566T、1 GiB LPDDR4**，
实例为 `instances/quartz64-zero.toml`。不适用于 Quartz64 Model A/B、SOQuartz 或 Radxa Zero。
当前提供单核 MMU 内核、UART2、GICv3、架构定时器、固件 DTB 内存保护、PSCI 调用和 SDMMC0 块驱动，
以及可构建的完整 microSD 镜像。**实机启动尚未验证**；默认实例仅执行内核初始化与
内存分配检查，没有交互 shell、用户态程序或持久 rootfs。


## 串口交互 shell 配置

`quartz64-zero-shell` 实例嵌入静态链接的 `/bin/init`、`mksh` 和基本命令。
它采用完整用户态启动路径（`bringup=false`、`ramfs_user=true`），不需要根磁盘。
默认嵌入 17 个程序：`init mksh help ls cat ps sleep timer_preempt timer_idle`
和 `echo uname mkdir rm cp sync poweroff reboot`。启动时保留 RAMFS 的 `/bin`，
不会把含内核与 DTB 的启动 FAT 分区自动覆盖挂载到这里。
原 `quartz64-zero` 实例仍用于内核初始化验收，二者不能互换验收标准。

```sh
tools/a20 prepare quartz64-zero
tools/a20 build quartz64-zero-shell
tools/a20 package quartz64-zero-shell
tools/a20 console quartz64-zero-shell
```

SD 镜像是 `build/quartz64-zero/a20os-quartz64-zero-shell-sdcard.img`，部署时使用
`tools/a20 deploy quartz64-zero-shell --media-device /dev/实际卡设备`。
打包会重新生成所选实例对应的内核与 handoff，公共 handoff 目录不得混用旧包。

串口接线、电平、波特率和固件要求沿用本文。`System ready` 只表示内核就绪；
验收还须看到 init 启动 mksh，实际输入并检查输出：

```sh
echo A20_SERIAL_OK
uname -a
ls /bin
cat /proc/meminfo
mkdir /tmp/check
echo hello > /tmp/check/message
cat /tmp/check/message
rm /tmp/check/message
sync
poweroff
```

文件系统为 RAM-only，重启后丢失写入。最小用户构建仅编译嵌入命令，不构建
fastfetch、FFmpeg 或完整发行版。这个配置不包含 SSH 服务，也不承诺网卡、
显示或音频可用。当前状态为交叉构建与封装检查；板上执行仍须实机验收。

## 硬件与内核接口

| 项目 | 实现及约束 |
| --- | --- |
| SoC | RK3566T，四核 Cortex-A55；仅启用硬件 CPU0，拒绝多核配置 |
| 启动协议 | ARM64 Image，物理入口 `0x02080000`；EL2/EL1 统一进入 EL1；U-Boot `booti` 传入 DTB |
| Image 头 | text offset `0x80000`、flags `0xa`，允许在 2 MiB 对齐的任意 RAM 基址上加载；不可改为强制靠近 RAM 起点的 flags |
| DDR | `0x00000000..0x40000000` 上限；分配范围排除前 2 MiB，必须检查 DTB RAM 和固件保留区域 |
| MMIO | 启动页表覆盖 `0xc0000000..0x100000000` 为 Device；DDR 为 Normal memory |
| 串口 | UART2 `0xfe660000`，DW APB UART，32 位访问、寄存器间距 4 字节；SPI 118 / INTID 150；1500000、8N1 |
| 中断 | **GICv3 / GIC600**，GICD `0xfd400000`，GICR `0xfd460000..0xfd4e0000`；按 GICR_TYPER affinity 选择启动 CPU 的 redistributor；CPU 接口使用 ICC 系统寄存器 |
| 定时器 | 非安全物理定时器 PPI 14 / INTID 30；从 CNTFRQ 读取频率，固件配置预期 24 MHz |
| 电源 | TF-A PSCI SMC 重启/关机；实际断电效果需上板确认 |
| microSD | SDMMC0 `0xfe2b0000`、SPI 98 / INTID 130；DW-MSHC 单线 PIO，固定 3.3 V、最高 12 MHz，SDSC/SDHC/SDXC，发布统一 block class；实机读写未验收 |
| 可选视频固件 | RK3566 VOP2 → Synopsys DW-HDMI，EDID 模式选择，实际扫描缓冲区以 DT simplefb 交接；交叉构建可验证，Zero 实机显示待验收。 |
| 未实现 | eMMC、USB、PCIe、以太网、Wi-Fi/BT、PCM 音频、内核 PMIC/DVFS、SMP、ITS/MSI |

GICv3 是上游 RK356x SoC 设备树确认的硬件事实。GICv3 的 CPU 接口不是 GICv2 的 GICC
MMIO，不能沿用 PINE A64 的中断控制器。EL3 固件必须先将 UART 和非安全物理定时器
分配为 Group 1 Non-secure；非安全 EL1 无权改写固件的安全中断归属。
`IGRPMODR` 在启用安全扩展时对非安全访问为 RAZ/WI，内核不会尝试改写安全分组。RK356x ITS 存在 DDR 地址限制，本平台没有启用
ITS，也不宣称支持 PCIe MSI。

串口保留固件配置的时钟、pinmux 和波特率除数。必须经过 DDR 初始化、TF-A 和 U-Boot，
不能从 BootROM 直接跳转内核。分配器只接收一段连续内存；DTB、memreserve 或
reserved-memory 位于内核后方时，保守地丢弃该位置之后的 RAM。错误板型、缺少有效
DTB、内核与保留区域重叠、内存不足会停止启动。动态 `reserved-memory` 的 `size`
分配请求不受支持，遇到此属性会明确停止，不能忽略后继续释放 RAM。

## 主机准备与完整构建

Linux 主机使用 AArch64 GNU 交叉工具链；Debian/Ubuntu 安装内核、固件及 FAT 组装依赖：

```sh
sudo apt-get install build-essential git curl ca-certificates python3 \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu libc6-dev-arm64-cross \
  bison flex swig libssl-dev libgnutls28-dev python3-dev python3-pip \
  python3-setuptools python3-yaml dosfstools mtools
```

安装 rustup 后配置仓库指定的 Rust 1.96.0；若尚无 rustup，可先执行下面前两条命令。已有 rustup 时可跳过安装器：

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 aarch64-unknown-none
rustup component add --toolchain 1.96.0 rust-src
```

下列仓库命令均在 A20OS 源码根目录执行；首次构建先运行 `git submodule update --init --recursive`。主机 Python 必须为 3.11 或更新版本，可用 `python3 --version` 确认。首次下载源码、Rust 组件和 Cargo 依赖需要联网。

AArch64 内核需要匹配的软件浮点 `core`，由 Makefile 使用 `rust-src` 自动构建；无需另行切换 nightly。

在仓库根目录执行：

```sh
tools/a20 check instances/quartz64-zero.toml
tools/a20 prepare quartz64-zero
tools/a20 package quartz64-zero
tools/a20 deploy quartz64-zero --dry-run
```

`prepare` 下载带 SHA256 校验的固定源码与 Rockchip 二进制固件，构建 SPL/U-Boot 和
Zero 独立 DTB。pyelftools 0.32 安装到私有构建目录，不修改主机 Python 环境。
源码每次从已校验归档重新解包；编译输出在独立目录重用。需预留约 1 GiB 主机空间。
`package` 自动构建对应内核、组合引导固件和 FAT32 分区，并逐文件读回校验。
准备与打包只写普通文件，不需要 root、loop mount 或手工分区。

| 产物 | 用途 |
| --- | --- |
| `build/quartz64-zero/firmware/` | `u-boot-rockchip.bin`、`rk3566-quartz64-zero.dtb`、宿主 mkimage、来源 manifest |
| `build/quartz64-zero/a20os-quartz64-zero-sdcard.img` | 默认 128 MiB 整盘 SD 镜像 |
| 同名 `.img.json` | 镜像及 FAT 文件 SHA256、固件版本和布局 |
| `build/quartz64-zero/handoff/` | ELF、ARM64 Image、匹配 DTB、manifest、U-Boot 命令文本 |

只需普通内核构建时运行 `tools/a20 build quartz64-zero`；`kernel.bin` 是内核 Image，
不能把它当作整盘镜像写卡。独立入口为 `python3 tools/quartz64_zero.py prepare` 和
`python3 tools/quartz64_zero.py sdcard`，各自 `--help` 可查看缓存、交叉编译器和镜像位置参数。

## 固件选择与板级设备树

启动链为 Rockchip BootROM → rkbin DDR 初始化程序 → U-Boot SPL → rkbin TF-A BL31
→ U-Boot → A20OS。使用以下固定输入：

| 组件 | 来源 |
| --- | --- |
| U-Boot | v2025.10，提交 `e50b1e8715011def8aff1588081a2649a2c6cd47`，归档 SHA256 `33b6910dc737bfbbf4317ae0a48fe687335d6a08eee8fdfcb3a4adffc4c4a516` |
| Rockchip rkbin | 提交 `3e288fe814e059dd06833495f845cab04ac20a5c` |
| DDR/TPL | `rk3566_ddr_1056MHz_v1.26.bin`，SHA256 `9966c21aadf00737ccecc2fad6fe3c902ff26956433029f70dfd759d4f60616a` |
| TF-A BL31 | `rk3568_bl31_v1.46.elf`，SHA256 `c81ac7e8e1fd727cf7f0db62a9aaea760bde2b270e34d98eb264a264b86df749` |
| 基础配置 | `generic-rk3568_defconfig`，明确替换 DEFAULT_DEVICE_TREE、OF_LIST 和 SPL_OF_LIST 为 Zero 独立 DT |
| 本地板级源码 | `tools/firmware/quartz64-zero/`；使用上游 RK3566 SoC DTS，根 compatible 为 `pine64,quartz64-zero` |

上游这一版本只有 Quartz64 Model A/B 的板级配置，没有 Zero defconfig。因此这里构建的是
本仓库维护的最小 Zero 配置，不能称为上游已验证的 Zero 固件。Rockchip 的 RK3566 DDR
和 RK3568 BL31 命名来自厂商组件及 PINE64 的构建说明；**1056 MHz DDR 训练程序对
Zero 实际 DRAM 颗粒的兼容性仍须到货确认**。准备成功表示主机侧构建完成。

Zero 原理图的 SD 插座电源直接接 `VCC3V3_SYS`，检测脚为 GPIO0_A4。设备树启用
SDMMC0 四位总线，固定 3.3 V、最高 25 MHz，并禁用 1.8 V 切换；不复制 Model A
的 GPIO 控制 SD 电源。保留已有 PMIC 电源状态，不执行未经核实的 PMIC/DVFS 调节。
eMMC、SPI、USB 和其他板级外设不作为启动依赖。OP-TEE 不是此路径的组成部分。

内核沿用 DT 枚举出的 SDMMC0 设备，不再创建另一份板级设备。`arch/aarch64/boards/quartz64-zero/storage.c`
负责 SD 管脚复用、上拉、驱动强度、时钟门控和复位；选择 24 MHz 晶振，计入 Rockchip
集成层固定二分频后向通用驱动提供 12 MHz 输入。通用 `drivers/block/dw_sdio.c`
完成有界控制器复位、卡枚举、CSD 容量解析、块/字节寻址和单扇区读写。
它不进行 UHS/1.8 V 切换、DMA、四线提速或热插卡。
无卡、CRC 或数据超时会返回错误；I/O 失败后停止该卡继续传输，重新绑定会重新枚举。
默认实例仍是内核初始化检查，未增加持久 rootfs；SD 镜像第一分区由 U-Boot
读取内核，内核能绑定 SD 控制器并不代表已建立用户态根文件系统。
固件 manifest 记录本地 DTS 的哈希；Model A/B 的 DTB 无法通过包的板型校验。

## 写入 microSD

| 镜像位置 | 内容 |
| --- | --- |
| LBA 0 | MBR，第一分区为 bootable FAT32 |
| 32 KiB / LBA 64 | `u-boot-rockchip.bin` 起点，Rockchip DDR + SPL |
| 8 MiB / LBA 16384 | 合并固件内的 U-Boot FIT；相对合并镜像偏移 `0x7f8000` |
| 16 MiB / LBA 32768 | FAT32 分区起点，避开合并固件范围 |
| FAT 根目录 | `kernel.bin`、`rk3566-quartz64-zero.dtb`、`boot.cmd`、`boot.scr` |

32 KiB 写入位置来自 PINE64 官方说明；FIT 相对位置来自固定 U-Boot 的 binman 配置与
`CONFIG_SPL_PAD_TO`，脚本检查 RKNS 头、FIT 标记与分区前的容量边界。
`boot.scr` 先检查 FAT 加载返回、文件大小和 CRC32，全部通过才执行 `booti`。
这是传输完整性检查，不是签名安全启动。脚本不写持久环境，也不更新 SPI/eMMC。

确认读卡器**整盘**设备和其所有分区已卸载，再执行：

```sh
tools/a20 deploy quartz64-zero --media-device /dev/实际SD整盘设备 --no-console
```

该步骤会覆盖所选卡的分区表和前 128 MiB 内容；实例 `/dev/sdX` 是占位符。
使用专用空卡，并按读卡器容量/型号识别设备。卡可以更大，镜像后方空间暂不使用。

## 供电、串口与首次启动

官方设备说明要求 **5 V / 3 A** 供电，圆口外径 **3.5 mm**、内径 **1.35 mm**、中心正极。
按板上接口及电源铭牌确认规格。串口转接器不要接供电线。

使用支持 **1500000 baud** 的 **3.3 V TTL** USB 串口。根据官方原理图第 18 页 PI-2
40 针座和丝印确认方向：

| 板上物理针脚 | 连接 |
| --- | --- |
| 8：UART2_TX_M0 / GPIO0_D1 | 接转接器 RX |
| 10：UART2_RX_M0 / GPIO0_D0 | 接转接器 TX |
| 6 或 9：GND | 共地 |

本实例为 1500000、8N1、无硬件流控。115200 设置会产生乱码。先连接终端：

```sh
tools/a20 console quartz64-zero --interactive --serial /dev/serial/by-id/实际串口设备
```

断电插卡后上电。预期顺序是 DDR 训练、SPL、BL31、U-Boot，随后自动扫描 FAT 中的
`boot.scr`，打印 `A20OS Quartz64-Zero SD boot`，进入 A20OS。
默认 bring-up 应输出 `[Quartz64-Zero] allocator RAM=`、定时器/内存初始化、
`System ready (bringup, no userspace)`、`part ok`，然后请求 PSCI 关机。
终端工具保存日志并检查实例标记。`part ok` 仅是初始化及一次内存分配检查，不证明
长期稳定、UART RX IRQ 压力、定时器精度或持久存储能力。

### U-Boot 提示符下恢复

使用本仓库准备的固件时，SD 为 `mmc 0`。打断倒计时后先检查：

```text
version
bdinfo
mmc list
mmc dev 0
mmc rescan
fatls mmc 0:1
```

确认 DRAM 从零开始、容量为 1 GiB、SD 文件齐全，然后运行带校验的自带脚本：

```text
setenv scriptaddr 0x00c00000
fatload mmc 0:1 ${scriptaddr} boot.scr
source ${scriptaddr}
```

`fatload` 必须打印读取字节数且没有错误，方可继续 `source`。手工逐项排查时也可使用：

```text
setenv loadaddr 0x02080000
setenv fdt_addr_r 0x0a000000
fatload mmc 0:1 ${loadaddr} kernel.bin
fatload mmc 0:1 ${fdt_addr_r} rk3566-quartz64-zero.dtb
booti ${loadaddr} - ${fdt_addr_r}
```

逐条执行、任一失败即停止；不要启动上次残留的 RAM。手工命令没有自动 CRC 校验，
优先执行卡上 `boot.scr`。DTB 暂存地址与内核完整 footprint 分离；U-Boot 仍可为 DTB
分配最终位置，内核按接收到的地址保护它。不要 `saveenv`。

### 故障定位

- 没有 DDR/SPL 首屏：先核对电源、8/10/GND 接线、1.5 Mbps 及 SD 内容；移除可拆卸
  eMMC 模块以排除其他固件启动优先级。若能通过原厂镜像启动而本镜像止于 DDR 训练，
  保留两份串口日志，DDR/TPL 与该板 DRAM 的兼容性尚未通过验收。
- 停在 U-Boot：用上述 `mmc`/`fatls` 区分卡检测、文件与启动脚本错误；`crc32` 或大小
  检查失败应重新 package、写卡，不能跳过校验直接启动。
- 内核内存检查停止：记录完整 `bdinfo`、固件版本、DTB 与镜像 manifest；不得换用
  A/B DTB 绕过板型检查。
- 卡上 SPL/U-Boot 尚未执行就进入其他固件：检查可拆卸 eMMC 和现有启动介质。此流程
  不自动改写其他介质；从 SD 启动不需要升级板载固件。ROM/板上启动顺序与 DDR
  颗粒均是首次上板要实际确认的项目。
- 内核有首屏但定时器无后续进展：保存 GIC/异常日志，确认固件支持非安全 ICC 接口、
  CNTFRQ 非零和 CPU0 入口。GICv3 IRQ、PSCI 实际掉电效果仍需各自实机验收。

厂商发布页提供 **明确标为 Quartz64 Zero 的 SD Android 镜像及配套 SDCardTool**，
可用另一张卡作对照。该下载是厂商镜像格式；不要把它当作本流程的 U-Boot 二进制或
直接替换 `u-boot-rockchip.bin`，也不要使用 Model A/B 的镜像。

## 可选 HDMI 固件和显示交接

默认 `prepare` 和 shell 包保持串口配置。需要 HDMI 时，使用独立的视频固件输出：

```sh
tools/a20 prepare quartz64-zero-media
tools/a20 package quartz64-zero-media -- -j2
tools/a20 deploy quartz64-zero-media --media-device /dev/实际SD设备
```

`quartz64-zero-media` 嵌入既有 `videoplay`/`audioplay` 播放器，自动选择独立的
`firmware-video`，生成 `a20os-quartz64-zero-media-sdcard.img` 并记录所用视频固件摘要。
`deploy` 按上文的整盘确认流程写卡；此处 `/dev/实际SD设备` 必须换成经核对的目标。
开机前连接供电正常、支持 HDMI 的显示器；串口仍为 UART2、1500000 8N1。
不要把 Model A/B、SOQuartz 或 NanoPi 的 DTB 写入 Zero 包。

视频配置固定 U-Boot v2026.07 和 [VOP2/HDMI 九补丁系列](https://patchwork.ozlabs.org/project/uboot/list/?series=514476)。
源码、每个补丁和本地板级 glue 均记录校验摘要；其 GPL-2.0+ 许可证及作者见
[视频固件输入说明](../../tools/firmware/quartz64-zero-video/README.md)。
默认的 v2025.10 固件没有 RK3566 VOP2 驱动，仅开启 `VIDEO_ROCKCHIP` 不会获得显示。

显示链路为 VOP2 VP0 → DW-HDMI → 板上 HDMI 接口。Zero 原理图第 23 页确认
RK817 LDO2（0.9 V）和 LDO7（1.8 V）经 R2137/R2136 为 HDMI PHY 供电，
视频 DT 单独描述这两路电源，不借用其他板型完整 PMIC 配置。
U-Boot 使能控制器 gate、PHY 参考时钟和这两路电源，通过 EDID 确定实际模式。
在启动内核前，板级 `EVT_FT_FIXUP` 将已成功探测的宽高、stride、物理地址和像素格式写到
`/chosen/framebuffer@...`，并通过 `/reserved-memory` 保留整个 U-Boot video 分配。
像素 fetch 为 ARGB8888，但 VP0 单层底部 mixer 显式设为全局 alpha=255、禁用逐像素 alpha，
源颜色系数为全局 alpha、目标系数为其反值，HDR 混合旁路。因此高 8 位不参与颜色合成，
发布 `x8r8g8b8` 与实际像素语义一致；不依赖冷启动或热复位后的寄存器默认值。
寄存器设置对照 [Linux v6.12 `vop2_setup_alpha`/`vop2_parse_alpha`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c)
及 [mixer 寄存器定义](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/rockchip/rockchip_drm_vop2.h)。
`VIDEO_REMOVE` 关闭，避免 U-Boot 在交接时停掉 VOP2；内核接管后不得关闭这些显示时钟和电源。
串口应出现 `A20OS HDMI simplefb:` 及实际模式。未连接显示器或 HDMI 探测失败时不创建虚假 framebuffer，
继续串口启动。内核能否显示图像还取决于相同版本的通用 simplefb/fbdev 驱动。

此配置的主机交叉构建、DTB 编译和普通文件 SD 封装可以自动检查；**HDMI 冷启动、
EDID 兼容性、持续扫描和内核画面尚无 Zero 实机验收结果**。QEMU virt 不模拟该显示链路。
验收时保留串口日志、`firmware-video/manifest.json`、SD 镜像 manifest 与显示器型号；
分别确认冷启动首屏、内核接管后持续输出及连续更新画面。

音频有两条不同通路：SoC 内部 `i2s0_8ch` → HDMI 音频发送器，以及原理图第 23 页的
`I2S1_M0` → RK817 codec → 耳机模拟输出。当前未提供 RK356x I2S、DMA、DW-HDMI
PCM/时钟恢复或 RK817 codec 内核驱动，视频配置也不启用 `hdmi-sound`。
因此不能将有 HDMI 画面视为有声音，也不把静态 `simple-audio-card` 节点视为音频驱动。

## 依据

- [PINE64 Quartz64-Zero 设备页](https://pine64.org/devices/quartz64-zero/)：RK3566T、1 GiB LPDDR4、供电规格。
- [PINE64 官方 Zero 原理图](https://pine64.sfo3.cdn.digitaloceanspaces.com/doc/quartz64/Quartz64-ZERO_Schematic_v01_20240428.pdf)：第 18 页 PI-2 UART2，第 19 页 SD 插座与直接 3.3 V 供电。
- [固定 U-Boot 的上游 RK356x DTS](https://github.com/u-boot/u-boot/blob/e50b1e8715011def8aff1588081a2649a2c6cd47/dts/upstream/src/arm64/rockchip/rk356x-base.dtsi)：GICv3、UART2 地址/IRQ、PSCI、架构定时器。
- [PINE64 U-Boot 构建指南](https://pine64.org/documentation/Quartz64/Software/Building_U-Boot/)：RK3566 DDR、RK3568 BL31 组件及 SD 32 KiB 位置；板型表没有 Zero。
- [U-Boot RK356x 启动描述](https://github.com/u-boot/u-boot/blob/e50b1e8715011def8aff1588081a2649a2c6cd47/arch/arm/dts/rockchip-u-boot.dtsi)：binman 合并格式。
- [Rockchip RK3566 DDR 选择](https://github.com/rockchip-linux/rkbin/blob/3e288fe814e059dd06833495f845cab04ac20a5c/RKBOOT/RK3566MINIALL.ini) 与 [RK3568 BL31 选择](https://github.com/rockchip-linux/rkbin/blob/3e288fe814e059dd06833495f845cab04ac20a5c/RKTRUST/RK3568TRUST.ini)：固定二进制版本。
- [PINE64 发布页 Zero 专用下载](https://pine64.org/documentation/Quartz64/Software/Releases/)：原厂 SD 对照镜像及 SDCardTool。
- [ARM64 Image 协议](https://docs.kernel.org/arch/arm64/booting.html)：Image flags、加载位置和 DTB 入口。
- [Arm GIC 架构规范 IHI0069](https://developer.arm.com/documentation/ihi0069/latest/) 与 [TF-A GICv3 分组实现](https://github.com/TrustedFirmware-A/trusted-firmware-a/blob/v2.13.0/drivers/arm/gic/v3/gicv3_main.c)：安全/非安全中断归属边界。

- [Linux v6.12 DW-MMC 核心](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/dw_mmc.c)、[Rockchip 接入](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/dw_mmc-rockchip.c) 和 [RK3568 时钟](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/rockchip/clk-rk3568.c)：寄存器布局、FIFO、固定二分频和 gate/mux 事实参考；本仓库驱动独立实现，未复制上游 GPL 实现。

- [Linux v6.12 RK356x HDMI audio topology](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/rockchip/rk356x.dtsi)：`hdmi_sound` 的 CPU DAI 是 `i2s0_8ch`，codec DAI 是 DW-HDMI；模拟 RK817 音频使用独立 I2S1。

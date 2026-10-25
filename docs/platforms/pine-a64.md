# PINE A64 512 MiB 上板

本页对应原版 **PINE A64 512 MiB、Allwinner A64、DDR3**，实例为
`instances/pine-a64.toml`。A64+、PINE A64-LTS、PINE H64 不适用本平台配置。
当前实例为单核内核初始化配置，提供串口、MMU、GICv2、定时器和 PSCI 接口；
提供 MMC0 microSD 原生 PIO、固件 HDMI 帧缓冲接管和内部 codec PCM 播放驱动；
默认实例不包含用户态或持久 rootfs。
实机运行状态：未验证，不能将构建及寄存器模型测试视为上板成功。


## 串口交互 shell 配置

`pine-a64-shell` 实例嵌入静态链接的 `/bin/init`、`mksh` 和基本命令。
它采用完整用户态启动路径（`bringup=false`、`ramfs_user=true`），不需要根磁盘。
默认嵌入 17 个程序：`init mksh help ls cat ps sleep timer_preempt timer_idle`
和 `echo uname mkdir rm cp sync poweroff reboot`。启动时保留 RAMFS 的 `/bin`，
不会把含内核与 DTB 的启动 FAT 分区自动覆盖挂载到这里。
原 `pine-a64` 实例仍用于内核初始化验收，二者不能互换验收标准。

```sh
tools/a20 prepare pine-a64
tools/a20 build pine-a64-shell
tools/a20 package pine-a64-shell
tools/a20 console pine-a64-shell
```

SD 镜像是 `build/pine-a64/a20os-pine-a64-shell-sdcard.img`，部署时使用
`tools/a20 deploy pine-a64-shell --media-device /dev/实际卡设备`。
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
显示或音频的实机可用性。需要现有播放器时使用下述 media 实例；板上执行仍须实机验收。

## 适配内容和边界

| 项目 | 当前实现 |
| --- | --- |
| 启动协议 | 带 ARM64 Image 头的 `kernel.bin`，物理入口 `0x40080000`，U-Boot `booti`；EL2/EL1 入口归一到 EL1 |
| 内存 | 512 MiB 上限 `0x40000000..0x60000000`；必须读取固件 DTB 的 RAM，保护 DTB、memreserve 和 reserved-memory |
| 串口 | UART0 `0x01c28000`，DW APB UART，32 位访问、寄存器间距 4 字节，SPI 0 即 GIC IRQ 32 |
| 中断 | GIC-400/GICv2，GICD `0x01c81000`、GICC `0x01c82000` |
| 定时器 | 读 CNTFRQ 获取频率，非安全物理定时器 PPI 14 即 IRQ 30；A64 UNKNOWN1 计数器勘误过滤和 CVAL deadline |
| 电源 | TF-A 提供的 PSCI SMC 关机/重启；板级电源结果等待真机确认 |
| CPU | 只启用启动 CPU；`NR_CPUS != 1` 明确拒绝 |
| microSD / MMC0 | DT 枚举的 `allwinner,sun50i-a64-mmc`，通用 `sunxi-mmc` 驱动；SDSC/SDHC/SDXC、512 字节扇区、单块 PIO 读写和 flush，1-bit / 400 kHz；实机未验证 |
| HDMI | 保留 SPL/U-Boot DE2 → DW-HDMI 已建立的扫描输出，以 DT simple-framebuffer 接入 GPU/fbdev；没有内核原生 modesetting/热插拔驱动 |
| 模拟音频 | `sunxi-codec`：A64 I2S → codec AIF1 → 模拟耳机输出，DMA channel 0，48 kHz / stereo / S16_LE；含 AXP803 ELDO1 1.8 V 充电泵供电初始化；未实机验证 |
| 尚未实现 | EMAC、USB、HDMI 音频、音频录制/混音、通用 PMIC/DVFS、SMP；MMC DMA、4-bit/高速、eMMC、SDIO、热插拔 |

串口驱动保留 U-Boot 设置好的 115200 波特率除数、时钟、复位和 PB8/PB9
pinmux。不能直接绕过 SPL/TF-A/U-Boot 从复位入口进入该内核。
通用 AArch64 PL011 寄存器协议不适用于这块板。
为避免用户态直接读取有勘误的 CNTVCT，PINE A64 不发布通用 vDSO，时钟查询走内核。

内存分配器使用设备树 RAM 扣除 DTB 和固件保留区域后的多个有效区间。
日志中的 allocator RAM 可能少于 512 MiB，这是保护固件内存的结果。没有有效 DTB、板型不符、保留区域覆盖内核或 RAM 不足会停止启动。

默认实例 `bringup = true`，到 `System ready (bringup, no userspace)` 后执行一次
分配/释放检查，再打印 `part ok` 并请求关机；**没有交互 shell 或持久 rootfs**。
`console_check` 检查初始化里程碑和 `part ok`，真机验收还应确认关机结果。

## 1. 主机依赖与完整构建

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
tools/a20 check instances/pine-a64.toml
tools/a20 prepare pine-a64
tools/a20 package pine-a64
tools/a20 deploy pine-a64 --dry-run
```

`prepare` 首次下载约 53 MiB 的固定提交源码，校验仓库记录的 SHA256，构建固件与匹配
DTB，并将固定版本 pyelftools 0.32 安装到私有构建目录，供 U-Boot 的 binman 使用；
不修改主机 Python site-packages。源码与构建缓存约需 550 MiB，建议至少留 1 GiB 空间。
后续可重用下载缓存及编译输出。每次仍核对源码归档哈希并重新解包，避免把编辑过的缓存
源文件误当成固定版本。联网不可用时，保留已经下载完成且哈希正确的 source-cache 即可。

`package` 自动构建 A20OS、从准备好的固件目录取得 DTB、生成 MBR/FAT32 整卡镜像；
不需要 sudo、loop mount、手工建分区或复制文件。主要交付物：

| 路径 | 内容 |
| --- | --- |
| `build/pine-a64/firmware/` | BL31、SPL/U-Boot 一体固件、原版 DTB、宿主 mkimage、固件 manifest |
| `build/pine-a64/a20os-pine-a64-sdcard.img` | 可以写到空卡的 128 MiB 整盘镜像 |
| 镜像同名 `.img.json` | 镜像 SHA256、固件来源、内核与 FAT 文件哈希、布局 |
| `build/pine-a64/handoff/` | 同一个 ELF 产生的 Image、匹配 DTB、manifest 与救援命令文本 |

`handoff/kernel.bin` 是 ARM64 Image，不能当作整盘镜像写卡。
`handoff/uboot.cmd` 是 **U-Boot 命令文本**；整卡 FAT 根目录的 `boot.scr` 才是
mkimage 包装过、可由 U-Boot `source` 执行的脚本。

## 2. 准备启动固件与设备树

启动链为 A64 BootROM → U-Boot SPL（DDR 初始化）→ TF-A BL31 → U-Boot → A20OS。
上游 `pine64_plus_defconfig` 同时包含 pine64 与 pine64-plus；SPL 依据 512 MiB RAM
自动选中非 plus 的配置。这个 defconfig 名字里的 plus 不代表要改用 plus 的 DTB。

准备脚本 `tools/pine_a64.py prepare` 固定使用：

| 组件 | 固定来源/参数 |
| --- | --- |
| TF-A | v2.13.0，`c17351450c8a513ca3f30f936e26a71db693a145`，`PLAT=sun50i_a64 DEBUG=1 bl31` |
| U-Boot | v2025.10，`e50b1e8715011def8aff1588081a2649a2c6cd47`，`pine64_plus_defconfig`，`BL31=... SCP=/dev/null` |
| 额外命令功能 | `CONFIG_CRC32_VERIFY=y`、`CONFIG_BOOTMETH_SCRIPT=y`，用于启动脚本的完整性检查和扫描 |
| DTB | 从本次 U-Boot 输出取得 `sun50i-a64-pine64.dtb`，解析根 compatible 必须含 `pine64,pine64` |

准备过程不接受 A64+ 或 LTS 的 DTB 替代品；检查 SPL 的 `eGON.BT0` 标记和固件大小，
并将编译器版本、源归档哈希及每个固件文件的 SHA256 记录在 manifest。Crust SCP 对
这一阶段不是必需；挂起/恢复不在此次支持范围。
源码、版本、参数与布局可重复执行；宿主工具版本和 FAT 时间戳仍可能影响最终镜像哈希，
不把这个流程称作跨主机逐字节可复现的固件发行。

不经 a20 的等价入口为 `python3 tools/pine_a64.py prepare`；可用
`--cache`、`--work`、`--out`、`--cross-compile`、`--jobs` 自定义工作位置与工具链。

## 3. 准备 microSD

整卡镜像布局已经由脚本完成，不要再次分区/格式化：

| 镜像位置 | 内容 |
| --- | --- |
| LBA 0 | MBR；分区 1 为 bootable FAT32 LBA 类型 |
| 8 KiB / LBA 16 | `u-boot-sunxi-with-spl.bin`，包含 SPL、BL31、U-Boot |
| 8 MiB / LBA 16384 起 | FAT32，默认一直到 128 MiB 镜像末尾 |
| FAT 根目录 | `kernel.bin`、`sun50i-a64-pine64.dtb`、`boot.cmd`、`boot.scr` |

`boot.scr` 逐项检查 FAT 加载成功、字节数匹配、CRC32 匹配，然后才执行 `booti`。
校验值来自本次打包的内核/DTB；这些保护用于发现旧文件或损坏，不是签名安全启动。
脚本不执行 `saveenv`。打包还逐文件从 FAT 读回并比较 SHA256，检查内容实际进入分区。

确认读卡器的**整盘**设备及其分区均已卸载，再写卡：

```sh
tools/a20 deploy pine-a64 --media-device /dev/实际SD整盘设备 --no-console
```

实例里的 `/dev/sdX` 是故意保留的占位符，不是可直接使用的设备名。该命令会覆盖目标
卡的分区表和前 128 MiB 内容；首次使用专用空卡。镜像构建脚本只写普通文件，拒绝设备
和符号链接作为输出；只有单独的实例 deploy 写卡步骤才访问所选设备。
卡容量可大于镜像，余下空间暂不使用。U-Boot 从卡加载内核后，A20OS 的
`sunxi-mmc` 重新初始化 SD 卡并发布块设备；默认 bring-up 仍不挂载根文件系统。

## 4. 串口与首次启动

使用官方接线图核对 EXP 插座方向与板上丝印。EXP **7 = UART0_TX / PB8**、
**8 = UART0_RX / PB9**，6 或 9 为 GND；TX 接转接器 RX，RX 接转接器 TX，
GND 共地。用 3.3 V TTL USB 串口，不接转接器供电针，板子通过自己的 5 V 电源供电。
Pi-2 接口 8/10 是 UART2，不能替代当前默认 UART0 控制台。

将实例的 `/dev/ttyUSB0` 改为主机实际设备（优先用 `/dev/serial/by-id/...`），
串口参数为 115200、8N1。先执行：

```sh
tools/a20 console pine-a64 --interactive --serial /dev/serial/by-id/实际串口设备
```

把卡插入板子再上电。默认应看到 SPL、TF-A、U-Boot 自动扫描 FAT 上的 `boot.scr`，
打印 `A20OS PINE A64 SD boot` 并加载内核；此时不需要人工重复加载。
工具同时保存日志、检查内核标记。需要救援时，在同一终端按键打断 U-Boot autoboot，
可手工加载自带脚本以保留 size/CRC 检查：

```text
fatload mmc 0:1 ${scriptaddr} boot.scr
source ${scriptaddr}
```

也可用 `handoff/uboot.cmd` 中的逐行命令排查加载/设备树问题；该文本不自带 CRC 校验，
每次应先确认卡上的文件和本次交付物一致：

```text
setenv loadaddr 0x40080000
setenv fdt_addr_r 0x4fa00000
fatload mmc 0:1 ${loadaddr} kernel.bin
fatload mmc 0:1 ${fdt_addr_r} sun50i-a64-pine64.dtb
booti ${loadaddr} - ${fdt_addr_r}
```

任一加载命令失败即停止，先核对 `mmc list`、`mmc dev 0`、`fatls mmc 0:1`，
不要继续启动残留的旧内存。`0x4fa00000` 是独立 DTB 暂存区，不能与 Image 复用。
U-Boot `booti` 会对工作 DTB 填写检测到的 RAM，并按其启动流程修正和移动 DTB。
不设置 `fdt_high=0xffffffffffffffff`，也不把控制 DTB 地址误当成已修正的启动 DTB。
第一次不执行 `saveenv`，保留断电后重新从已知启动环境进入的恢复路径。

如果使用 TFTP，只将两个 `fatload` 换成 `tftpboot`，先设置 `serverip`、`ipaddr`；
这里使用的是 U-Boot 的网卡驱动，不能据此声称 A20OS 已支持板载网卡。

## 5. 实例部署与验收

首次建议先 `prepare`、`package`、`deploy --no-console` 写卡，然后再接入板子运行
`console --interactive` 并上电；卡上 `boot.scr` 自动启动，手动输入仅用于救援。
`tools/a20 deploy pine-a64 --interactive --media-device ...` 可把写卡和后续串口监听串起来，
操作员仍需在写完后移卡、上电。也可以用 picocom 等终端独立操作和记录日志。
串口只能由一个程序占用，启动 interactive 之前退出已有串口终端。

`console --interactive` 在全部 `console_check` 标记出现后退出；Ctrl-] 提前中止，
日志默认保存于 `.kernel-build/console/pine-a64.log`。120 秒启动超时包括人工输入时间，
准备阶段较长时复制一个本地实例，把 `target.boot_timeout` 调大。不要在内核已经启动完
之后才运行自动 console：它只能检查本次连接收到的输出，不能验证此前的历史日志。

启动检查：

1. 板型/版本、512 MiB 检测日志、固件版本、供电方式和主机串口节点。
2. U-Boot 的 `bdinfo`、成功加载两个文件的字节数以及 `Starting kernel ...`。
3. `[PINE A64] allocator RAM=... counter=... Hz (single CPU)`，确认 RAM 在预期范围内。
4. `[INIT] Timer initialized`、`[INIT] Memory initialized`、`System ready` 和 `part ok`。
5. PSCI 请求后是否确实掉电；若只是停在 WFI，关机驱动仍未完成验收。

默认 bring-up 的一次内存检查不证明长时间定时器精度、UART 接收中断可靠性或完整用户态。
定时器精度、串口压力与用户态运行需要各自的验证入口。
没有首屏输出时，先确认是 UART0 而非 UART2，核对 Image 地址、匹配 DTB、固件启动层级，
不要先把问题归因于 shell 或根文件系统。

## 内核 SD 驱动与验收范围

驱动入口为 `kernel/drivers/block/sunxi_mmc.c`，协议引擎为
`kernel/drivers/block/sunxi_mmc_core.c`。固件枚举器从 DT 创建设备；板级
`configure_devices` 仅为启用的 MMC0 节点配置 CCU 总线门控/复位、PF0..PF5
pinmux 和上拉，再附加电气参数，不另行发布静态 MMC 设备。MMC1/MMC2 不会被猜测启用。
A64 的固定二分频已计入 CCU 设置，新 timing 模式和 clock-update DAT0 mask
由控制器协议处理。板级不重新编程 AXP803，卡槽 3.3 V 供电沿用启动固件状态。

驱动支持 CMD0/8/55/ACMD41/2/3/9/7/16/13 初始化、CSD 容量与写保护解析，
用 CMD17/CMD24 逐扇区读写；SDSC 使用字节地址，SDHC/SDXC 使用扇区地址。
命令、FIFO、busy 和卡就绪等待均有时间与轮询上限。出现传输错误后停止接受后续
I/O，返回错误，需重新绑定初始化；失败探测撤销映射和私有状态。移除时先由公共
class 核心阻止新调用并排空在途调用，再关闭控制器。卡检测 GPIO、拔插重新枚举、
物理写保护开关、DMA、4-bit/高速调谐和电压切换尚未实现；不能热拔正在使用的卡。

生产代码宿主验证命令：

```sh
python3 -m unittest tools.tests.test_sunxi_mmc
```

测试覆盖 SDHC/SDSC 容量及地址、非对齐读写、多扇区拆分、SDXC 最大 LBA、CSD 写保护、
错误/超时、类适配和探测失败回滚；使用 ASan/UBSan。它不模拟真实管脚时序、供电和卡兼容性。
首次实机应保留完整 `[SUNXI-MMC]` 日志，核对容量与实际卡一致；该日志只表示卡初始化。
完成扇区内容与主机读回对比、受控写入及断电后读回以前，不宣称存储实机可用。
默认 bring-up 到达 `part ok` 也不构成 SD I/O 验收。

## HDMI 画面与耳机播放

`pine-a64-media` 使用相同启动链，额外嵌入已有的 `videoplay` 和 `audioplay`。
无需窗口管理器。主机关联的串口、写卡步骤与前文相同：

```sh
tools/a20 prepare pine-a64-media
tools/a20 package pine-a64-media -- -j2
tools/a20 deploy pine-a64-media --media-device /dev/实际SD整盘设备 --no-console
tools/a20 console pine-a64-media --serial /dev/ttyUSB0 --interactive
```

整卡镜像为 `build/pine-a64/a20os-pine-a64-media-sdcard.img`。开机前连接 HDMI
显示器，使用本页对应的原版 512 MiB 板和匹配固件。U-Boot 的 DE2/DW-HDMI
驱动选取实际模式，并在启动时向 DT 写入物理地址、宽高、stride 和格式。
A20 从 DT 接管已有扫描缓冲区，提前将其排除出页分配器；RAM 帧缓冲的内核
和用户映射使用一致的缓存属性，提交画面时清理缓存。没有有效交接时不创建
显示设备，也不假定固定分辨率。内核不负责重新初始化 HDMI、EDID 或恢复断开的显示器。

耳机播放使用原版板的 3.5 mm 输出。板级代码只从已枚举的 DT 资源组装
I2S、数字/模拟 codec、DMA、CCU 和 RSB 拓扑；通用音频驱动不包含板级物理地址。
RSB 依赖启动固件已初始化的总线和 AXP803 运行时地址；驱动校验芯片 ID，
只开启耳机充电泵所需的 ELDO1=1.8 V，不重设活跃的其他电源。
音频 PLL 被不同配置占用、DMA channel 0 已被使用或任一必要节点缺失时拒绝绑定。
目前没有可分配 DMA channel 的通用 broker，不能同时由另一个驱动占用 channel 0。

在板上检查日志、设备和现有播放器：

```sh
ls /dev
videoplay --test 320 240 30
audioplay --tone 440 --duration 1000
```

首条播放命令连续更新 10 秒测试画面后退出。RGB24 文件可用
`videoplay /tmp/clip.rgb 320 240 30` 播放；PCM WAV 可用 `audioplay /tmp/clip.wav`。
默认 RAM 镜像不含这些文件，也没有 MP4/H.264 解码或音画同步承诺。
驱动输出固定为 48 kHz 双声道 S16_LE，支持写入背压、尾部排空和欠载报错；
不是 HDMI 音频或麦克风录音接口。通过现有音频类接口提供 `/dev/audioN` 及 PCM 兼容入口。

主机可运行 `python3 -m unittest tools.tests.test_sunxi_audio`，执行实际生产驱动的
DMA/codec/供电/PLL 模型，覆盖错误和失败清理。通用 DT 帧缓冲另有 QEMU 实际像素检查。
这些测试不替代板上的 HDMI 首屏、持续更新、耳机波形、DMA 缓存一致性和中断延迟验收。
目前 PINE 实机显示及发声均未验证。

音频寄存器与拓扑依据：
[Linux v6.17 I2S](https://github.com/torvalds/linux/blob/v6.17/sound/soc/sunxi/sun4i-i2s.c)、
[A64 数字 codec](https://github.com/torvalds/linux/blob/v6.17/sound/soc/sunxi/sun8i-codec.c)、
[模拟 codec](https://github.com/torvalds/linux/blob/v6.17/sound/soc/sunxi/sun50i-codec-analog.c)、
[sun6i DMA](https://github.com/torvalds/linux/blob/v6.17/drivers/dma/sun6i-dma.c)。
显示交接依据 [U-Boot v2025.10 DE2](https://github.com/u-boot/u-boot/blob/v2025.10/drivers/video/sunxi/sunxi_de2.c)。
上述公开实现用于核对寄存器与协议，没有将 Linux GPL 实现链接进 A20 内核。

## 依据

- [PINE64 官方板型、内存和接口说明](https://pine64.org/documentation/Pine_A64/_full/)：原版及 LTS 区别、512 MiB 版本。
- [PINE64 官方引脚分配 PDF，第 3 页](https://files.pine64.org/doc/Pine%20A64%20Schematic/Pine%20A64%20Pin%20Assignment%20160215.pdf)：EXP UART0 接线。
- [Linux v6.17 A64 SoC DTS](https://github.com/torvalds/linux/blob/v6.17/arch/arm64/boot/dts/allwinner/sun50i-a64.dtsi)：UART/GIC/定时器地址和 IRQ、PSCI SMC。
- [Linux v6.17 原版 PINE64 DTS](https://github.com/torvalds/linux/blob/v6.17/arch/arm64/boot/dts/allwinner/sun50i-a64-pine64.dts)：板型 compatible、UART0 115200、PB8/PB9。
- [Linux v6.17 架构定时器驱动](https://github.com/torvalds/linux/blob/v6.17/drivers/clocksource/arm_arch_timer.c)：`sun50i_a64_read_cntpct_el0` 与 UNKNOWN1 deadline 修正。
- [U-Boot v2025.10 defconfig](https://github.com/u-boot/u-boot/blob/v2025.10/configs/pine64_plus_defconfig) 和 [sunxi 板级选择](https://github.com/u-boot/u-boot/blob/v2025.10/board/sunxi/board.c)：512 MiB 自动选择原版 DT。
- [U-Boot 官方 Allwinner 启动固件指南](https://docs.u-boot.org/en/latest/board/allwinner/sunxi.html)：SPL/TF-A 集成与 SD 写入位置。
- [TF-A 官方 Allwinner 平台说明](https://trustedfirmware-a.readthedocs.io/en/latest/plat/allwinner.html)：`sun50i_a64`、BL31、PSCI 与可选 SCP。
- [U-Boot booti 协议](https://docs.u-boot.org/en/latest/usage/cmd/booti.html)：ARM64 Image 和独立 FDT 参数。

- [Allwinner A64 User Manual，SMC 与 CCU 章节](https://files.pine64.org/doc/datasheet/pine64/Allwinner_A64_User_Manual_V1.0.pdf)：寄存器、命令/FIFO 状态及电气时钟配置。
- [Linux v6.17 A64 CCU](https://github.com/torvalds/linux/blob/v6.17/drivers/clk/sunxi-ng/ccu-sun50i-a64.c) 与 [sunxi MMC](https://github.com/torvalds/linux/blob/v6.17/drivers/mmc/host/sunxi-mmc.c)：A64 固定后分频、新 timing、DAT0 mask 和 sample delay。
- [U-Boot v2025.10 sunxi MMC](https://github.com/u-boot/u-boot/blob/v2025.10/drivers/mmc/sunxi_mmc.c)：PIO 寄存器行为交叉核对。
- [SD Association 简化规范](https://www.sdcard.org/downloads/pls/)：SD 初始化、CSD、块寻址和写保护协议。

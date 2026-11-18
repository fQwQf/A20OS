# Sipeed LicheeRV Nano / SG2002-E

当前提供单核 S-mode 内核初始化配置，使用 Sv39 MMU、UART 和 SBI 定时器。实机运行状态：未验证。`licheerv-nano` 与 `licheerv-nano-e` 默认生成可写入空卡的 MBR/FAT/FIP 镜像，使用同一个`BOARD=licheerv-nano` 内核；`licheerv-nano-e-tftp` 另保留已验证厂商 E/WE 固件的可选网络救援方案。主线默认 Nano B 设备树只用于共同的 SD/UART 路径，不据此声明 E/WE 网口已适配。


## 串口交互 shell 配置

`licheerv-nano-shell` 实例嵌入静态链接的 `/bin/init`、`sh`（BusyBox）和基本命令。它采用完整用户态启动路径（`bringup=false`、`ramfs_user=true`），不需要根磁盘。默认嵌入 BusyBox 与基础系统命令。启动时保留 RAMFS 的 `/bin`，不会把含内核与 DTB 的启动 FAT 分区自动覆盖挂载到这里。原 `licheerv-nano` 实例仍用于内核初始化验收，二者不能互换验收标准。

```sh
tools/a20 prepare licheerv-nano
tools/a20 build licheerv-nano-shell
tools/a20 package licheerv-nano-shell
tools/a20 console licheerv-nano-shell
```

SD 镜像是 `build/licheerv-nano/a20os-licheerv-nano-shell-sdcard.img`，部署时使用`tools/a20 deploy licheerv-nano-shell --media-device /dev/实际卡设备`。打包会重新生成所选实例对应的内核与 handoff，公共 handoff 目录不得混用旧包。

串口接线、电平、波特率和固件要求沿用本文。`System ready` 只表示内核就绪；验收还须看到 init 启动 `sh`，实际输入并检查输出：

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

文件系统为 RAM-only，重启后丢失写入。最小用户构建仅编译嵌入命令，不构建 fastfetch、FFmpeg 或完整发行版。这个配置不包含 SSH 服务，也不承诺网卡、显示或音频可用。当前状态为交叉构建与封装检查；板上执行仍须实机验收。

## 硬件与系统支持

| 项 | 当前依据与处理 |
|---|---|
| CPU | SG2002 主核 C906 1 GHz（与 A53 二选一），另有 700 MHz C906 与低功耗 8051；本移植只启用固件交出的主核 |
| 内存 | 256 MiB DDR3，物理窗口 `0x80000000..0x90000000`；内核装载到 `0x80200000` |
| ISA/MMU | RV64IMAFDC，Sv39；内核用 LP64 软件浮点 ABI，无须厂商向量扩展 |
| UART0 | DW APB UART，`0x04140000`，PLIC IRQ 44，**32 位读写、寄存器间距 4 字节** |
| 时间 | 固件 DTB timebase 优先，回退 25 MHz；定时器与关机通过 OpenSBI |
| 中断 | T-Head C900 PLIC，`0x70000000`；当前只覆盖电平中断路径 |
| SD | `0x04310000`，IRQ 36，SDHCI/DWCMSHC；`sdhci` 块驱动提供 1-bit、3.3 V、最高 25 MHz 的 PIO 读写，尚未实机验证 |
| 网络 | E/WE 有 100M 网口；U-Boot TFTP 能力不等于 A20OS 已有 SG2002 网卡驱动 |
| 显示 | 板载 31-pin、2-lane MIPI DSI 接口；没有 HDMI。A20OS 尚无 SG2002 显示控制器、DSI PHY 和屏幕初始化驱动 |
| 音频 | 原生 `sg2002-audio`：I2S3 → 内部 DAC → 板载功放，48 kHz stereo S16_LE PCM；板上单功放仅接右路，物理输出为 mono。麦克风录音未实现；实机未验证 |

UART 使用 32 位寄存器访问与 4 字节间距，保留 FSBL/U-Boot 设置的时钟、引脚和 115200 波特率。内核采用标准 RISC-V Image 头，由 U-Boot 以 `booti KERNEL - DTB` 启动；第三参数必须为独立 DTB，`-` 只表示不使用 initrd。

### 显示和声音的设备条件

Nano 的板载 DAC/功放已有原生播放驱动。显示口仍需要具体的 MIPI 屏幕、转接板和排线；不同屏幕的 DCS 初始化、 lane 时序、复位与背光供电不能由通用 framebuffer 推断。[厂商规格及接线入口](https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/1_intro.html)列出了显示接口、板载功放和不同型号的配置。

启动固件的 mainline DT 原本没有完整音频节点；prepare 会加入本仓库的`tools/firmware/licheerv-nano-audio.dtsi`，描述真实 I2S3、DAC、DMA 和系统控制资源，并把该输入的摘要记入固件 manifest。板级代码验证实际枚举的资源再附加时钟/缓存策略，通用 PCM 驱动不含 RISC-V 指令或板级物理地址。没有初始化具体屏幕，不发布虚假的 `/dev/fb0`。

## 板载功放 PCM

使用包含现有播放器的独立 RAMFS 实例：

```sh
tools/a20 prepare licheerv-nano-media
tools/a20 package licheerv-nano-media -- -j2
tools/a20 deploy licheerv-nano-media --media-device /dev/实际SD整盘设备 --no-console
tools/a20 console licheerv-nano-media --serial /dev/ttyUSB0 --interactive
```

镜像为 `build/licheerv-nano/a20os-licheerv-nano-media-sdcard.img`。进入串口 shell 后运行 `audioplay --tone 440 --duration 1000`，或将适合的 PCM WAV 交给 `audioplay /tmp/clip.wav`。默认镜像不含外部媒体文件，也不提供网络登录。

PCM 接口固定为 48 kHz 双声道 S16_LE；Nano rev1.4 的单功放 U11 仅连接 DAC 右路，实际扬声器输出为 mono，不要据此接口格式推断板上有两个声道。功放 EN 经 R36 上拉固定使能，没有可用的 GPIO mute；静音通过内部 DAC 数字路径控制。驱动不假定不同扩展板的功放或屏幕接线。

DMA 使用 SG200x 的 AXI 风格寄存器和 64-byte LLI，不能套用通常的 DW AHB 布局。 channel 7、request 7、master 1/1 承担播放，32 KiB PCM 环形缓冲按 4 KiB 周期中断；每个微块不超过 64 byte，512 个 LLI 另占 32 KiB。支持背压、尾部排空、停止与错误回报。停止无法确认时撤销中断回调，仅隔离仍可能被硬件读取的 DMA 存储。不提供通用 DMA channel 分配器，不能让其他驱动同时占用 channel 7。

板级逻辑读取并验证共享 A0PLL，选择能精确得到 24.576 MHz 的 AUD3 分频；不重新调节同时供给 C906B 的 PLL。内部 DAC 使用厂商明确的 S16 stereo 特殊 framing，不将文件名 `cv1835-i2s` 当作 CV1835 codec 的寄存器兼容承诺。

C906 DMA 缓存维护在设备 probe 前授权：核对 SG2002/C906 和 SBI vendor ID，再执行可精确恢复的 S-mode 指令探针。只有指定 PC 的非法指令可恢复，普通异常保持原行为。成功后，CPA/CIPA 以物理地址维护 64-byte 缓存行，并完成同步屏障。权限或型号不满足时，DMA mask/分配接口拒绝启用，音频不发布；串口与 PIO SD 继续工作。它不等于其他 RISC-V 板型的非一致性 DMA 已经完成。

生产代码模型可执行：

```sh
python3 -m unittest tools.tests.test_sg2002_audio tools.tests.test_riscv_dma_cache \
  tools.tests.test_dma_arch_readiness
```

检查覆盖 PCM 队列、环末边界、LLI、缓存物理地址、精准异常恢复、失败清理和未就绪 DMA 拒绝。模型与交叉构建不能验证板上 S-mode 权限、真实 DMA 握手、声学输出或供电时序，尚无实机发声记录。

## 从空卡准备完整产物

Linux 主机需要两套 RISC-V 工具链：内核使用裸机 ELF 工具链，固件使用 Linux GNU 工具链。Debian/Ubuntu 安装：

```sh
sudo apt-get install build-essential git curl ca-certificates python3 \
  gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
  gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu libc6-dev-riscv64-cross \
  bison flex swig libssl-dev libgnutls28-dev python3-dev python3-pip \
  python3-setuptools python3-yaml dosfstools mtools
```

安装 rustup 后配置仓库指定的 Rust 1.96.0；若尚无 rustup，可先执行下面前两条命令。已有 rustup 时可跳过安装器：

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 riscv64imac-unknown-none-elf riscv64gc-unknown-none-elf
```

下列仓库命令均在 A20OS 源码根目录执行；首次构建先运行 `git submodule update --init --recursive`。主机 Python 必须为 3.11 或更新版本，可用 `python3 --version` 确认。首次下载源码、Rust 组件和 Cargo 依赖需要联网。

在仓库根目录执行：

```sh
tools/a20 check instances/licheerv-nano.toml
tools/a20 prepare licheerv-nano
tools/a20 package licheerv-nano
# E/WE 使用相同 SD/UART 早期固件：
tools/a20 package licheerv-nano-e
```

`prepare` 下载固定提交的源码归档并验证 SHA256，构建主线 U-Boot/OpenSBI，再调用官方 sophgo/fiptool，将厂商 FSBL、DDR 参数、OpenSBI、U-Boot 合成为 FAT 中的 `fip.bin`。 pyelftools 0.32 安装到私有构建目录，不修改主机 Python site-packages。默认缓存/构建约需 600 MiB，建议至少留 1 GiB 空间；归档缓存有效时不用再次下载。

| 组件 | 固定来源与构建契约 |
| --- | --- |
| U-Boot | v2025.10，`e50b1e8715011def8aff1588081a2649a2c6cd47`，`sipeed_licheerv_nano_defconfig` |
| OpenSBI | v1.7，`a32a91069119e7a5aa31e6bc51d5e00860be3d80`，`PLATFORM=generic FW_FDT_PATH=<本次 U-Boot DTB>` |
| fiptool / FSBL | sophgo/fiptool，`7f59889c91f7d5d440d6a09aad0209f0aca3d09d`，SG2002 使用 **cv181x.bin** |
| DDR 参数 | 同一 fiptool 提交的 `data/ddr_param.bin` |
| 二核 RTOS | 显式传入空文件，生成后解析 FIP 确认 `BLCP_2ND_SIZE=0`，不加载默认 cvirtos |
| DTB | `sg2002-licheerv-nano-b.dtb`，根 compatible 含 Nano/SG2002，memory reg 必须为 `0x80000000,0x10000000` |

FSBL/DDR 参数来自厂商二进制，不能把这条链称作全部源码可构建。归档 SHA256、编译器版本、实际每个产物的 SHA256 和厂商许可文件都保存在 `build/licheerv-nano/firmware/`。固定来源、参数和布局可重跑，主机工具版本/FAT 时间戳仍可能改变最终镜像哈希；不声称跨主机逐字节一致。

U-Boot 只增加 `CONFIG_CRC32_VERIFY=y` 与 `CONFIG_BOOTMETH_SCRIPT=y` 命令能力，另将默认`scriptaddr` 从 **0x80c00000** 移到 **0x8e000000**，`kernel_addr_r` 改为 `0x80200000`。旧脚本地址可能落入 A20OS Image/BSS，加载内核会覆盖仍在执行的脚本。完整内存范围随构建变化，以本次 bundle manifest 的 `image_size` 为准；外置启动 DTB 放`0x82000000`，打包器检查它与整个 Image（含 BSS）分离。打包检查 Image footprint 未触及 DTB；固件构建检查 U-Boot ELF、malloc 和额外栈/GD/FDT 空间的保守预算不超过 DDR 顶部 16 MiB。实际 `bdinfo` 的重定位/堆栈仍需首板核对，不能仅凭固定地址断言任意厂商固件均使用同一布局。

主要输出：

| 路径 | 内容 |
| --- | --- |
| `build/licheerv-nano/firmware/` | fip.bin、匹配 DTB、OpenSBI、U-Boot、vendor FSBL/DDR 参数、mkimage、manifest |
| `build/licheerv-nano/a20os-licheerv-nano-sdcard.img` | 默认 128 MiB 的完整 MBR/FAT32 整盘镜像 |
| 镜像旁 `.img.json` | 固件来源/布局与各文件、整镜像 SHA256 |
| `build/licheerv-nano/handoff/` | 同一 ELF 产生的 Image、匹配 DTB、manifest 和手动救援文本 |

两个默认实例共用板级产物目录。`handoff/kernel.bin` 是内核 Image，不能直接当整盘镜像写卡。`handoff/uboot.cmd` 是命令文本；FAT 中 `boot.scr` 才是 mkimage 包装的二进制脚本。`tools/boards/licheerv_nano.py prepare/sdcard` 是等价私有入口，支持自定义缓存、输出、交叉工具前缀与并行数。

## 写空卡与自动启动

镜像构建不需要 root 或 loop mount，自动创建 bootable MBR FAT32 分区，从 8 MiB 开始：

```text
LBA 0                    MBR
8 KiB 等 raw 区域         保持零，不写 Allwinner SPL
8 MiB 起的 FAT 根目录      fip.bin
                         kernel.bin
                         sg2002-licheerv-nano-b.dtb
                         boot.cmd / boot.scr
```

SG2002 BootROM 从 FAT 文件加载 **fip.bin**，不是从 PINE A64 的 8 KiB 固件偏移取 SPL。构建逐文件从 FAT 读回并比对 SHA256。默认 U-Boot `run distro_bootcmd` 扫描 `boot.scr`；脚本检查内核和 DTB 的加载状态、字节数和 CRC32，全部通过才 `booti`，失败退出、不 `saveenv`。这些是旧文件/损坏检测，不是签名安全启动。U-Boot 读 FAT 与内核驱动是独立路径；内核接管后由 `sdhci` 驱动重新初始化卡，必须以实际读写结果验收。

确认待写卡的**整盘设备**和所有分区已卸载，再执行：

```sh
tools/a20 deploy licheerv-nano --media-device /dev/实际SD整盘设备 --no-console
```

`/dev/sdX` 是实例占位符。写卡会覆盖该卡分区表及前 128 MiB 内容，首次使用专用空卡，保留原来的厂商工作卡作为恢复路径。卡可以大于镜像，余下空间目前不用。构建脚本只接受普通文件输出，不会打开真实块设备。

## 串口启动与恢复

接 3.3 V USB-UART，板 TX 接适配器 RX，板 RX 接适配器 TX，共地，115200 8N1；根据官方引脚图使用 UART0，USB-C 供电口不是本移植的串口。在上电前打开同一条可交互且有断言的连接：

```sh
tools/a20 console licheerv-nano --interactive --serial /dev/serial/by-id/实际串口设备
```

将写好的卡插入板再上电，观察 FSBL → OpenSBI → U-Boot → `A20OS LicheeRV Nano SD boot` → 内核。正常自动启动无需键入 handoff。若失败，打断 U-Boot autoboot，先检查`mmc list`、`part list mmc 0`、`bdinfo`、`printenv scriptaddr fdt_addr_r`，再逐行恢复：

```text
fatload mmc 0:1 ${scriptaddr} boot.scr
source ${scriptaddr}
```

该方式保留文件大小/CRC 校验。进一步排查可用 handoff 文本对应的显式装载：

```text
setenv loadaddr 0x80200000
setenv fdt_addr_r 0x82000000
fatload mmc 0:1 ${loadaddr} kernel.bin
fatload mmc 0:1 ${fdt_addr_r} sg2002-licheerv-nano-b.dtb
fdt addr ${fdt_addr_r}
fdt print /memory@80000000
booti ${loadaddr} - ${fdt_addr_r}
```

显式命令本身不带自动 CRC 检查，任一步失败立即停止。`mmc 0:1` 按实际卡布局核对。`booti` 第三参数始终是独立可写的启动 DTB，不能直接用控制 DTB 的 malloc 区，也不能用内核地址；`go` 的 argc/argv ABI 不满足内核要求。

## E/WE 厂商固件 TFTP 可选路径

默认主线 Nano B 设备树没有启用 E/WE 网口，不能因为编译了 MAC 驱动就假设 TFTP 可用。只有先用 `help tftpboot`、`printenv ethact`、`ping` 确认原厂 E/WE 固件可用网口后，才使用：

```sh
tools/a20 package licheerv-nano-e-tftp
```

这是 kernel-bundle，不重建空卡固件。下面以主机 `192.168.50.10`、板子 `192.168.50.20`为例；替换为同一实际 LAN 的地址，主机地址必须已配置到连接板子的网卡，板子地址应未被占用。不要随后再打包另一实例：同一 handoff 目录会更新为另一个实例的 manifest。

终端 1 启动只读服务：

```sh
sudo python3 tools/cli/a20_tftp.py --bundle "$PWD/build/licheerv-nano/handoff" \
  --bind 192.168.50.10 --port 69
```

服务在打开 UDP 端口前验证 manifest 中 `kernel.bin` 的大小和 SHA256，只提供这一个文件的不可变快照；拒绝上传、其他路径和非 octet 模式。UDP 69 通常需要绑定权限，因此示例用 sudo。确认该地址上的 UDP 69 未被其他 TFTP 服务占用，主机防火墙允许板子访问 TFTP 及其响应端口。每次重新 `package` 后停止旧服务再启动，避免继续提供旧快照。Ctrl-C 退出服务。

终端 2 在板子上电之前运行自动串口交接：

```sh
tools/a20 console licheerv-nano-e-tftp --auto-handoff \
  --serial /dev/serial/by-id/实际串口设备 \
  --server-ip 192.168.50.10 --board-ip 192.168.50.20
```

该命令在同一串口连接打断 U-Boot，设置易失的 `serverip`/`ipaddr`，验证每条命令状态、实际下载字节数/CRC32 和 FDT 头，最后检查内核日志。服务本身不打开串口、不会写板载 Flash。板子仍需已具备可用的厂商 E/WE 网络固件；看到明确 U-Boot 提示符之后才会发送装载指令。

也可用 `console --interactive` 手动输入；与生成的 recipe 一致的完整地址设置为：

```text
setenv serverip 192.168.50.10
setenv ipaddr 192.168.50.20
setenv loadaddr 0x80200000
setenv fdt_addr_r 0x82000000
tftpboot ${loadaddr} kernel.bin
fdt move ${fdtcontroladdr} ${fdt_addr_r} 0x20000
fdt addr ${fdt_addr_r}
fdt print /memory@80000000
booti ${loadaddr} - ${fdt_addr_r}
```

克隆空间上限为 128 KiB，`fdt move` 失败就停止；先确认 `0x82000000..0x82020000` 未被厂商固件/小核 RTOS 保留。控制 DTB 必须包含真实 RAM 和保留区，不能把被裁剪的控制树当完整启动树。也可 TFTP 加载单独核对过的匹配 DTB。带二核 RTOS/多媒体固件的厂商卡，必须提供它们实际占用 DDR 的 reserved-memory；不能沿用空 RTOS 的内存假设。TFTP 成功只证明 U-Boot 网卡能力， A20OS 仍没有 SG2002 Ethernet 驱动。

## 验收与局限

```sh
tools/a20 console licheerv-nano --interactive
```

在内核启动**之前**开始监听，确保同时捕获：

- `[FDT] RAM range`：有效 DTB 到达内核并收窄内存范围。
- `System ready (bringup, no userspace)`：完成内核初始化。
- `part ok`：最小分配/释放成功。
- `System is going down for power-off NOW`：进入 SBI 关机路径；还需实际确认板子已停止运行。

当前实例 `bringup=true`、`RAMFS_USER=0`，会自动关机，**没有 shell**；旧版本的 `cat /etc/os-release`与 `poweroff` 串口命令并不适用于这个配置。`System ready` 本身也不证明进入用户态。

SDHCI 已有独立的 SD 卡块驱动，支持 SDSC、SDHC/SDXC 的容量解码和单扇区命令组合读写；不支持 eMMC、SDIO、SDUC、1.8 V/UHS、4-bit、DMA、热插拔自动重枚举或睡眠恢复。传输采用轮询 PIO，无需把普通内存交给非一致性 DMA，也不依赖 SD IRQ；中断描述错误仍由公共枚举层拒绝。驱动只在初始化完整成功后发布块设备，所有寄存器等待都有截止时间；遵守 CSD 永久/临时写保护、固件 `read-only` 与内核只读存储配置。传输错误后停止后续 I/O，重新绑定驱动才能重新初始化卡，避免把未知卡状态当作可以继续传输。

板级 `arch/riscv64/boards/licheerv-nano/storage.c` 为公共 DT 设备添加平台机制：开启 SD0 时钟门、明确选择 25 MHz 晶振旁路，配置 SD0 引脚复用与默认速率 PHY；保留本页 SD 启动 U-Boot 已建立的 3.3 V pad 电压与驱动强度，不支持从任意 1.8 V 固件状态接管。协议代码位于`kernel/drivers/block/sdhci_core.c`，绑定/class/受管资源位于 `sdhci.c`。支持旧 U-Boot 的`sophgo,cv1800b-dwcmshc` 和新 Linux 的 `sophgo,sg2002-dwcmshc`，不会匹配 dw-mci。当前板层仅配置 SD0，其他 SDHCI 实例仍不具备板级电源和引脚支持。

`python3 -m unittest tools.tests.test_sdhci` 对生产协议引擎和两板寄存器机制运行宿主硬件模型，覆盖分频、命令/数据错误、超时、非对齐缓冲区、SDSC 字节地址及 class 绑定生命周期。它不能验证真实电气时序。实机需要先读取镜像已知扇区比对哈希，再在专用测试分区做写入/读回；仅看到 `[SDHCI] ... sectors` 不足以宣布存储验证通过。

目前仍不提供 SG2002 网卡、通用 DMA broker、NPU、视频或第二核驱动。音频 DMA 与 C906 缓存维护已有专门实现，其他 DMA 设备仍需自己的所有权和地址约束。 PLIC 的边沿中断扩展也未覆盖；SBI TIME/SRST 和 T-Head MMU/cache 设置需实机记录。

内存解析现在检查 DTB v17 的 RAM 描述、`memreserve` 与 `/reserved-memory/reg`，也保留 DTB 缓冲区本身。公共分配器保留被保留区分割后的多个可用区间，并限制在板级 256 MiB 物理窗口内；实际可用容量会扣除固件占用。保留区与内核重叠、坏 DTB、动态 `size` 保留或无法转换的 `ranges` 会停止启动，避免继续分配固件内存。缺失 DTB 仍有板级窗口回退，但不计入本实例的成功验收。厂商多媒体/小核固件可能占用 DDR，必须给出与实际固件匹配的保留区；不能把任意 Linux DTB 改成 256 MiB 后直接使用。没有写进 DTB 的固件占用无法由内核推断。

实机运行状态尚未验证；SBI TIME/SRST、PLIC 路由、RAM 保留区和断电行为以实际固件及板卡为准。

## 参考资料

- [Sipeed 板卡规格与 B/E/W/WE 区别](https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/1_intro.html)
- [Sipeed 官方 SDK](https://github.com/sipeed/LicheeRV-Nano-Build)
- [Nano 70418 官方原理图 rev1.4](https://dl.sipeed.com/fileList/LICHEE/LicheeRV_Nano/02_Schematic/LicheeRV_Nano-70418_Schematic.pdf)：第 4 页单路功放、DAC 右路与固定上拉 EN。
- [固定 SDK 音频实现](https://github.com/sipeed/LicheeRV-Nano-Build/tree/d4003f15b35d43ad4842f427050ab2bba0114fa5/linux_5.10/sound/soc/cvitek)、[DMA 实现](https://github.com/sipeed/LicheeRV-Nano-Build/tree/d4003f15b35d43ad4842f427050ab2bba0114fa5/linux_5.10/drivers/dma/cvitek)和 [RISC-V 缓存维护](https://github.com/sipeed/LicheeRV-Nano-Build/tree/d4003f15b35d43ad4842f427050ab2bba0114fa5/linux_5.10/arch/riscv/mm)：寄存器和协议事实依据；A20 内核代码为独立实现。
- [U-Boot v2025.10 T-Head CMO](https://github.com/u-boot/u-boot/blob/v2025.10/arch/riscv/lib/thead_cmo.c)：使用物理地址的缓存指令编码。
- [Linux SDHCI/DWCMSHC 的 CV18xx/SG2002 PHY reset 与设备匹配](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-of-dwcmshc.c)
- [Linux CV18xx 时钟寄存器](https://github.com/torvalds/linux/blob/master/drivers/clk/sophgo/clk-cv1800.c)和 [SG2002 引脚寄存器](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/sophgo/pinctrl-sg2002.c)
- [U-Boot v2025.10 的 CV1800B SDHCI](https://github.com/u-boot/u-boot/blob/v2025.10/drivers/mmc/cv1800b_sdhci.c)
- [Linux SG2002 DTS](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/sophgo/sg2002.dtsi)与 [CV180x 外设 DTS](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/sophgo/cv180x.dtsi)
- [U-Boot 官方 Nano FSBL/OpenSBI/FAT FIP 启动指南](https://docs.u-boot.org/en/v2025.10/board/sophgo/licheerv_nano.html)
- [Sophgo 官方 fiptool、cv181x FSBL、DDR 参数](https://github.com/sophgo/fiptool/tree/7f59889c91f7d5d440d6a09aad0209f0aca3d09d)
- [U-Boot 正确板名与默认 Nano B DTB](https://github.com/u-boot/u-boot/blob/master/configs/sipeed_licheerv_nano_defconfig)
- [U-Boot booti 头校验](https://github.com/u-boot/u-boot/blob/master/arch/riscv/lib/image.c)与 [a0/a1 交接实现](https://github.com/u-boot/u-boot/blob/master/arch/riscv/lib/bootm.c)
- [RISC-V 64 字节 Image 头协议](https://docs.kernel.org/arch/riscv/boot-image-header.html)

以上参考资料用于核对 SoC 硬件寄存器、引脚复用与固件启动协议。

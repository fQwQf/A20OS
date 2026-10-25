# PINE A64 512 MiB 上板

本页对应原版 **PINE A64 512 MiB、Allwinner A64、DDR3**，实例为
`instances/pine-a64.toml`。A64+、PINE A64-LTS、PINE H64 不适用本平台配置。
当前实例为单核内核初始化配置，提供串口、MMU、GICv2、定时器和 PSCI 接口；
不包含用户态、持久 rootfs 或板载存储/网络驱动。实机运行状态：未验证。

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
| 尚未实现 | sunxi MMC、EMAC、USB、HDMI、音频、PMIC 驱动、SMP |

串口驱动保留 U-Boot 设置好的 115200 波特率除数、时钟、复位和 PB8/PB9
pinmux。不能直接绕过 SPL/TF-A/U-Boot 从复位入口进入该内核。
通用 AArch64 PL011 寄存器协议不适用于这块板。
为避免用户态直接读取有勘误的 CNTVCT，PINE A64 不发布通用 vDSO，时钟查询走内核。

内存分配器目前使用单段连续 RAM，因此遇到内核后方的第一处固件保留区域时，
保守地舍弃其后的 RAM。日志中的 allocator RAM 可能少于 512 MiB，这是保护固件
内存的结果。没有有效 DTB、板型不符、保留区域覆盖内核或 RAM 不足会停止启动。

默认实例 `bringup = true`，到 `System ready (bringup, no userspace)` 后执行一次
分配/释放检查，再打印 `part ok` 并请求关机；**没有交互 shell 或持久 rootfs**。
`console_check` 检查初始化里程碑和 `part ok`，真机验收还应确认关机结果。

## 1. 主机依赖与完整构建

安装仓库构建指南要求的 AArch64 交叉工具链、Rust targets 和依赖。固件准备还需要
GNU make、宿主 C 编译器、bison、flex、swig、OpenSSL/GnuTLS 开发头文件、Python 开发
头文件、pip/setuptools/PyYAML；整卡组装需要 dosfstools 与 mtools。
Debian/Ubuntu 对应包名示例：

```sh
sudo apt install build-essential gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
  bison flex swig libssl-dev libgnutls28-dev python3-dev python3-pip \
  python3-setuptools python3-yaml dosfstools mtools
```

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
卡容量可大于镜像，余下空间暂不使用。内核尚无 MMC 驱动，并不妨碍 U-Boot 从卡加载
内核；内核接管后无法继续访问卡。

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

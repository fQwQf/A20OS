# Sipeed LicheeRV Nano / SG2002-E

当前提供单核 S-mode 内核初始化配置，使用 Sv39 MMU、UART 和 SBI 定时器。实机运行状态：未验证。
`licheerv-nano` 与 `licheerv-nano-e` 默认生成可写入空卡的 MBR/FAT/FIP 镜像，使用同一个
`BOARD=licheerv-nano` 内核；`licheerv-nano-e-tftp` 另保留已验证厂商 E/WE 固件的可选网络救援方案。
主线默认 Nano B 设备树只用于共同的 SD/UART 路径，不据此声明 E/WE 网口已适配。

## 硬件与系统支持

| 项 | 当前依据与处理 |
|---|---|
| CPU | SG2002 主核 C906 1 GHz（与 A53 二选一），另有 700 MHz C906 与低功耗 8051；本移植只启用固件交出的主核 |
| 内存 | 256 MiB DDR3，物理窗口 `0x80000000..0x90000000`；内核装载到 `0x80200000` |
| ISA/MMU | RV64IMAFDC，Sv39；内核用 LP64 软件浮点 ABI，无须厂商向量扩展 |
| UART0 | DW APB UART，`0x04140000`，PLIC IRQ 44，**32 位读写、寄存器间距 4 字节** |
| 时间 | 固件 DTB timebase 优先，回退 25 MHz；定时器与关机通过 OpenSBI |
| 中断 | T-Head C900 PLIC，`0x70000000`；当前只覆盖电平中断路径 |
| SD | `0x04310000`，IRQ 36，SDHCI/DWCMSHC；现有 A20 dw-mci 驱动不能代用 |
| 网络 | E/WE 有 100M 网口；U-Boot TFTP 能力不等于 A20OS 已有 SG2002 网卡驱动 |

UART 使用 32 位寄存器访问与 4 字节间距，保留 FSBL/U-Boot 设置的时钟、引脚和 115200 波特率。
内核采用标准 RISC-V Image 头，由 U-Boot 以 `booti KERNEL - DTB` 启动；
第三参数必须为独立 DTB，`-` 只表示不使用 initrd。

## 从空卡准备完整产物

安装仓库 RISC-V 构建依赖。固件还需要宿主 C 编译器、make、bison、flex、swig、
OpenSSL/GnuTLS 开发头文件、Python 开发头文件、pip/setuptools/PyYAML；整卡组装需要
`dosfstools`、`mtools`。Debian/Ubuntu 的补充包示例：

```sh
sudo apt install build-essential gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
  bison flex swig libssl-dev libgnutls28-dev python3-dev python3-pip \
  python3-setuptools python3-yaml dosfstools mtools
```

在仓库根目录执行：

```sh
tools/a20 check instances/licheerv-nano.toml
tools/a20 prepare licheerv-nano
tools/a20 package licheerv-nano
# E/WE 使用相同 SD/UART 早期固件：
tools/a20 package licheerv-nano-e
```

`prepare` 下载固定提交的源码归档并验证 SHA256，构建主线 U-Boot/OpenSBI，再调用官方
sophgo/fiptool，将厂商 FSBL、DDR 参数、OpenSBI、U-Boot 合成为 FAT 中的 `fip.bin`。
pyelftools 0.32 安装到私有构建目录，不修改主机 Python site-packages。默认缓存/构建约需
600 MiB，建议至少留 1 GiB 空间；归档缓存有效时不用再次下载。

| 组件 | 固定来源与构建契约 |
| --- | --- |
| U-Boot | v2025.10，`e50b1e8715011def8aff1588081a2649a2c6cd47`，`sipeed_licheerv_nano_defconfig` |
| OpenSBI | v1.7，`a32a91069119e7a5aa31e6bc51d5e00860be3d80`，`PLATFORM=generic FW_FDT_PATH=<本次 U-Boot DTB>` |
| fiptool / FSBL | sophgo/fiptool，`7f59889c91f7d5d440d6a09aad0209f0aca3d09d`，SG2002 使用 **cv181x.bin** |
| DDR 参数 | 同一 fiptool 提交的 `data/ddr_param.bin` |
| 二核 RTOS | 显式传入空文件，生成后解析 FIP 确认 `BLCP_2ND_SIZE=0`，不加载默认 cvirtos |
| DTB | `sg2002-licheerv-nano-b.dtb`，根 compatible 含 Nano/SG2002，memory reg 必须为 `0x80000000,0x10000000` |

FSBL/DDR 参数来自厂商二进制，不能把这条链称作全部源码可构建。归档 SHA256、编译器版本、
实际每个产物的 SHA256 和厂商许可文件都保存在 `build/licheerv-nano/firmware/`。
固定来源、参数和布局可重跑，主机工具版本/FAT 时间戳仍可能改变最终镜像哈希；不声称跨主机逐字节一致。

U-Boot 只增加 `CONFIG_CRC32_VERIFY=y` 与 `CONFIG_BOOTMETH_SCRIPT=y` 命令能力，另将默认
`scriptaddr` 从 **0x80c00000** 移到 **0x8e000000**，`kernel_addr_r` 改为 `0x80200000`。
旧脚本地址位于 A20OS Image/BSS 内部：当前完整 Image 覆盖 `0x80200000..0x81231000`，
加载内核会把尚在执行的 U-Boot 脚本冲掉。外置启动 DTB 放 `0x82000000`，与 Image 分离。
打包检查 Image footprint 未触及 DTB；固件构建检查 U-Boot ELF、malloc 和额外栈/GD/FDT
空间的保守预算不超过 DDR 顶部 16 MiB。实际 `bdinfo` 的重定位/堆栈仍需首板核对，不能仅凭
固定地址断言任意厂商固件均使用同一布局。

主要输出：

| 路径 | 内容 |
| --- | --- |
| `build/licheerv-nano/firmware/` | fip.bin、匹配 DTB、OpenSBI、U-Boot、vendor FSBL/DDR 参数、mkimage、manifest |
| `build/licheerv-nano/a20os-licheerv-nano-sdcard.img` | 默认 128 MiB 的完整 MBR/FAT32 整盘镜像 |
| 镜像旁 `.img.json` | 固件来源/布局与各文件、整镜像 SHA256 |
| `build/licheerv-nano/handoff/` | 同一 ELF 产生的 Image、匹配 DTB、manifest 和手动救援文本 |

两个默认实例共用板级产物目录。`handoff/kernel.bin` 是内核 Image，不能直接当整盘镜像写卡。
`handoff/uboot.cmd` 是命令文本；FAT 中 `boot.scr` 才是 mkimage 包装的二进制脚本。
`tools/licheerv_nano.py prepare/sdcard` 是等价私有入口，支持自定义缓存、输出、交叉工具前缀与并行数。

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

SG2002 BootROM 从 FAT 文件加载 **fip.bin**，不是从 PINE A64 的 8 KiB 固件偏移取 SPL。
构建逐文件从 FAT 读回并比对 SHA256。默认 U-Boot `run distro_bootcmd` 扫描 `boot.scr`；
脚本检查内核和 DTB 的加载状态、字节数和 CRC32，全部通过才 `booti`，失败退出、不 `saveenv`。
这些是旧文件/损坏检测，不是签名安全启动。内核没有 SD 驱动并不妨碍 U-Boot 读 FAT；接管后内核仍不能访问卡。

确认待写卡的**整盘设备**和所有分区已卸载，再执行：

```sh
tools/a20 deploy licheerv-nano --media-device /dev/实际SD整盘设备 --no-console
```

`/dev/sdX` 是实例占位符。写卡会覆盖该卡分区表及前 128 MiB 内容，首次使用专用空卡，
保留原来的厂商工作卡作为恢复路径。卡可以大于镜像，余下空间目前不用。
构建脚本只接受普通文件输出，不会打开真实块设备。

## 串口启动与恢复

接 3.3 V USB-UART，板 TX 接适配器 RX，板 RX 接适配器 TX，共地，115200 8N1；
根据官方引脚图使用 UART0，USB-C 供电口不是本移植的串口。
在上电前打开同一条可交互且有断言的连接：

```sh
tools/a20 console licheerv-nano --interactive --serial /dev/serial/by-id/实际串口设备
```

将写好的卡插入板再上电，观察 FSBL → OpenSBI → U-Boot → `A20OS LicheeRV Nano SD boot`
→ 内核。正常自动启动无需键入 handoff。若失败，打断 U-Boot autoboot，先检查
`mmc list`、`part list mmc 0`、`bdinfo`、`printenv scriptaddr fdt_addr_r`，再逐行恢复：

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

显式命令本身不带自动 CRC 检查，任一步失败立即停止。`mmc 0:1` 按实际卡布局核对。
`booti` 第三参数始终是独立可写的启动 DTB，不能直接用控制 DTB 的 malloc 区，
也不能用内核地址；`go` 的 argc/argv ABI 不满足内核要求。

## E/WE 厂商固件 TFTP 可选路径

默认主线 Nano B 设备树没有启用 E/WE 网口，不能因为编译了 MAC 驱动就假设 TFTP 可用。
只有先用 `help tftpboot`、`printenv ethact`、`ping` 确认原厂 E/WE 固件可用网口后，才使用：

```sh
tools/a20 package licheerv-nano-e-tftp
```

这是 kernel-bundle，不重建空卡固件。下面以主机 `192.168.50.10`、板子 `192.168.50.20`
为例；替换为同一实际 LAN 的地址，主机地址必须已配置到连接板子的网卡，板子地址应未被占用。
不要随后再打包另一实例：同一 handoff 目录会更新为另一个实例的 manifest。

终端 1 启动只读服务：

```sh
sudo python3 tools/a20_tftp.py --bundle "$PWD/build/licheerv-nano/handoff" \
  --bind 192.168.50.10 --port 69
```

服务在打开 UDP 端口前验证 manifest 中 `kernel.bin` 的大小和 SHA256，只提供这一个文件的
不可变快照；拒绝上传、其他路径和非 octet 模式。UDP 69 通常需要绑定权限，因此示例用 sudo。
确认该地址上的 UDP 69 未被其他 TFTP 服务占用，主机防火墙允许板子访问 TFTP 及其响应端口。
每次重新 `package` 后停止旧服务再启动，避免继续提供旧快照。Ctrl-C 退出服务。

终端 2 在板子上电之前运行自动串口交接：

```sh
tools/a20 console licheerv-nano-e-tftp --auto-handoff \
  --serial /dev/serial/by-id/实际串口设备 \
  --server-ip 192.168.50.10 --board-ip 192.168.50.20
```

该命令在同一串口连接打断 U-Boot，设置易失的 `serverip`/`ipaddr`，验证每条命令状态、
实际下载字节数/CRC32 和 FDT 头，最后检查内核日志。服务本身不打开串口、不会写板载 Flash。
板子仍需已具备可用的厂商 E/WE 网络固件；看到明确 U-Boot 提示符之后才会发送装载指令。

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

克隆空间上限为 128 KiB，`fdt move` 失败就停止；先确认 `0x82000000..0x82020000` 未被
厂商固件/小核 RTOS 保留。控制 DTB 必须包含真实 RAM 和保留区，不能把被裁剪的控制树当完整启动树。
也可 TFTP 加载单独核对过的匹配 DTB。带二核 RTOS/多媒体固件的厂商卡，必须提供它们实际占用
DDR 的 reserved-memory；不能沿用空 RTOS 的内存假设。TFTP 成功只证明 U-Boot 网卡能力，
A20OS 仍没有 SG2002 Ethernet 驱动。

## 验收与局限

```sh
tools/a20 console licheerv-nano --interactive
```

在内核启动**之前**开始监听，确保同时捕获：

- `[FDT] RAM range`：有效 DTB 到达内核并收窄内存范围。
- `System ready (bringup, no userspace)`：完成内核初始化。
- `part ok`：最小分配/释放成功。
- `System is going down for power-off NOW`：进入 SBI 关机路径；还需实际确认板子已停止运行。

当前实例 `bringup=true`、`RAMFS_USER=0`，会自动关机，**没有 shell**；旧版本的 `cat /etc/os-release`
与 `poweroff` 串口命令并不适用于这个配置。`System ready` 本身也不证明进入用户态。

目前不提供 SD、网卡、DMA、NPU、视频或第二核驱动。C906 的 DMA 缓存一致性不能视为 QEMU 的一致性：
后续 SDHCI/网卡实现需要单独完成 T-Head 缓存维护和设备地址约束，再做实际数据校验。PLIC 的边沿中断
扩展也未覆盖。固件是否提供 SBI TIME/SRST、其 T-Head MMU/cache 设置仍需实机记录。

内存解析现在检查 DTB v17 的 RAM 描述、`memreserve` 与 `/reserved-memory/reg`，也保留 DTB 缓冲区本身。
当前板级分配接口提供单一连续区间，因此从内核之后的第一个保留区起保守截去高地址 RAM；可用容量
可能小于完整 256 MiB。保留区与内核重叠、坏 DTB、动态 `size` 保留或非空 `ranges` 会停止启动，
避免继续分配固件内存。缺失 DTB 仍有板级窗口回退，但不计入本实例的成功验收。
厂商多媒体/小核固件可能占用 DDR，必须给出与实际固件匹配的保留区；不能把任意 Linux DTB 改成
256 MiB 后直接使用。没有写进 DTB 的固件占用无法由内核推断。

实机运行状态尚未验证；SBI TIME/SRST、PLIC 路由、RAM 保留区和断电行为以实际固件及板卡为准。

## 参考资料

- [Sipeed 板卡规格与 B/E/W/WE 区别](https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/1_intro.html)
- [Sipeed 官方 SDK](https://github.com/sipeed/LicheeRV-Nano-Build)
- [Linux SG2002 DTS](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/sophgo/sg2002.dtsi)
  与 [CV180x 外设 DTS](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/sophgo/cv180x.dtsi)
- [U-Boot 官方 Nano FSBL/OpenSBI/FAT FIP 启动指南](https://docs.u-boot.org/en/v2025.10/board/sophgo/licheerv_nano.html)
- [Sophgo 官方 fiptool、cv181x FSBL、DDR 参数](https://github.com/sophgo/fiptool/tree/7f59889c91f7d5d440d6a09aad0209f0aca3d09d)
- [U-Boot 正确板名与默认 Nano B DTB](https://github.com/u-boot/u-boot/blob/master/configs/sipeed_licheerv_nano_defconfig)
- [U-Boot booti 头校验](https://github.com/u-boot/u-boot/blob/master/arch/riscv/lib/image.c)
  与 [a0/a1 交接实现](https://github.com/u-boot/u-boot/blob/master/arch/riscv/lib/bootm.c)
- [RISC-V 64 字节 Image 头协议](https://docs.kernel.org/arch/riscv/boot-image-header.html)

以上来源证明硬件事实及固件协议，不构成 A20OS 已完成实机验收的证据。

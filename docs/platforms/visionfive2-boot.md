# StarFive VisionFive 2 / JH7110

本页适用于 VisionFive 2 的 v1.2a/v1.3b 板型和 2/4/8 GiB RAM 配置。 A20OS 提供内核板级实现、OpenSBI/U-Boot SPL 构建脚本、直接引导 FIT 和 SD 镜像目标。 NOMMU 上板启动已有真机验证基线；默认 MMU 用户态组合、SDIO 中断、GMAC 数据面和 SMP 仍须分别验收，不能从一次基础启动推定全部设备可用。

## 系统和设备范围

| 项目 | 配置及限制 |
| --- | --- |
| 执行核 | SiFive U74，RV64IMAFDC；默认单核，SBI HSM/IPI 支持在源码中提供 |
| RAM / 内核 | RAM 从 `0x40000000` 起；内核物理入口 `0x40200000`，Sv39 或 NOMMU 链接脚本 |
| DTB | 随固件构建的 v1.2a/v1.3b DTB；FIT 装载到 `0x46000000`，避开内核 BSS |
| UART0 | `0x10000000`；保留固件配置，接收使用轮询 |
| PLIC / timer | 标准 PLIC；定时器使用 SBI，DTB 时基优先，JH7110 回退 24 MHz |
| SD | `dw_sdio.c` 对接 `0x16020000`；单线 PIO、SDSC/SDHC/SDXC、最高 25 MHz；IRQ 可选，时钟由实际 PLL2/AXI 分频寄存器计算；新驱动读写和中断路径尚未真机验收 |
| 有线网络 | `starfive_gmac.c` 对接 GMAC1 `0x16040000`、Motorcomm PHY；数据面为轮询，仅支持 embedded 驱动部署，需实机网络验收 |
| 其他设备 | 不由此页承诺 HDMI、GPU、USB、无线、PCIe 或音频可用；软件 world 的存在也不证明显示设备可用 |

MMU 默认镜像带 FAT32 用户态，挂载到 `/bin`；可选 ext4 软件分区挂载到 `/extra`。 NOMMU 配置用于兼容/启动对照，不具备与 MMU 配置相同的进程隔离和程序兼容性。实际可分配内存以启动日志为准：当前单区间分配器在内核之后的首个保留区处裁剪，其中包括位于 `0x46000000` 的 DTB，因此不能把硬件 2/4/8 GiB 容量当成当前可分配容量。

## 依赖和启动链

下面以 Debian/Ubuntu 为例，Python 需为 3.11 或更新版本：

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential git curl ca-certificates xz-utils file python3 \
  dosfstools e2fsprogs mtools util-linux gdisk picocom minicom \
  gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
  gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \
  bison flex bc pkg-config libssl-dev libgnutls28-dev uuid-dev \
  python3-dev python3-setuptools swig device-tree-compiler
```

在已取得的 A20OS 源码根目录更新子模块；以下步骤假定当前目录包含仓库 Makefile：

```sh
git submodule update --init --recursive
```

尚无 rustup 时安装，再为本仓库固定工具链安装目标：

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 \
  riscv64imac-unknown-none-elf riscv64gc-unknown-none-elf
```

已有 rustup 可跳过下载安装器，但仍执行 `toolchain install` 和 `target add`。构建使用仓库 `rust-toolchain.toml`，不要求另外安装 nightly。

固件脚本默认使用 `riscv64-linux-gnu-`，内核使用仓库选择的 RV64 工具链。首次固件/子模块/world 获取需要网络；更完整的宿主选项见[构建指南](../dev/build.md)。

```text
BootROM → U-Boot SPL → FIT：OpenSBI fw_dynamic → A20OS
                         └ 匹配 v1.2a/v1.3b DTB
SD FAT32 → /bin；可选 ext4 → /extra
```

此路径由 SPL 直接进入 OpenSBI/A20OS，正常启动时**没有 U-Boot proper 提示符**。 FIT 将内核标为 `os = "U-Boot"`，用于满足 SPL 查找下一入口的协议；这不改变内核类型。 OpenSBI 位于 `0x40000000`，内核位于 `0x40200000`，DTB 位于 `0x46000000`。 EEPROM 板型选择对应 FIT 配置；EEPROM 无法读取时的默认配置为 v1.3b，应核对实际板型。

## 从源码生成镜像

仓库提供三个 VisionFive 2 实例：

| 实例 | 当前用途 / 注意事项 |
| --- | --- |
| `vf2-minimal` | `fit-sdcard` / `minimal`，调用 `make vf2-minimal`，不声明物理串口或目的盘 |
| `vf2-sdcard` | `fit-sdcard` / `sdcard`，调用 `make vf2-sdcard`，不声明物理串口或目的盘 |
| `vf2-physical` | 声明串口和写卡目标，但配置存在下述问题，不作为本页推荐部署入口 |

两个打包实例可在固件准备后直接使用：

```sh
tools/a20 check instances/vf2-minimal.toml instances/vf2-sdcard.toml
tools/a20 package vf2-minimal
tools/a20 package vf2-sdcard -- VF2_WORLD=devel
```

它们复用下方 Make 镜像目标与同名产物，不额外构建启动固件。 VF2 没有 `a20 prepare` 分派；先执行 `make vf2-firmware`。实例名是 `vf2-minimal`/`vf2-sdcard`，不能用板名 `visionfive2` 代替。本页默认采用显式 Make 构建、独立写卡和串口记录流程：

```sh
make vf2-firmware
VF2_GIT_TRANSPORT=https make vf2-extra-sources
make vf2-minimal
```

`vf2-firmware` 按脚本锁定的上游提交构建 OpenSBI、SPL、救援 U-Boot 和两份 DTB，工作树位于 `build/vf2-work/`；产物位于 `build/vf2-firmware/`。`vf2-minimal` 默认构建 MMU 内核、FAT32 用户态与 fastfetch，不创建 `/extra`。没有 SSH key 时用上面的 HTTPS 子模块获取方式。

其他组合：

```sh
# 默认 MMU 内核 + FAT32 + Alpine devel world 的 ext4 /extra
make vf2-sdcard VF2_WORLD=devel

# 同一镜像布局，换为 NOMMU 内核/用户态
make vf2-minimal VF2_MMU=0
make vf2-sdcard VF2_MMU=0 VF2_WORLD=devel

# 使用已有的无分区表 ext4 文件作为 /extra，不下载 world
make vf2-sdcard VF2_EXTRA_IMAGE=/absolute/path/extra.ext4.img

# 独立的历史 NOMMU 组合目标：固定使用 NOMMU，不读取 VF2_MMU
make vf2-image VF2_WORLD=devel
```

`vf2-minimal`、`vf2-sdcard` 读取 `VF2_MMU`，默认 `1`；`vf2-image` 固定为 NOMMU。不要把它们当成三个完全相同的目标。所有组合都会覆盖同名 `a20os.itb` 和 `a20os-sd.img`。`VF2_WORLD=xfce` 只选择软件包，不启用此板显示驱动。

| 产物（`build/vf2-firmware/`） | 用途 |
| --- | --- |
| `u-boot-spl.bin.normal.out` | 含 StarFive 头的 SPL |
| `fw_dynamic.bin`、两份 `jh7110-starfive-visionfive-2-*.dtb` | FIT 输入 |
| `a20os.itb` | OpenSBI + A20OS + DTB 的直接引导 FIT |
| `a20os-sd.img` | GPT 整卡镜像：SPL 在 2 MiB、FIT 在 4 MiB，FAT32 从 32 MiB 起；可选 ext4 随后布局 |
| `u-boot.itb` | OpenSBI + U-Boot proper，用于救援控制台 |
| `mkimage`、`dtc` | 宿主检查和打包工具 |

构建后先检查普通文件：

```sh
build/vf2-firmware/mkimage -l build/vf2-firmware/a20os.itb
sgdisk -v build/vf2-firmware/a20os-sd.img
sha256sum build/vf2-firmware/a20os-sd.img
```

FIT 内核 load/entry 应为 `0x40200000`，DTB load 应为 `0x46000000`。当前 FIT 配方没有签名节点或已配置的 SPL 验签链，不提供启动真实性保证。

## 写卡、拨码与串口

`vf2-physical` 不应直接作为无人值守部署/验收入口：它把`target.media_device` 固定为 `/dev/sda`，可能指向宿主系统盘；`kernel.bringup=true` 的单独 build 不提供 shell，却配置了 `cat`/`ps`/`poweroff`命令及 `SMC: System ready` 期望。它的 package 又转入独立的 VF2 开发镜像编排，不能把单独 build 的配置和 package 产物混为同一候选。实例语法检查不会消除这些语义和目的盘问题。本页改用明确选择整卡的写入命令，并用终端记录实际镜像的启动，不照用该实例的默认 deploy 或 shell 检查。

准备独立电源、microSD/读卡器和 3.3 V 逻辑 USB-TTL。先确认目的盘、卸载其所有已挂载分区，再写入整卡：

```sh
lsblk -o NAME,MODEL,SERIAL,SIZE,TYPE,MOUNTPOINTS
sudo dd if=build/vf2-firmware/a20os-sd.img \
  of=/dev/disk/by-id/REPLACE_WITH_VF2_SD bs=4M conv=fsync status=progress
sync
sudo sgdisk -e /dev/disk/by-id/REPLACE_WITH_VF2_SD
sudo sgdisk -v /dev/disk/by-id/REPLACE_WITH_VF2_SD
```

占位符必须替换为实际 TF 整卡，写入会覆盖其数据。`sgdisk -e` 将备份 GPT 移到更大物理卡的末尾；它不扩展文件系统。

断电后按 **RGPIO_1、RGPIO_0** 的顺序设置拨码：

| 启动方式 | RGPIO_1 | RGPIO_0 |
| --- | --- | --- |
| QSPI NOR | 0 / L | 0 / L |
| SDIO3.0 | 0 / L | 1 / H |
| eMMC | 1 / H | 0 / L |
| UART ROM 恢复 | 1 / H | 1 / H |

这是[StarFive 官方启动表](https://doc-en.rvspace.org/VisionFive2/Quick_Start_Guide/VisionFive2_SDK_QSG/boot_mode_settings.html)的逻辑值；不同板版的开关丝印可能不同，不要用“两个都向右”代替逻辑值。本镜像选择 SDIO3.0；官方提示该模式偶尔可能需要重新上电。

[官方 40-pin 表](https://doc-en.rvspace.org/VisionFive2/40-Pin_GPIO_Header_UG/VisionFive2_40pin_UG/gpio_pinout%20-%20vf2.html)给出 pin 6 为 GND、pin 8 为 GPIO5/UART TX、pin 10 为 GPIO6/UART RX。板 TX 接适配器 RX，板 RX 接适配器 TX；115200、8N1、无流控。板子独立供电，不将 5 V UART 或 RS-232 电平接入。

先开终端并开始记录，再插卡上电，例如：

```sh
picocom --baud 115200 --flow n --logfile vf2-serial.log /dev/ttyUSB0
```

预期顺序是 SPL → OpenSBI → A20OS 内存/定时器初始化 → init/shell。`BRINGUP=1` 且未配置 RAMFS_USER 的内核只会运行自检后结束，不会出现 shell；本页 `vf2-minimal`/`vf2-sdcard` 使用的是带用户态的开发配置。

进入 shell 后先检查 `cat /etc/os-release`、`mount`、`ls /bin`；有 `/extra` 时再核对其实际内容。网络需接 RJ45 后检查 PHY/link、`cat /proc/net/config` 和同网段 ping。默认 DHCP 也依赖真实可用的数据面。 SMP 要单独设置 `NR_CPUS=2/4 ALLOW_UNVERIFIED_SMP=1` 并检查 secondary、IPI 和调度；默认单核启动不证明多核已验收。

## 救援控制台与可选 QSPI 更新

默认用 SD 试启动，无需擦写板载 Flash。需要 U-Boot 控制台时，先从已生成的卡镜像制作救援副本：

```sh
test "$(stat -c %s build/vf2-firmware/u-boot.itb)" -le $((12 * 1024 * 1024))
cp build/vf2-firmware/a20os-sd.img build/vf2-firmware/u-boot-rescue-sd.img
dd if=build/vf2-firmware/u-boot.itb of=build/vf2-firmware/u-boot-rescue-sd.img \
  bs=512 seek=8192 conv=notrunc
```

按前面的写卡步骤改用 `u-boot-rescue-sd.img`，SD 模式上电并截停 U-Boot。这张卡保留 SPL/GPT，只将第二分区换成 U-Boot proper FIT。

只有在已保存原固件、救援卡可启动并确认 `sf probe` 的容量后，才考虑更新 QSPI。将产物放到同网段 TFTP 服务器；以下操作会实际覆盖 Flash：

```text
sf probe
setenv serverip <主机IPv4>
setenv ipaddr <板子IPv4>
tftpboot ${kernel_addr_r} u-boot-spl.bin.normal.out
sf update ${kernel_addr_r} 0x0 ${filesize}
tftpboot ${kernel_addr_r} a20os.itb
sf update ${kernel_addr_r} 0x100000 ${filesize}
```

每次 `tftpboot` 成功后核对文件长度/校验值和目标范围，失败不继续； FIT 的末尾 `0x100000 + filesize` 必须落在实际 Flash 容量内。更新后断电，选择 QSPI 的 **L,L**。内核位于 QSPI 并不消除 `/bin` 和 `/extra`的 SD 依赖，默认开发配置不能据此声称完整脱卡运行。

Flash 启动失败时切回 SD 救援卡，不继续反复擦写。 SD 也不能启动时，UART 恢复为 **H,H**，使用[官方恢复说明](https://doc-en.rvspace.org/VisionFive2/Quick_Start_Guide/VisionFive2_SDK_QSG/recovering_bootloader%20-%20vf2.html)及匹配的恢复固件。仓库没有 VF2 ROM UART 自动恢复脚本。

## 故障定位与轻量构建

- 完全没有 SPL：检查电源、SD 镜像、拨码逻辑和 UART 接线。
- 有 SPL、没有 OpenSBI：检查 FIT 分区与 SPL 对板型/EEPROM的识别。
- 有 OpenSBI、无 A20OS：检查 FIT load/entry、链接脚本、DTB 与 BSS 是否重叠。
- 内存容量或保留区异常：停止启动；匹配 DTB，不能把 8 GiB 窗口直接当成所有板的 RAM。
- init 后无 shell：先用不带 `/extra` 的最小卡；检查 FAT32、用户态 ABI 和启动日志。
- SDIO 中断超时：用内核参数 `a20.dw-sdio.poll=1` 对照轮询路径。
- 网口有 PHY 无流量：需要单独核对 DMA 描述符、缓存维护和 carrier，不能由 PHY 探测推定收发成功。

只编译内核、不下载软件包：

```sh
make ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=1 \
  DRIVER_DEPLOYMENT=embedded kernel-only
make ARCH=riscv64 BOARD=visionfive2 ABI=linux BRINGUP=1 NOMMU=1 \
  DRIVER_DEPLOYMENT=embedded LDSCRIPT=kernel/arch/riscv64/boards/visionfive2/ldscript-nommu.ld kernel-only
```

不要手写可能变化的 `.kernel-build` 子目录；用相同选项执行 `make -s ... print-build-dir`查出目录，再用其中的 ELF 做 `addr2line`。平台构建检查为 `make check-visionfive2-build`。相关接口见[板级接口参考](physical-boards.md)、[平台约定](README.md)。

SD 时钟 glue 位于 `kernel/arch/riscv64/boards/visionfive2/storage.c`，只配置 SDIO1 局部分频和门控，不重编程共享 PLL 或总线。保留由 SD 启动固件设置的 pinmux、供电和采样相位；不宣称从未配置的裸 SoC 或其他启动介质恢复 SD 接口。卡协议和 PIO 收敛到通用 DW-MSHC 驱动；当前不支持 eMMC、SDIO function、UHS 电压切换或热插卡。 CRC/数据超时会使当前卡停止传输并返回错误，显式重新绑定后重新枚举。依据为 [Linux v6.12 StarFive MMC](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/dw_mmc-starfive.c)、[系统时钟](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/starfive/clk-starfive-jh7110-sys.c) 和[PLL 寄存器计算](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/starfive/clk-starfive-jh7110-pll.c)。

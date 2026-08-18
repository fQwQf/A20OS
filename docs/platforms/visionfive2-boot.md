# VisionFive 2 上板启动:从源码构建启动链

本文描述 A20OS 在 StarFive VisionFive 2(JH7110)上的完整上板流程。启动链
完全从上游源码构建,不依赖任何 Debian 镜像或厂商预编译固件;内核经
OpenSBI 直接引导,可烧入板载 QSPI Flash 脱卡启动。

板级硬件事实与驱动边界见 [physical-boards.md](physical-boards.md)。

## 启动链架构

```text
JH7110 BootROM
  └─ U-Boot SPL            (flash 0x0 / SD GPT "spl" 分区 @2 MiB)
       └─ a20os.itb (FIT,flash 0x100000 / SD GPT "uboot" 分区 @4 MiB)
            ├─ OpenSBI fw_dynamic   load=0x40000000  (M-mode 常驻,SBI 服务)
            ├─ A20OS kernel.bin     load=0x40200000  (S-mode 入口 _start)
            └─ DTB(v1.2a / v1.3b,SPL 按板载 EEPROM 产品 ID 自动选择)
  ├─ FAT32 userspace       (SD GPT "a20os-rootfs" 分区 @32 MiB,挂载到 /bin)
  └─ extra ext4 packages   (SD GPT "a20os-extra",挂载到 /test)
```

关键点:

- A20OS 的 `entry.S` 入口约定(a0=hart ID,a1=DTB,S-mode)正是 OpenSBI
  的下一级引导协议,因此 **U-Boot proper 不参与启动**:SPL 加载 FIT 后
  由 `spl_invoke_opensbi()` 填入 `fw_dynamic_info`(next_addr=内核入口,
  next_mode=S),OpenSBI 直接落入内核。
- FIT 中 A20OS 镜像的 `os = "U-Boot"` 是有意为之:SPL 在未启用
  `SPL_LOAD_FIT_OPENSBI_OS_BOOT` 时按该 OS 名查找下一级入口(见
  `tools/vf2/a20os-fit.its` 注释)。
- 内核链接/加载地址为 PA `0x40200000`(`kernel/platform/visionfive2/
  ldscript.ld`),启动页表 RAM 窗口由链接脚本符号 `BOOT_MAP_PHYS=
  0x40000000` 推导(`kernel/arch/riscv64/boot/entry.S`),与 QEMU virt
  的 `0x80000000` 布局共用同一份 arch 代码。
- 定时器频率运行时取自 DTB `timebase-frequency`(JH7110 = 24 MHz),
  QEMU virt 路径不受影响(10 MHz)。

## 一、构建(全部从源码)

```sh
# 1. 固件:克隆并构建 OpenSBI + U-Boot(版本按 commit 锁定,
#    可用 OPENSBI_REF/UBOOT_REF 覆盖;工作树在 /tmp/vf2-firmware,增量构建)
make vf2-firmware

# 2. 内核 + 额外用户态 + 打包:生成 FIT、FAT32 根文件系统和 extra ext4 分区
make vf2-image

# 只准备 extra 所需源码（GitHub 默认走 SSH；没有 SSH key 时加
# VF2_GIT_TRANSPORT=https）。GCC 源码从 musl-cross-make 的校验下载规则取得。
make vf2-extra-sources

# extra/gcc 默认复用 musl-cross-make/output 中已经安装的工具链；也可以
# 显式指定另一套兼容的 RISC-V musl 工具链根目录：
MUSL_CROSS_ROOT=/path/to/riscv64-linux-musl-cross make vf2-image
```

产物(`build/vf2-firmware/`,不入库):

| 文件 | 用途 |
|---|---|
| `u-boot-spl.bin.normal.out` | SPL(带 StarFive 头+CRC),flash 0x0 / SD spl 分区 |
| `a20os.itb` | OpenSBI+A20OS+DTB 的 FIT,flash 0x100000 / SD uboot 分区 |
| `a20os-sd.img` | 完整 SD 卡裸镜像(GPT 四分区,含 FAT32 userspace 和 extra ext4),可直接 dd 到 TF 卡 |
| `u-boot.itb` | OpenSBI+U-Boot proper 的 FIT,救援/开发用 U-Boot 控制台 |
| `fw_dynamic.bin`、`*.dtb`、`mkimage`、`dtc` | 中间产物与打包工具 |

## 二、SD 卡启动(首次 bring-up 推荐)

1. 写卡(假设 TF 卡为 `/dev/sdX`,**先 lsblk 确认**):

   ```sh
   sudo dd if=build/vf2-firmware/a20os-sd.img of=/dev/sdX bs=4M conv=fsync
   ```

2. 拨码开关(RGPIO_1/RGPIO_0):**SD 启动 = L,H**(即 Flash 档位两个开关
   都在右侧 ON 时,把 RGPIO_1 拨到左侧;以板子丝印和官方表格为准)。

3. 串口:USB 转 TTL 接 40pin 排针 GND=pin6、TXD=pin8(GPIO14)、
   RXD=pin10(GPIO15),115200 8N1。

4. 上电,预期串口依次出现 SPL 日志 → OpenSBI banner → A20OS 内核日志
   (`[FDT] RAM range ...`、`[TIMER] backend=... freq=24000000 Hz`)并进入
   mksh 的 `#` 提示符。

## 三、烧入 QSPI Flash(脱卡启动)

Flash 布局(JH7110 约定,`u-boot,spl-payload-offset = <0x100000>`):

| 偏移 | 内容 |
|---|---|
| `0x0` | `u-boot-spl.bin.normal.out` |
| `0x100000` | `a20os.itb` |

板载 Flash 擦写需要一个运行中的 U-Boot 控制台(上游 U-Boot 自带 eqos
网卡驱动,推荐 TFTP 传输):

1. 先用 SD 卡把板子引导到 **U-Boot 控制台**:把 `u-boot-spl.bin.normal.out`
   和 `u-boot.itb` 按与 `a20os-sd.img` 相同的偏移写入一张 TF 卡
   (spl 分区 seek=4096,uboot 分区 seek=8192),SD 档位启动,串口按任意键
   打断 autoboot。

2. 在 U-Boot 中烧写(宿主机起 TFTP 服务,board 与宿主机同网段):

   ```text
   => sf probe
   => dhcp
   => tftpboot $kernel_addr_r u-boot-spl.bin.normal.out
   => sf update $kernel_addr_r 0x0 $filesize
   => tftpboot $kernel_addr_r a20os.itb
   => sf update $kernel_addr_r 0x100000 $filesize
   ```

   没有网络环境时也可用 U 盘(`usb start; fatload usb 0:1 ...`)替代 TFTP。

3. 拨码改为 **Flash 启动 = H,H(两个开关均在 ON/右侧)**,重新上电,
   启动链变为 BootROM → flash SPL → flash FIT(OpenSBI → A20OS)。

4. 后续迭代内核只需重烧 FIT:`make vf2-image` 后经 U-Boot
   `sf update $kernel_addr_r 0x100000 ...` 更新即可,SPL 不必动。

**救砖**:Flash 内容损坏时拨回 SD 档位从 TF 卡引导,重新执行第 2 步。
若 SD 也不可用,JH7110 BootROM 支持 UART XMODEM 恢复(拨码 H,L),
可用 StarFive 的 `jh7110-recover` 工具(Rust,源码开源)重建 Flash。

## 四、验证清单(逐级推进,卡住即停)

1. 串口有 SPL/OpenSBI 日志 → BootROM、SPL、FIT 加载正常。
2. 出现 A20OS 早期打印 → 加载地址、启动页表、satp 切换正确。
3. `[FDT] RAM range 0x40000000..` → 内存窗口与 DTB 解析正确。
4. `[TIMER] ... freq=24000000 Hz` → 运行时定时器频率生效。
5. 中断/timer tick 正常,进入 init。
6. `[StarFive-GMAC] PHY link up`,ping 通(驱动边界见 physical-boards.md)。
7. SMP:`NR_CPUS=2/4 ALLOW_UNVERIFIED_SMP=1` 重构,确认 secondary
   online、reschedule/TLB IPI,跑 `smp_bench`、`sched_stress`。

## 五、调试

- **串口完全无输出**:先确认 SPL 有日志(没有则是卡/拨码/串口接线问题);
  SPL 有日志而内核无输出,基本是 FIT 加载地址与内核链接地址不一致,
  用 `build/vf2-firmware/mkimage -l a20os.itb` 核对(load/entry 应为
  0x40200000)。
- **panic**:记录 `mepc`/`stval`,宿主机
  `riscv64-unknown-elf-addr2line -e .kernel-build/riscv64-visionfive2-linux-bringup/kernel.elf <addr>`。
- **QEMU 对照**:同一 arch 代码在 `make ARCH=riscv64 run` 下验证过,
  真机与 QEMU 行为分叉时优先怀疑板级地址/IRQ(board.c)与 DTB 解析。

## 六、修改 A20 代码后的标准迭代流程

上板调试不应该直接在一条命令里“盲烧”。每次修改后按下面的层级推进，
可以把问题限制在源码、用户态、打包或硬件中的某一层。

### 6.1 开始前保存工作树并同步远程

```sh
git status --short
git diff -- kernel user tools Makefile
git fetch origin
```

如果工作树干净且本地没有独立提交，可以快进到远程主分支；有本地提交时
先查看 `git log --oneline --decorate -8`，再选择 `git merge origin/main`
或 `git rebase origin/main`。不要在未查看 `git status` 的情况下运行会覆盖
工作树的命令。同步后重新检查 submodule：

```sh
git submodule sync --recursive
git submodule status --recursive
```

extra 软件的 gitlink 没有物化时，使用仓库脚本（默认 GitHub SSH）：

```sh
make vf2-extra-sources
# 没有 SSH key 时：
VF2_GIT_TRANSPORT=https make vf2-extra-sources
```

### 6.2 先做便宜的编译检查

只改内核或板级代码时，先不要生成大镜像：

```sh
NPROC=$(getconf _NPROCESSORS_ONLN)
make -j"$NPROC" ARCH=riscv64 BOARD=visionfive2 \
  ABI=linux BRINGUP=1 NOMMU=1 KERNEL_WERROR=0 kernel-only
```

`BRINGUP=1` 只检查内核，不生成 FAT32/extra 镜像；若改动涉及用户程序，
再做完整用户态构建：

```sh
NPROC=$(getconf _NPROCESSORS_ONLN)
make -j"$NPROC" -C user ARCH=riscv64 NOMMU=1 \
  BUILD_DIR=build/riscv64-nommu
```

### 6.3 生成板上产物

第一次构建或固件更新时先执行 `make vf2-firmware`；只改 A20OS 内核时可以
复用已有的 `build/vf2-firmware/`。完整 SD 镜像使用：

```sh
NPROC=$(getconf _NPROCESSORS_ONLN)
make -j"$NPROC" vf2-image \
  EXTRA_PACKAGES='vim git gcc' \
  EXTRA_IMAGE_MB=2048
```

`vf2-image` 会重新生成 NOMMU 内核、FAT32 根文件系统、extra ext4 分区和
FIT。`EXTRA_IMAGE_MB` 必须大于实际 staging 内容；GCC/Git/Vim 的完整组合建议
至少 2048 MiB，只有少量工具时才在命令行覆盖成更小的值。主机磁盘
需要同时容纳 staging 目录和镜像，空间不足时先删除可重建的
`build/vf2-firmware/a20os-sd.img`、`.kernel-build/*/extra.img`，不要删除
`user/external` 源码。

构建结束后检查产物和 FIT 地址：

```sh
ls -lh build/vf2-firmware/a20os-sd.img build/vf2-firmware/a20os.itb
build/vf2-firmware/mkimage -l build/vf2-firmware/a20os.itb
```

FIT 中 A20OS 的 `load`/`entry` 应为 `0x40200000`。如果只修改内核，
也可以只重建 `kernel.bin`，再运行 `tools/vf2/make-boot-image.sh`；但最终
上板前应优先使用完整 `vf2-image`，避免 FAT32 和 extra 内容滞后。

### 6.4 安全写入 TF 卡

写卡前必须确认设备名。`/dev/sda` 只是本次机器的例子，不能照抄到另一台
电脑：

```sh
lsblk -o NAME,MODEL,SERIAL,SIZE,TYPE,MOUNTPOINTS
findmnt -S /dev/sda1; findmnt -S /dev/sda2
sudo umount /dev/sda1 /dev/sda2 /dev/sda3 /dev/sda4 2>/dev/null || true
sync
```

确认目标确实是 TF 卡整盘后才写入，目标必须是磁盘 `/dev/sda`，不能是分区
`/dev/sda1`：

```sh
sudo dd if=build/vf2-firmware/a20os-sd.img of=/dev/sda \
  bs=4M conv=fsync status=progress
sync
sudo sgdisk -e /dev/sda
sudo partprobe /dev/sda
sudo sgdisk -v /dev/sda
lsblk -o NAME,SIZE,FSTYPE,PARTLABEL,MOUNTPOINTS /dev/sda
```

`sgdisk -v` 应报告 `No problems found`。物理卡比镜像大时，GPT 修复后会
显示尾部空闲扇区，这是正常的；不要为了“填满卡”而把 extra 分区随意扩大，
除非同时修改镜像构建参数并重新生成 GPT。

### 6.5 串口启动与证据记录

USB-TTL 使用 115200 8N1，连接 GND、板 TX 到转接器 RX、板 RX 到转接器 TX：

```sh
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
screen /dev/ttyUSB0 115200
```

上电前就打开串口，保存完整日志。`screen` 中按 `Ctrl-a H` 开始/停止硬件
日志；退出按 `Ctrl-a k`。日志应按以下顺序出现：

```text
U-Boot SPL ...
Trying to boot from MMC2
OpenSBI ...
[FDT] ...
[INIT] ...
mksh ...
#
```

看到 OpenSBI 但没有 `[FDT]`，优先检查 FIT 的 load/entry、内核链接脚本和
DTB；看到 `[INIT]` 但没有 `#`，优先检查进程调度、用户栈、标准输入输出
绑定和 `mksh` 的 fork/exec；看到 `#` 后再检查 `mount`、`/test/bin` 和
具体软件。每次只改变一个变量，并把源码提交、镜像 SHA256、拨码位置和
串口日志放在同一份实验记录中：

```sh
sha256sum build/vf2-firmware/a20os-sd.img
git rev-parse HEAD
```

### 6.6 失败后的最小化回归

当完整 extra 组合启动失败时，用最小镜像区分内核问题和用户态问题：

```sh
make -j"$NPROC" vf2-image EXTRA_PACKAGES='' EXTRA_IMAGE_MB=256
```

最小镜像能进入 `#`，再逐项增加 `git`、`vim`、`gcc`。Rust 是可选的大型
工具链，确认磁盘空间和 glibc 运行库后再显式加入：

```sh
make -j"$NPROC" vf2-image \
  EXTRA_PACKAGES='vim git gcc rust' EXTRA_IMAGE_MB=2048
```
每次增加后在 shell 中运行对应命令，并确认：

```sh
mount
ls -l /test/bin
/test/bin/git --version
/test/bin/vim --version
/test/bin/gcc --version
/bin/fastfetch
```

### 6.8 VisionFive 2 有线网络配置

GMAC1 使用板上 RJ45 接口。最简单的拓扑是“板子 RJ45 -> 交换机/家用路由器”，
路由器开启 DHCP；A20OS 在没有网络启动参数时默认发送 DHCP 请求。串口看到
`[StarFive-GMAC] PHY link up` 后，在 shell 中检查：

```sh
cat /proc/net/config
netstat
ping 192.168.1.1
ping example.com
git clone https://github.com/fQwQf/A20OS.git /tmp/A20OS
```

网线直连电脑时，电脑端需要给以太网口配置静态地址并开启转发/NAT。例如电脑
以太网口为 `enp3s0`，板子使用 `192.168.50.2/24`：

```sh
# 电脑（Linux，临时配置；接口名以 `ip link` 实际输出为准）
sudo ip addr flush dev enp3s0
sudo ip addr add 192.168.50.1/24 dev enp3s0
sudo ip link set enp3s0 up
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -o wlan0 -j MASQUERADE
```

然后在 U-Boot 的 bootargs 中传入静态参数（或在 FIT/启动脚本中加入同样的
参数），例如：

```text
a20.dhcp=0 a20.ip=192.168.50.2 a20.netmask=255.255.255.0 \
a20.gateway=192.168.50.1 a20.dns=1.1.1.1 a20.hostname=a20os
```

若电脑只提供链路、不提供 NAT，仍可 `ping 192.168.50.1`，但不能访问公网或
执行远程 `git clone`。交换机拓扑则不需要电脑转发；将板子和电脑接入同一网段，
使用路由器分配的地址即可。A20OS 当前网络栈提供 IPv4/IPv6、ARP、DHCP、DNS、
TCP、UDP、ICMP 和 HTTPS Git helper；`/proc/net/config` 是运行时确认最终地址、
网关和 DNS 的权威入口。

### 6.7 代码与板卡的回滚

保留上一份可启动的 `a20os-sd.img` 或其校验值。新版本不能启动时，关机、
将卡插回主机、卸载分区并重新写入上一份镜像即可。不要在板子运行时拔卡，
也不要用 `git reset --hard` 代替源码级回滚；需要回滚时用已知提交重新构建，
这样串口日志才能与源码对应。

更换软件或增加新命令的源码适配流程见
[从源码适配新软件](../development/source-software-porting.md)。

## 当前状态

- 启动链固件、FIT、四分区 SD 镜像及 FAT32/extra userspace 已完成源码构建；
  之前的板上镜像已经从 TF 卡启动并进入串口 mksh。每次修改网络驱动后仍需用
  新镜像重复串口启动记录，不能拿旧镜像日志替代当前提交。
- GMAC1 IRQ 线号 78、DTB 中的 YT8531 PHY 地址 0 和 RGMII RX 300 ps 延迟已由
  随镜像构建的 VF2 DTB 核对；当前驱动采用轮询数据面。PHY link-up、DMA 收发、
  `ping` 和公网 `git clone` 必须在接入网线的目标板上按本页 6.8 节逐项确认。

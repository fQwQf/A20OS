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

## 当前状态

- 启动链固件、FIT、四分区 SD 镜像及 FAT32/extra userspace 已在 VisionFive 2
  真机验证;系统可从 TF 卡完成启动并进入串口 mksh。
- GMAC IRQ 线号(78)与 SYS_CRG 寄存器偏移沿用 RocketOS 记录,首次上板
  需按 JH7110 文档/实测核对,见 physical-boards.md。

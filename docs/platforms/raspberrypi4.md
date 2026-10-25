# Raspberry Pi 4（BCM2711，AArch64）

A20OS 的 `raspberrypi4` 板级端口实现了 BCM2711 的内核启动、PL011 串口、GICv2、通用定时器、固件内存发现及 spin-table SMP。`raspberrypi4-qemu` 使用 QEMU 的原生 `raspi4b` 模型做内核功能验证；物理 Raspberry Pi 4 启动尚未验收，也没有固件 prepare、完整 SD 镜像或自动写卡流程。

## 系统支持

| 项目 | A20OS 实现与边界 |
| --- | --- |
| CPU / MMU | Cortex-A72，MMU 模式，内核容量为 1–4 核；物理入口 `0x80000`，标准 64 字节 AArch64 Image 头。入口接受 EL1/EL2/EL3 并归一到 EL1。 |
| SMP | 从 DTB 发现 CPU，使用 BCM2711 spin-table release slot；通过公共 secondary 启动、调度、IPI、Park/Wake、定时睡眠和退出回收路径。物理一致性初始化仍由固件完成。 |
| UART | PL011 `0xfe201000`、IRQ 153；GPIO14/15 ALT0，115200，分频要求 48 MHz UART 时钟。不是 mini UART。 |
| 中断 / 时间 | GICv2 `0xff841000/0xff842000`；ARM 通用计数器和物理定时器，时间换算读取 `CNTFRQ_EL0`，不硬编码模拟器频率。 |
| RAM | 低 1 GiB 直接映射窗口；从 DTB 读取实际 RAM，并在 DTB、自身 memreserve、静态 reserved-memory 之前保守裁剪连续分配前缀。不保证使用全部板载内存。 |
| 电源 | shutdown/reboot hook 停机，不实现物理断电或 watchdog 复位；终态日志不能作为电源关闭证据。 |
| 存储 / 网络 / USB / 显示 | 无 SD/eMMC、GENET、PCIe/USB、图形输入驱动或本板 rootfs。默认 bring-up 不启动用户态、shell 或网络服务。 |

四核共享同一芯片内的一致性 RAM，并使用同一内核的任务、PID 和堆。多块板通过普通网络互联不具备同样的共享内存、原子性和缓存一致性条件，不能由本端口的 SMP 支持推导出跨板线程迁移能力。

## 主机依赖

构建需要 Linux、GNU make、Python 3.11+、`aarch64-linux-gnu-gcc`/binutils、交叉 libc 开发头，以及项目 Rust 工具链、Cargo 和 `rust-src`。Makefile 构建 AArch64 soft-float Rust core；仅安装普通 `aarch64-unknown-none` target 不足以替代该步骤。

Debian/Ubuntu 安装示例：

```sh
sudo apt-get install build-essential git curl ca-certificates python3 tio \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu libc6-dev-arm64-cross qemu-system-arm
```

使用 rustup 配置 Rust 1.96.0；尚无 rustup 时先执行安装器，已有时可跳过前两条命令：

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 aarch64-unknown-none
rustup component add --toolchain 1.96.0 rust-src
git submodule update --init --recursive
```

模拟器需要 `qemu-system-aarch64` 支持 `raspi4b`；先确认机器列表：

```sh
qemu-system-aarch64 -machine help
```

有界 FDT 宿主测试另需宿主 C 编译器及 AddressSanitizer/UndefinedBehaviorSanitizer。QEMU fixture 由 Python 编码，不依赖 dtc。

## 构建与模拟器运行

在仓库根目录执行：

```sh
tools/a20 check raspberrypi4 raspberrypi4-qemu
tools/a20 build raspberrypi4-qemu -- -j8
tools/a20 show-vars raspberrypi4-qemu
tools/a20 run raspberrypi4-qemu
```

QEMU machine 是 `raspi4b`，固定 2 GiB RAM、四个物理核；不是通用 `virt`。内核默认配置四核。bring-up 结束后内核停机，QEMU 不会因板级 poweroff 自动退出；需要从宿主结束该模拟器会话。

构建产物位于 `.kernel-build/aarch64-raspberrypi4-*/`，末行 `Kernel-only build complete:` 显示本次准确 `kernel.bin` 路径。同目录包括：

- `kernel.bin`：带 AArch64 Image 头的内核，入口为 `0x80000`。
- `kernel.elf`：符号和 load segment 信息，用于调试及确认完整内存占用。
- `qemu-fixture.dtb`：仅供模拟器的 CPU/RAM/保留区描述，不能复制到实体板代替真实 DTB。
- 对象文件和生成配置；没有 rootfs 或整卡 SD 镜像。

可检查实际 ELF：

```sh
aarch64-linux-gnu-readelf -h -l /替换为构建输出目录/kernel.elf
```

完整板模型功能检查与 FDT 边界检查分别运行：

```sh
make -j8 check-raspberrypi4
make check-fdt-view
```

板模型检查在 `.kernel-build/cluster/raspberrypi4/` 写入 `report.json`、DTB fixture 和逐用例串口日志。它覆盖正常/乱序/稀疏 CPU、静态保留区及错误板型/CPU 描述的接受或拒绝路径，并要求公共 SMP 自检证明线程迁移、共享堆对象、timer IRQ、IPI、Park/Wake 和退出回收。FDT 宿主报告位于 `.kernel-build/cluster/fdt-view/`。这些是模拟器/宿主测试，不是实体固件或电气验收。

## 物理构建与部署边界

物理实例使用同一板级内核：

```sh
tools/a20 build raspberrypi4 -- -j8
tools/a20 show-vars raspberrypi4
```

仓库没有该板的 `prepare`、`package` 或 `deploy` 目标，也不取得 VideoCore 固件、生成 FAT 启动分区或修改 `config.txt`。因此不能从这些命令得到一张可写入空 SD 卡的系统镜像。物理实例只有构建和串口观察契约；其 `run` 仍选择模拟器，不能用它证明实体板运行。

实体交接需要外部已可工作的 Raspberry Pi 4 EEPROM/VideoCore 固件及适用的 armstub，并逐项满足：

1. 以 AArch64、MMU 关闭状态在 `0x80000` 启动 Image，`x0` 指向 8 字节对齐的有效 BCM2711 DTB；不能依赖固件默认加载地址恰好匹配。
2. 固件完成 DDR、系统缓存及 CPU 一致性初始化，设置 SMPEN，并使 GIC SGI/PPI/SPI 可由非安全 EL1 使用。启用 GIC 路由，不能替换成 BCM 旧中断控制器。
3. DTB 描述从 0 开始的实际 RAM、固件保留区，以及硬件 CPU0–3 的 `spin-table` 节点与 release slot `0xd8 + 8 * hw_id`；由硬件 CPU0 启动内核。内核不会自动将 PSCI CPU 描述转换成 spin-table。
4. DTB 位于低 1 GiB 可读窗口，大小不超过 2 MiB，不能覆盖内核。分配前缀还必须至少留出内核末尾之后的 16 MiB。
5. PL011 在 GPIO14/15，UART 时钟为 48 MHz；需要核对蓝牙/UART 路由和固件时钟，不能仅凭 `enable_uart` 推断全部契约满足。

官方固件的 `arm_64bit`、`kernel`、`armstub` 等设置可选择加载代码和 stub，但仓库尚未提供已验证的配置组合。上列条件没有闭合前，不提供“复制内核即可启动”的写卡步骤或替代 DTB。QEMU 的 fixture 不具备实体固件保留内存和时钟信息。

## 串口验收与故障定位

物理 Pi 4 使用 3.3 V TTL USB-UART，RX 接 GPIO14/TXD（40 针排针第 8 针），TX 接 GPIO15/RXD（第 10 针），共地（例如第 6 针）；使用板卡独立供电。实例参数为 115200、8N1、无流控。先打开串口，再上电：

```sh
tools/a20 console raspberrypi4 --serial /dev/ttyUSB0 --interactive
```

默认等待 60 秒并检查 `[RPi4] firmware CPUs=4`、`4/4 configured CPUs online` 和 `System ready`。日志默认写入 `.kernel-build/console/raspberrypi4.log`；`Ctrl-]` 退出。只有固件输出或 CPU 数不匹配时检查会失败；需要改变核数、超时或验收字符串时复制并修改物理实例，保留与内核构建一致的配置。没有自动 reset 线路，已经发生的串口输出不会重放。

应保留从上电开始的日志，包括固件版本、RAM 裁剪范围、counter 频率、实际在线核和异常信息。`System ready (bringup, no userspace)` 表示内核初始化完成；不代表有用户态 shell、磁盘或网络。

- 完全无输出：先检查供电、接线、UART 路由和 48 MHz 分频条件，再检查固件是否加载了指定内核。
- 早期 DTB panic：核对 BCM2711 compatible、RAM、对齐、spin-table、release slot 和保留区；不要换用模拟器 DTB。
- 从核离线或 timer/IPI 自检失败：检查固件的一致性、GIC Group 1、release slot 和计数器权限，保留完整日志。
- 打印关机后没有断电：符合当前 halt 实现；需要外部断电，不应归因于测试工具失灵。

## 一手参考

- [BCM2711 外设手册](https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf)：GPIO、UART 和地址布局。
- [Raspberry Pi 官方 config.txt](https://www.raspberrypi.com/documentation/computers/config_txt.html)：固件加载、armstub 与串口配置选项。
- [官方 armstub8 源码](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S)：一致性、计数器和 GIC 初始化。
- [BCM2711 上游 DTS](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm2711.dtsi)：真实固件/设备树适配参考。
- [ARM64 启动协议](https://docs.kernel.org/arch/arm64/booting.html) 与 [QEMU Raspberry Pi 模型](https://www.qemu.org/docs/master/system/arm/raspi.html)：Image 交接条件和模拟器能力边界。

# ESP32-C3

ESP32-C3 的 RV32IMC、单核、M-mode/NOMMU MCU 内核。它复用 A20OS 的 MCU 裁剪、堆、任务生命周期、调度器、睡眠队列、completion 和 C/Rust 公共实现；没有在 ESP-IDF/FreeRTOS 任务中运行另一个内核。主机打包工具使用 esptool，固件不链接 ESP-IDF。

## 支持范围与验证边界

| 项目 | 当前实现 |
|---|---|
| CPU | RV32IMC + Zicsr/Zifencei，ILP32 软浮点，M-mode 异常和整数上下文切换 |
| 内核 | 单核任务、IRQ 返回点抢占、定时睡眠、同步对象、MCU 堆、串口命令行 |
| 原子操作 | 单核关中断软件原子运行时，覆盖 1/2/4/8 字节；不要求 A 扩展 |
| UART0 | GPIO21 TX / GPIO20 RX，115200 8N1；定时中断轮询接收 |
| 定时器 | SYSTIMER unit0/target0，16 MHz 计数，1 kHz 周期中断 |
| 中断 | C3 中断矩阵将 SYSTIMER source37 映射到 CPU IRQ1，向量式 mtvec |
| 启动/恢复 | ROM SPI 启动镜像，全部加载到 SRAM；ROM 下载模式重新刷写 |
| 看门狗 | 启动时关闭 TIMG/RTC WDT，super WDT 自动喂狗；无运行期监控服务 |
| 存储与网络 | 暂无 Flash 文件系统、Wi-Fi/BLE、SPI/I2C/ADC/LEDC 设备服务 |
| 用户程序 | 当前运行受信任内核任务；无 Linux 用户态、ELF 用户程序或隔离保证 |

验证分三层：主机检查与链接、上游 QEMU `virt` 的 RV32IMC 公共内核回归、Espressif QEMU `esp32c3` 的 SoC/ROM 镜像回归。模拟器通过不代表实板通过。**实机尚未验证**；具体开发板的供电、串口电路、芯片修订版和 Flash 型号仍需按下述步骤验收。

适用 ESP32-C3 模组/开发板，不适用 Xtensa ESP32/ESP32-S3，也不应直接用于 C2/C6/H2。没有假定板载 LED 的引脚，启动不会驱动未知 GPIO。板上的 USB 接口可能接 USB-UART 桥，也可能直连 C3 USB Serial/JTAG；当前控制台使用 UART0，直连 USB 的板子需外接 3.3 V UART。 USB Serial/JTAG 下载与 USB 控制台是不同功能，当前没有 USB 控制台驱动。

## 主机依赖

Linux 下准备 Python 3.11.8+、GNU make、`riscv64-unknown-elf-gcc`/binutils、Rust/Cargo，以及当前仓库指定工具链的 `rust-src`。部分共享调度/任务模块为 Rust，不能省略 Rust。

```sh
sudo apt-get install build-essential git python3 python3-venv \
    gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf
git submodule update --init --recursive
rustup component add rust-src
```

从仓库根目录执行后续命令。`tools/a20` 本身只需 Python 标准库。普通内核构建离线完成；首次打包前显式准备 esptool 4.8.1（需要网络），只安装到 `build/esp32-c3/venv/`：

```sh
tools/a20 prepare esp32-c3
tools/a20 check esp32-c3
tools/a20 trim esp32-c3
tools/a20 build esp32-c3 -- -j4
tools/a20 package esp32-c3 -- -j4
```

Rust 使用 `tools/rust-targets/riscv32imc-a20.json` 构建匹配的 core。它的 ILP32、无 A/F/D 目标与现有 QEMU RV32 的 ILP32D 目标分离，不能复用后者的目标库。

## 内存与镜像

当前采用保守、无需 Flash cache/XIP 的 ROM 直接启动布局：

| 窗口 | 地址 | 预算 |
|---|---|---|
| 指令 SRAM | `0x40380000..0x403a0000` | 128 KiB，只放代码及异常向量 |
| 数据 SRAM | `0x3fca0000..0x3fcdc000` | 240 KiB，rodata/data/BSS/堆/栈 |
| 启动栈 | 数据窗口顶部 | 8 KiB |
| 内核任务栈 | 从 MCU 堆分配 | 每任务 4 KiB |

IRAM/DRAM 是 SRAM 别名，以上两段对应不重叠的物理内存；上部 SRAM 留给 ROM 加载器。链接器要求初始堆至少 64 KiB。并未把标称 400 KiB 全部当作可分配空间。代码全部驻 SRAM，运行时不擦写 Flash，不进行时钟/Flash cache 重配置。

`tools/a20 ledger esp32-c3` 查询当前构建目录、产物、尺寸及 SHA256。打包目录为：

| 文件 | 用途 |
|---|---|
| `build/esp32-c3/handoff/a20os.bin` | ESP32-C3 ROM 镜像，写入 Flash `0x0` |
| `build/esp32-c3/handoff/kernel.elf` | 调试用 ELF，入口 `_start` |
| `build/esp32-c3/handoff/image.json` | 芯片、布局、入口、镜像/ELF 哈希与验证边界 |
| `build/esp32-c3/handoff/README.md` | 本操作说明 |

构建目录中的普通 `kernel.bin` 是 objcopy 导出的平面二进制，跨越 SRAM 别名地址，**不能烧录**。打包器检查 ELF 的软浮点 ABI、加载范围、入口、段重叠，以及 ROM 镜像芯片 ID、逐字节加载内容、校验和与 SHA256。当前镜像是 ROM 首级程序，无二级 bootloader、分区表、OTA、Secure Boot 或 Flash Encryption 启动适配；烧录会替换原有启动镜像，先保留原固件与其恢复方法。已有安全启动/Flash 加密配置的设备不在此启动方案的支持范围内。

## 烧录与串口验收

外接 UART：板 GPIO21/TX → 适配器 RX，GPIO20/RX → 适配器 TX，GND 共地，使用 **3.3 V** 信号。开发板若已将 UART0 接至 USB-UART 桥，直接使用对应桥接串口。自动下载/复位要求板载桥接电路正确连接 EN/GPIO9；否则按板卡说明手动进入 ROM 下载模式。

显式指定串口，避免选中其他设备；下面 `/dev/ttyUSB0` 需替换为实际端口：

```sh
tools/a20 flash esp32-c3 -- ESPTOOL_PORT=/dev/ttyUSB0
# 刷写后打开控制台并复位、检查启动和命令：
tools/a20 console esp32-c3 --serial /dev/ttyUSB0
# 合并构建、刷写、复位与串口验收：
tools/a20 deploy esp32-c3 --serial /dev/ttyUSB0
```

`console` 在打开 UART 后通过 esptool 的 HardReset/RTS 复位；需要同一串口支持复位。无自动复位线路时使用 `--no-reset`，开始监听后手动按 EN/RESET。原生 USB 下载端口与外接 UART 控制台不是同一设备时，分别使用 `flash ... ESPTOOL_PORT=<下载端口>` 和`console ... --serial <UART端口>`；不要用单端口 `deploy`。

仅查看操作计划或仅刷写：

```sh
tools/a20 deploy esp32-c3 --serial /dev/ttyUSB0 --dry-run
tools/a20 deploy esp32-c3 --serial /dev/ttyUSB0 --no-console
python3 tools/esp32c3.py verify
```

esptool 在写入前检查芯片身份、检测 Flash 容量，并校验烧录结果；工具不主动全片擦除。固件 UART 波特率沿用 ROM SPI 启动的 115200 8N1，不依赖主机下载时选择的波特率。

预期看到：

```text
A20OS RV32 M-mode MCU board=esp32-c3
[BOOT] scheduler initialized heap=... bytes
[MCU TEST] preemption=irq-return
[MCU TEST] PASS heap atomics timer sleep context-switch completion worker=...
A20OS MCU READY
a20>
```

`help`、`info`、`heap`、`ticks`、`tasks`、`test`、`reboot` 是当前内核命令。启动自测和 `test` 会检查堆读写、同步与定时调度；启用抢占时，测试中的忙循环不主动让出 CPU，要求 worker 仍取得进展。命令不是完整 POSIX shell。日志保存于`.kernel-build/esp32-c3/console.log`。`TRAP cause=... pc=... addr=...` 或 panic 表示失败；只有镜像校验/写入成功而没有启动标记，不能记录为实机启动通过。

## 模拟回归

公共执行环境无需 Espressif 模拟器：

```sh
tools/a20 test qemu-riscv32-mcu -- -j4
```

该实例使用上游 QEMU `virt -bios none -cpu lowrisc-ibex`，刻意不提供 A/F/D 扩展或 SBI。它验证共用内核，不能代替 C3 外设验证。在 Linux x86_64 上显式准备 Espressif QEMU （版本 `esp-develop-9.2.2-20260417`，下载后校验官方 SHA256），再运行两种 C3 测试：

```sh
make esp32-c3-qemu-prepare
tools/a20 test esp32-c3-qemu -- -j4
make ARCH=riscv32 BOARD=esp32-c3 ABI=native check-esp32-c3-rom -j4
```

其他宿主自行准备对应的 Espressif QEMU，并在上述命令尾部使用`ESP32C3_QEMU=/绝对路径/qemu-system-riscv32`（`tools/a20` 命令需放在 `--` 后）。普通发行版 QEMU 通常没有 `esp32c3` 模型。`esp32-c3-qemu` 经 `-kernel` 加载 ELF，`check-esp32-c3-rom` 则把已校验镜像置于模拟 Flash 的 `0x0`，从芯片 ROM 启动；后者必须观察 ROM 的 `load/entry`、内核自测和串口命令结果。日志分别为 `.kernel-build/smoke/esp32-c3-qemu.log` 与 `esp32-c3-rom.log`， ROM 检查另生成同名 JSON 报告，明确 `hardware_validated=false`。

相关主机回归与工程门禁：

```sh
python3 -m unittest tools.tests.test_esp32c3 tools.tests.test_a20_trim \
    tools.tests.test_mcu_heap_backend
make check-manifests
```

测试还覆盖错误芯片镜像、校验损坏/截断、错误浮点 ABI、越界加载，以及 C3 的单核/Native 约束。低剩余磁盘的主机若被 `tools/a20` 的通用资源预算阻止，可为这些小型 MCU 回归显式设置 `A20_MIN_DISK_MB=512`；这不改变固件的内存预算。

## 恢复与限制

若无启动输出，先检查 UART0 与 USB-UART 的连接方式、波特率及 ROM 启动日志，然后检查是否陷入看门狗复位或出现异常。按开发板说明拉低 GPIO9、复位进入 ROM 下载模式，再用 esptool 重新刷写原固件或本镜像；退出下载模式后释放 GPIO9 并再次复位。不需要修改 eFuse，也不要以修改 eFuse 作为启动调试步骤。

Wi-Fi/BLE 需要无线固件、驱动及其 OS 适配，当前未接入；后续应复用现有网络栈与设备模型。其他外设也应按共享驱动协议、板级资源描述的边界接入。无 MMU 并不等于没有 Flash 映射硬件，本移植只是暂不使用 XIP。全任务处于 M-mode，无进程内存隔离；当前系统不支持 SMP。

## 实现与官方参考

- [裁剪策略](../../components/trim.toml)：`profile.mcu.boards` 按 `arch/board` 选择，不改变全部 RV32 平台。
- [公共 M-mode 执行层](../../kernel/arch/riscv32/mcu/) 与 [板级资源](../../kernel/arch/riscv32/boards/esp32-c3/)。
- [ESP32-C3 数据手册](https://documentation.espressif.com/esp32-c3_datasheet_en.html)。
- [ESP32-C3 技术参考手册](https://documentation.espressif.com/esp32-c3_technical_reference_manual_en.pdf)。
- [ESP-IDF v5.4.2 寄存器定义](https://github.com/espressif/esp-idf/tree/v5.4.2/components/soc/esp32c3/register/soc)与 [ROM 符号](https://github.com/espressif/esp-idf/blob/v5.4.2/components/esp_rom/esp32c3/ld/esp32c3.rom.ld)。
- [esptool v4 镜像格式](https://docs.espressif.com/projects/esptool/en/release-v4/esp32c3/advanced-topics/firmware-image-format.html)。

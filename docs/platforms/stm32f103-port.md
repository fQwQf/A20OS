# STM32F103 / Cortex-M3

本平台是 **ARMv7-M、无 MMU 的 MCU 内核配置**，不是完整的 Linux/POSIX 系统镜像。源码包含复位入口、UART、NVIC、SysTick、通用内核调度器接入、堆分配和板级外设服务；默认没有独立用户态进程、shell、完整 VM 或通用网络栈。编译、烧录校验、复位启动与每个外设的实机验收是四个不同结果，不应相互替代。

## 目标与支持状态

| 实例 | 目标和资源 | 支持入口 |
| --- | --- | --- |
| `stm32f103` | 通用 STM32F103，64 KiB Flash / 20 KiB SRAM | UART、堆、定时器与 idle 调度器基础固件；不创建 Xuanwu 外设服务线程 |
| `stm32f103-xuanwu` | 普中玄武 STM32F103ZET6，512 KiB Flash / 64 KiB SRAM | 编译外设配置，注册的 CMSIS-DAP/SWD OpenOCD 烧录 |
| `stm32f103-qemu` | QEMU `stm32vldiscovery`，128 KiB Flash / 8 KiB SRAM | 编译并模拟 UART/初始化/受限调度器 idle probe；不是 Xuanwu 外设模型 |

三个配置都使用 `ARCH=armv7m BOARD=stm32f103`，由 MCU profile 自动选择 NOMMU、 bring-up 和 embedded 驱动。Flash 地址为 `0x08000000`，SRAM 从 `0x20000000` 起。不得把 512 KiB 的 Xuanwu 固件或烧录配置用于 64 KiB 芯片。

实际 Xuanwu 外设运行、抢占可靠性和接线组合须在目标板重新验收。本页列的是源码接口和可执行操作，不将旧硬件日志或 QEMU 初始化成功当作当前实板通过。

## 宿主依赖

在 Linux 主机的仓库根目录操作。Python 需 3.11 或更新版本，需 GNU make、宿主 C 工具链以及 ARM bare-metal 工具链；QEMU 和 OpenOCD 分别用于模拟和烧录：

```sh
sudo apt install build-essential git python3 gcc-arm-none-eabi binutils-arm-none-eabi \
  openocd qemu-system-arm picocom
```

仓库若启用 Rust 组件，使用 `rust-toolchain.toml` 选择的工具链。安装[rustup](https://rustup.rs/) 后准备对应目标：

```sh
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 thumbv7m-none-eabi
git submodule update --init --recursive
```

Makefile 也有 Clang `--target=arm-none-eabi` 备选，主流程建议使用上述 GCC 工具链。烧录需能访问 USB 调试器及主机 udev 权限，串口需能访问相应 tty 设备。 QEMU 无需物理板或调试器，实板烧录无需启动 QEMU。

## 构建与产物

基础芯片和玄武板分别构建，不混用几何参数：

```sh
tools/a20 check instances/stm32f103.toml instances/stm32f103-xuanwu.toml \
  instances/stm32f103-qemu.toml
tools/a20 build stm32f103
tools/a20 build stm32f103-xuanwu
tools/a20 ledger stm32f103-xuanwu
```

产物在该实例配置的构建目录中：`kernel.elf` 含地址/调试信息，`kernel.bin` 是从 Flash 起点开始的平面固件。目录包含配置指纹，不应照抄历史目录名。可直接查询：

```sh
a20_stm32_base=$(make -s ARCH=armv7m BOARD=stm32f103 \
  STM32_FLASH_KB=64 STM32_RAM_KB=20 print-build-dir)
a20_stm32_xuanwu=$(make -s ARCH=armv7m BOARD=stm32f103 \
  STM32_FLASH_KB=512 STM32_RAM_KB=64 STM32_XUANWU=1 print-build-dir)
ls -l "$a20_stm32_base/kernel.elf" "$a20_stm32_xuanwu/kernel.elf"
arm-none-eabi-size "$a20_stm32_xuanwu/kernel.elf"
```

本平台没有 `[package]` 和 SD/USB 启动镜像；部署对象就是 ELF/Flash 固件。`tools/a20 package stm32f103-xuanwu` 不是有效打包入口。`make check-stm32f103` 进行架构边界静态检查并构建 Xuanwu 固件，**不运行 QEMU、不烧录、不验证实板**。`tools/a20 trim stm32f103` 可查看裁剪配置。

## 玄武板烧录

确认芯片为 STM32F103ZET6，调试器连接 SWDIO、SWCLK、GND 和所需电压参考。板子通过自身电源正确供电，避免不同电源相互馈电；按调试器规范连接参考电压。使用 SWD 保留调试端口；正常从 Flash 复位启动时 BOOT0 保持低电平。

默认烧录使用 `interface/cmsis-dap.cfg`、`target/stm32f1x.cfg`、SWD、1000 kHz：

```sh
tools/a20 flash stm32f103-xuanwu --dry-run
tools/a20 flash stm32f103-xuanwu
```

此命令会擦写固件覆盖的 Flash 区域。先保存需要保留的原固件；Flash 中不能有不可丢弃数据。工具先构建，再执行写入和 `verify_image`，从向量表读出 MSP/PC 并恢复执行。它没有使用通用 `reset halt`，因此写完之后还应单独断开调试器、复位验证冷启动。

需要指定同型号 probe 或降低速率时：

```sh
tools/a20 flash stm32f103-xuanwu -- \
  STM32_CMSIS_DAP_SERIAL=实际调试器序列号 STM32_OPENOCD_ADAPTER_KHZ=500
```

其他 probe 需修改本地实例 `[flash].interface` 等字段，并核对其 OpenOCD 配置，不能只把设备名当作 CMSIS-DAP 参数。注册后端仅允许 512/64 KiB 的 Xuanwu 几何配置，通用 64/20 KiB 实例没有 `a20 flash` 后端。

基础芯片可使用独立 STM32CubeProgrammer：选择实际芯片/接口，写 ELF，或把`kernel.bin` 写至 **`0x08000000`**，执行校验后复位。使用其他工具时，同样先核对芯片容量和文件地址，不要绕过目标型号检查。

## 串口、启动和可观察验收

USART1 **PA9=TX、PA10=RX，115200、8N1、无流控**。使用 3.3 V TTL 串口，TX/RX 交叉并共地，不连接 RS-232 电平。按实际 tty 替换：

```sh
picocom --baud 115200 --flow n /dev/serial/by-id/实际串口设备
```

这些 STM32 实例没有 `[target]` 串口会话配置，因此此处使用串口终端，不能直接期望`a20 console stm32f103-xuanwu` 具备自动验收。先连终端再复位，保存日志并检查：

1. `A20OS ARMv7-M STM32F103 bringup`、正确 board 和 heap 大小。
2. `[BOOT] allocator=ok`；Xuanwu 配置另外检查实际外设 probe 的 ready/failure 状态。
3. `[BOOT] scheduler initialized`。
4. `[BOOT] SysTick=1000Hz source=HCLK=...`；继续观察计数/服务任务是否推进。
5. 脱离调试器后重复冷启动，确认不是 probe 留下的时钟/寄存器状态导致偶然成功。

普通配置使用 HSI；Xuanwu 尝试 8 MHz HSE ×9 的 72 MHz PLL，失败回退到 HSI。 UART BRR 和 SysTick reload 从实时 RCC 时钟推导。日志显示的是配置与运行状态； 1 kHz 字符串不等同于用外部仪器测得的定时器精度。

USART1 RX 中断只送入 ring buffer，当前没有文本命令解析器。`uart`、`perf`、`sd retry`、`bt retry` 都不是有效命令，也没有 `cat` 或 shell。

## Xuanwu 可选外设

板级组合由 `kernel/arch/armv7m/boards/stm32f103/` 管理，协议驱动位于`kernel/arch/armv7m/boards/stm32f103/drivers/`。以下为当前接口，需实际安装对应硬件才可验收：

| 外设 | 接口及验收 |
| --- | --- |
| 外部 SRAM | FSMC Bank1 NOR/SRAM3 `0x68000000`；probe 成功后检查容量/读写与 allocator；标称容量不能替代探测 |
| LCD | 16 位 FSMC、HX8357D 兼容路径；先观察自测色帧，再检查状态页和 uptime；不同面板不保证兼容 |
| 触摸 | XPT2046 兼容：PB1 CLK、PB2 DOUT、PF9 DIN、PF10 PEN、PF11 CS；以按下/抬起和坐标证明真实输入 |
| TF 卡 | SDIO PC8–PC12、PD2；检查容量、总线宽度、FAT32 元数据和插拔恢复；不自动成为用户态 rootfs |
| 按键 | PA0、PE2/PE3/PE4；确认去抖后事件及页面动作 |
| 光感/背光 | PF8 ADC3 channel 6、PB0 背光；ADC 相对值不是 lux 测量 |
| HC-05 | USART3 PB10/PB11，PA4 KEY、PA15 状态；默认 38400；检查模块实际响应、连接与双向传输 |
| ESP8266 | USART2 PA2/PA3，PC6 EN、PC7 RESET；需独立满足 3.3 V 电流；AT 响应不等于 AP/TCP 已连通 |
| 电机 | PB5/TIM3_CH2 partial remap；仅在匹配驱动电路/负载下验证 PWM |

可选外设失败会在 UART 报告，不能由内核继续运行推断该设备正常。 HC-05 缺省名称/PIN/UUID/数据率可用 `STM32_BT_NAME`、`STM32_BT_PIN`、`STM32_BT_UUID`、`STM32_BT_BAUD` 构建变量调整；模块固件是否支持读回必须按日志判断。 TF 数据写测试使用可丢弃卡；驱动的单扇区 I/O、FAT32 元数据解析不等于完整 VFS 已挂载。

## QEMU 独立验证

```sh
tools/a20 build stm32f103-qemu
tools/a20 run stm32f103-qemu
```

实际 launcher 是 `qemu-system-arm -machine stm32vldiscovery -nographic -kernel ...`，使用 **128 KiB Flash / 8 KiB SRAM** 的专用产物。终止 QEMU 使用 `Ctrl-A X`。观察 allocator、scheduler、SysTick 初始化，以及`[SCHED] constrained QEMU idle probe online`。当前模型只有受限 idle probe，不应期待双忙循环 `PREEMPT PASS`，也不能据此宣称物理多任务抢占通过。 QEMU 不模拟 Xuanwu LCD、SDIO 卡、电气连接或外部 SRAM fabric。

## 故障与边界

| 现象 | 检查 |
| --- | --- |
| 无编译器/链接失败 | ARM bare-metal 工具链和所选 Flash/SRAM 几何；不要改大 RAM 数值掩盖溢出 |
| 找不到调试器 | USB 权限、interface cfg、probe 序列号、电源、SWD 连线和速率 |
| 烧录校验通过但复位无输出 | BOOT0、向量表/Flash 起点、PA9 接收线、115200、时钟及独立复位 |
| allocator 失败或任务分配 panic | 实际 SRAM 容量和 MCU profile 内存预算；QEMU 8 KiB 限制不同于物理板 |
| 有启动日志但外设不可用 | 具体板型、跳线、外设供电、pinmux 和该 probe 的错误信息 |
| 串口键入无响应 | 当前无命令解析器；只用日志和面板/物理输入验收 |

普通 MCU profile 的任务是同一地址空间的内核线程，没有 MMU 隔离。默认关闭的`MCU_SHARED_KERNEL=1` 需要额外共享 RAM 和仲裁硬件，其接线不等于 Xuanwu 单端口 SRAM；不属于本页可直接烧写运行的配置。调度器、外设 DMA、完整 VFS/网络与共享硬件各自需要独立验收，不能从当前初始化标记推出。

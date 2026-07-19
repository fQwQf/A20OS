# STM32F103 移植与运行手册

> ❌ 不要这样做：不要把板级常量或引脚定义藏进可复用的 `kernel/drivers/stm32f1/` 驱动里。可复用驱动应通过 board config 或 platform data 获取资源；引脚和时钟这些事实属于 `kernel/platform/stm32f103/`。

STM32F103 是 ARMv7-M/Cortex-M3、无 MMU 的 bring-up profile。它复用 A20OS 的架构和 board 边界：

- `kernel/arch/armv7m`：vector table、reset path、CPU 原语、exception frame、PendSV/SVC 抢占、SysTick、fault handling、firmware hooks、编译器 runtime helper。
- `kernel/drivers/stm32f1`：可复用的 STM32F1 UART、SDIO、display、input、sensor、radio、RTC、watchdog、外部 SRAM 和 actuator 驱动。
- `kernel/platform/stm32f103`：clock/memory 配置、NVIC-facing board 操作、外部 IRQ 路由、board/device 组合。
- `kernel/mcu`：小内存 kernel profile 和 allocator。

MCU profile 链接通用调度器和一小部分 NOMMU/VFS，但不包含完整网络或 VM 子系统。STM32F103 通常只有 20 KiB SRAM 且无 MMU，所以这些设施需要嵌入式配置层，而不是只加新 CPU 和设备驱动。当前镜像已验证：reset/data/BSS 设置、UART 输出、IRQ 进入、1 kHz SysTick、抢占式任务调度、设备服务任务和动态分配。

## 构建固件

默认 bring-up：

```sh
make stm32f103-bringup
```

输出：

```text
.kernel-build/armv7m-both-bringup-nommu-stm32f103-f64k-r20k/kernel.elf
.kernel-build/armv7m-both-bringup-nommu-stm32f103-f64k-r20k/kernel.bin
```

二进制链接到 flash 地址 `0x08000000`。用 OpenOCD、ST-Link 或其他 STM32 编程器烧写。USART1 用 PA9/PA10，115200 8N1。通用 bring-up 目标使用 8 MHz HSI 时钟。

普中玄武 STM32F103ZET6 板（512 KiB Flash / 64 KiB SRAM）用：

```sh
make stm32f103-xuanwu
```

同时构建无外设基线和完整 Xuanwu 镜像：

```sh
make check-stm32f103
```

输出：

```text
.kernel-build/armv7m-both-bringup-nommu-stm32f103-f512k-r64k/kernel.bin
```

接上 CMSIS-DAP probe 并安装 OpenOCD 后：

```sh
make flash-stm32f103-xuanwu
```

默认 OpenOCD interface 是 `interface/cmsis-dap.cfg`，用 SWD 1 MHz。可按需要覆盖：

```sh
make flash-stm32f103-xuanwu \
    STM32_OPENOCD_ADAPTER_KHZ=400
```

PZ CMSIS-DAP firmware 不能可靠完成 OpenOCD 通用 `reset halt` 序列。Makefile 因此通过 Cortex-M Debug Halting Control and Status Register 停住，写入并校验 flash，再从 flash vector table 恢复 MSP 和 PC 后恢复运行。可用 `STM32_CMSIS_DAP_SERIAL=<serial>` 选择特定 probe。

## Xuanwu 时钟与 UART

Xuanwu 目标启用 8 MHz HSE 并切换到标称 72 MHz HSE×9 PLL 时钟，然后初始化 UART 和 SysTick；如果 HSE 或 PLL 启动失败则回退到 HSI。APB1 跑 36 MHz，APB2 跑 72 MHz。USART1 驱动从实时 RCC 寄存器读取 APB2 时钟，计算 BRR，而不是直接假设示例里的 72 MHz。RXNE 中断把数据放进 128 字节接收 ring，并通过所需的 SR/DR 读取序列清除 parity、framing、noise 和 overrun 错误。

## 电机与 LCD

PB5 分配给 Xuanwu 板的 ULN2003 电机输入，由 TIM3_CH2 通过 partial remap 驱动。Xuanwu 构建中禁用之前 PB5 的心跳 LED，避免破坏风扇 PWM 波形。

板载 16-bit FSMC LCD 在启动时初始化。背光在 PB0 有信号时启用。面板复位接到板级复位，而不是某个 GPIO。固件先显示红、绿、蓝自测帧，然后画一个带实时 uptime 计数器的 A20OS 状态页。驱动按 vendor 示例的默认 HX8357D 兼容 3.5 英寸面板路径；该面板通过 STM32 16-bit FSMC 总线连接，但每个 RGB565 像素仍拆成两个 8-bit 写入。

所有 Xuanwu 外设都是运行时可选。LCD、外部 SRAM、TF 卡、触摸初始化失败都会通过 USART1 报告，然后被忽略；内核仍会到达 SysTick/WFI 心跳循环。普通 `stm32f103-bringup` 目标编译同一个 platform manager，但禁用所有 Xuanwu 专用 probe，这就是无外设构建门禁。

## TF 卡与 SDIO

板载 TF 卡座使用 STM32 SDIO 引脚 `PC8-PC12` 和 `PD2`。驱动支持 SDSC 和 SDHC/SDXC 卡、1-bit fallback 和 4-bit 传输、单扇区读写、通用 `block_dev_t` 接口、FAT32 boot sector 检测、卷标报告、定期健康检查、移除处理和插入后自动重新 probe。MCU profile 不在进程可见命名空间里暴露这张卡。小型 board adapter 可以挂载 FAT32lite 用于校准和内核侧访问，同时 SDIO 驱动也发布通用 `block_dev_t` 接口。

## 触摸与按键

3.5 英寸面板搭载 XPT2046 兼容电阻触摸屏：

```text
PB1 = T_CLK
PB2 = T_DOUT
PF9 = T_DIN
PF10 = T_PEN
PF11 = T_CS
```

触摸每 20 ms 采样一次，带 trimmed sampling、噪声剔除、平滑和可配置轴校准。XPT2046 没有可读 device ID，所以空闲面板和缺失面板都无害地表现为 armed interface，直到 `T_PEN` 被拉低。

四个黄色按键在触摸不可用时也可用。vendor 映射：

```text
PA0  active-high
PE4  active-low
PE3  active-low
PE2  active-low
```

它们每 20 ms 采样并做 debounce，对应上、左、下、右事件（顺序为 `PA0`、`PE2`、`PE3`、`PE4`）。左右键在 `STATUS`、`MEM`、`TF`、`BT`、`INPUT TEST` 页面间切换；上下键选择状态行；右键打开选中的 memory、storage、Bluetooth 或 input 行。在 input 页，上键移动可见测试光标，下键清除画布。

## 光感与背光

板载光敏电阻在 `PF8` 通过 `ADC3 channel 6` 采样，与 vendor 光感实验一致。每次读取做 10 次真实 12-bit 转换，去掉最高和最低样本，然后平均并低通滤波。LCD 系统页显示测到的 ADC 值和 vendor 兼容的相对亮度 0-100。这故意不标成 lux，因为电阻网络没有工厂光度学校准。

LCD 背光控制驱动板文档里标为高电平有效的 `LCD_BL` 信号，接在 `PB0`。亮度调制和自动亮度都关闭；PB0 保持高电平，面板维持正常硬件亮度。光敏电阻继续独立采样并显示。

## 内存页测量

内存页使用运行时测量，而不是把 Makefile 容量设置当成硬件事实。在 Xuanwu 上：

- Flash 容量从 STM32 工厂 size 寄存器读取。
- 内部 SRAM 容量从实时 DBGMCU 设备标识符和工厂 Flash density 推导；未知设备会显式使用链接布局 fallback，而不是假装测到了硅片容量。
- 固件 Flash 使用来自最终链接的 load-image 边界。
- 内部 RAM 使用结合静态 data/BSS 范围、实时 allocator 元数据和分配、以及从复位时填充模式测得的栈高水位。
- 外部 SRAM 容量通过逐次探测地址线边界直到第一个镜像地址，然后在该区间内用保存/恢复模式校验，来确定。
- 外部 SRAM 的 live allocator 使用只有 probe 成功后才单独显示。

板载 1 MiB 异步 SRAM 位于 FSMC Bank1 NOR/SRAM3 的 `0x68000000`。可通过 `stm32_extsram_alloc()` 和 `stm32_extsram_free()` 分配，用于未来 framebuffer、cache 或文件系统 working set，而不消耗内部 64 KiB SRAM。`kmalloc()` 在内部堆无法满足分配时也会自动使用外部 SRAM，但内部 SRAM 仍是首选。

## 蓝牙 HC-05

PZ-HC05 蓝牙模块接在 `USART3` 的 `PB10/PB11`，38400 8N1。`PA4` 驱动模块 KEY 输入，保持低电平进入透明数据模式；`PA15` 在释放 JTAG-only 引脚后接收专用 PIO9 连接状态输出，SWD 保持启用。PA15 使用上拉，只有当信号保持高电平 500 ms 后才接受为已连接；这能防止缺失模块或 LED 脉冲被误判为连接。

默认名称 `KasaneTeto`，PIN `2233`，UUID `0x1101`，波特率 `38400`。构建时可覆盖：

```sh
make stm32f103-xuanwu \
    STM32_BT_NAME=MY-BOARD STM32_BT_PIN=6789 \
    STM32_BT_UUID=1101 STM32_BT_BAUD=38400
```

服务是经典蓝牙 SPP。标准 HC-05 固件把 UUID 固定为 `0x1101`（`00001101-0000-1000-8000-00805F9B34FB`），不暴露 `AT+UUID`；兼容固件如果实现了该命令，会写入并读回。标准 HC-05 上请求非 `1101` 的 UUID 会被报告为 unverified，而不是显示为真实模块状态。

USART3 接收走中断驱动 ring buffer，按换行或 20 ms 空闲间隔分帧。Bluetooth 页显示已验证 role、名称、PIN、UUID 来源、连接状态和字节计数。测试发送按钮只在 PA15 报告真实连接时成功。

启用 USART3 前，驱动会先测试 PB11 是否跟随内部上拉/下拉。如果跟随，说明模块 TX 路径浮空，启动会报告 `rx=floating`。在 Xuanwu 上通常意味着 P10 的 `USART3_TX`/`W_TX` 和 `USART3_RX`/`W_RX` 跳线帽缺失或接反、模块插反或未供电。正常上电空闲模块会报告 `rx=driven-high`。

完整 AT probe 不会自动重复，因为缺失模块可能阻塞外设服务任务数秒。改变接线或供电后用 `bt retry` 触发重新探测。

## USART 共享驱动

USART1、USART2、USART3 共享 STM32 UART 寄存器驱动。它从实时 RCC 时钟树推导各外设时钟，计算并报告实际 BRR 值，每次使用前复位 USART，配置 8N1 TX/RX 引脚，排空硬件错误，并控制对应 NVIC 线。USART3 会显式清除 remap 位，确保模块始终使用 Xuanwu 的 `PB10/PB11` 路由，即使调试器或之前烧写的应用把 AFIO 留在其他状态。

## ESP8266 WiFi

两个射频可同时工作。板座上直接插着 HC-05，Dupont 线连接的 ESP8266 用 `USART2`（`PA2` TX 接 ESP RX，`PA3` RX 接 ESP TX），`PC6` 驱动 CH_PD/EN，`PC7` 驱动 RESET。两个模块和 MCU 必须共地；ESP8266 供电要能提供足够 3.3 V 电流。USART2 和 USART3 有独立的接收中断和 ring buffer，所以 WiFi probe 不会占用或重新配置蓝牙 UART。

ESP8266 是 ESP8266EX，1 MiB Flash，官方 Nano AT v1.7.4.0（SDK v3.0.4），115200 8N1。非阻塞 probe 拉低 RESET 500 ms，等待 2 秒，然后最多重试 3 次候选波特率，从 115200 开始。硬件测试已确认 reset 控制和两个 UART 方向。上一次失败是 PA2 到模块 RX 之间的 Dupont 线损坏；换线后 AT 硬件链路完成。AP 关联和代理 TCP 连接是独立的配置/集成步骤。

板级 early-init 保持 PA4 低电平，让模块进入可发现数据模式。配置严格按 PZ 示例的运行时 AT 序列：先反复尝试 9600，发送命令时拉高 KEY，发送完立刻拉低 KEY，然后等待响应。收到第一次有效回复后，测试后续命令需要 KEY 低、每命令脉冲或保持高。然后检查其他支持的数据率。写入并读回 slave role（`ROLE=0`）、任意 peer 配对（`CMODE=1`）、设备名、PIN 和透明 UART 设置，再发 `AT+RESET`，拉低 KEY，进入透明 slave 等待模式。RESET 命令离开 USART3 后立即拉低 KEY，让模块在重启期间采样到数据模式；当 vendor 专用命令无法读回时也会发 reset，防止已检测到的模块卡在不可发现的 AT 模式。

接口只有真实模块在电气上驱动 PB11 或回复 AT 命令后才显示 ready；`WAITING` 仍需要验证过的 slave 配置和成功的 reset。这区分了“模块存在但 AT 方言未知”和“模块缺失”，而不会声称未验证的配置。默认名 `KasaneTeto`，PIN `2233`，UUID `0x1101`，波特率 `38400`，可通过上述构建变量修改。

## 交互式串口命令

USART1 在 115200 8N1 提供交互式 bring-up 提示：

```text
uart
perf
sd retry
bt
bt probe
bt at AT
bt at AT+ROLE?
bt at AT+NAME?
bt at AT+PSWD?
bt at AT+UART?
```

每次自动或手动 AT 交换都会打印 USART3 波特率、KEY 模式、转义命令、转义原始回复、字节数和硬件错误数。`reply=<none>` 表示 PB11 没有 UART 字节；反复出现带错误的 `\xNN` 数据说明波特、接线或信号质量有问题；可读的 `ERROR` 表示模块存在但不支持该 AT 方言。`uart` 命令打印两个端口的实测外设时钟、请求和实际波特、BRR、接收中断状态和累计硬件错误。蓝牙初始化后，USART3 应报告 `initialized=1`、`requested=38400`、`rx-irq=1`；非零错误计数直接说明 PB11 在翻转，但接收到的帧格式错误。

SysTick 从实时 RCC HCLK 推导 1 kHz reload，而不是假设复位默认的 8 MHz。`perf` 命令报告 HCLK/PCLK 值和实测外设服务循环、光感采样、手动蓝牙重试、SD 卡检查的最大耗时。可选 HC-05 和缺失 TF 卡的初始化只在显式 UART 命令时重试，避免命令超时周期性地冻结输入和显示更新。

## 调度器与任务

MCU profile 现在链接通用 A20OS 进程调度器。ARMv7-M `__switch` 保存并恢复 `r4-r11`、SP、返回地址、任务指针和 CONTROL。物理板上把外设服务跑成一个内核任务；QEMU 用两个调度器探测任务替代。任务设计为通过 `proc_yield()` 协作让出，同时也会在 10 ms 时间片内被抢占。SysTick 挂起最低优先级 PendSV 异常；PendSV 把异常返回重定向到 thread-mode trampoline，保存易失寄存器集并在 handler mode 外安全进入现有调度器；SVC 在任务恢复时还原中断的 PC/xPSR。每个任务有 preemption guard，落在调度器内部时会把 tick 推迟。MCU-only 批处理限制、256 项 PID 命名空间和兼容桩减少了通用调度器占用。8 KiB QEMU profile 跑两个不主动让出的忙循环任务，并报告 `PREEMPT PASS`，证明两个任务只靠时间片推进。

## 显示与仪表板

当 workable panel 安装好时，board bring-up 仪表板仍可通过触摸选择。存储页显示容量、FAT32 状态、SDIO 总线宽度和卷标。触摸页是一个小画板，支持连续笔画和 `CLEAR` 按钮，无需串口终端即可检查面板方向和校准。TF 和 touch 的状态行也兼任页面快捷键。

## 烧写后验证

STM32CubeProgrammer 也可以把二进制写到地址 `0x08000000`，或直接写 ELF。复位后 USART1 在 PA9 115200 8N1 打印输出；接收是 PA10。正常 flash 启动时保持 BOOT0 低电平。

## QEMU smoke test

```sh
make run-stm32f103-qemu
```

QEMU 的 `stm32vldiscovery` 模型有 128 KiB flash 和 8 KiB SRAM，所以该目标使用与物理 STM32F103C8 镜像不同的构建目录和内存布局。

## 硬件 smoke test

烧写后预期行为：

1. LED0 上电后亮起，然后每 500 ms 翻转一次。
2. USART1 打印启动状态和外设事件。原来的每秒一次 `[TICK]` 行已关闭，保持控制台安静。
3. LCD 显示外部 SRAM、TF/FAT32、方向键和 uptime 状态。
4. 黄色方向键发出 `[KEY]` 消息。左右切换页面，上下控制当前页面。
5. 如果触摸可用，底部 `STATUS`、`MEM`、`TF`、`BT`、`INPUT` 按钮切换页面；画图留下青色笔画，`CLEAR` 擦除。
6. 触摸面板显示坐标并发出 `[TOUCH] down`/`[TOUCH] up` 消息。
7. `BT` 页报告 HC-05 连接状态。手机在 38400 8N1 配对，从蓝牙串口应用发送文本，按上键发送 `A20OS HC05 TEST` 回复。
8. 遮住和移开板载光敏电阻。`LIGHT` ADC/level 值会变化，面板背光保持固定亮。
9. 插入或移除 TF 卡分别在不重启的情况下在 5 秒或 2 秒内被检测到。

用 FAT32 格式化的 TF 卡检查文件系统元数据。原始或非 FAT32 卡仍会被初始化成块设备。

> ⚠️ 注意：不要在装有唯一数据的 TF 卡上做写测试。破坏性块测试先用可丢弃的镜像。

## 驱动开发提示

- 复现 bring-up：执行 `make stm32f103-bringup` 或 `make stm32f103-xuanwu`，然后烧写并观察 USART1 输出。
- 扩展 STM32F1 驱动层：把 GPIO、EXTI、SPI、I2C、SPI-flash 控制器接口做成可复用驱动，不要在可复用驱动里硬编码 Xuanwu 引脚。
- 新板级移植：提供 `kernel/platform/<board>/` 下的链接脚本、`BOARD_INCLUDE_DIR` 和 `BOARD_DRIVER_DIR`；板级常量留在 platform 代码。
- 调试外设时先用 `uart` 和 `perf` 命令确认时钟和错误计数，再怀疑驱动逻辑。

## 下一步

1. 在物理 Xuanwu 板上，并发 LCD、SDIO、UART 中断负载下验证 10 ms SysTick/PendSV 抢占路径。
2. 决定应用是跑在特权 flat 地址空间，还是使用 Cortex-M MPU 做粗粒度隔离。
3. 把 MCU 兼容桩替换为必需的 IPC 和小型 VFS 实现，然后挂载现有 TF `block_dev_t`。
4. 在 STM32F1 驱动层扩展可复用的 GPIO、EXTI、SPI、I2C 和 SPI-flash 控制器接口。
5. 把 SDIO 传输移到 DMA，并用外部 SRAM 做块缓存。

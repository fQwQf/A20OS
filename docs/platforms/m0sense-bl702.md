# Sipeed M0Sense / BL702

当前提供 **M-mode SRAM 探针**，不提供完整 A20OS 内核或用户态。实机运行状态：未验证。
现在有两条部署路径：**原厂 UF2 bootloader 的 USB-C 应用上传**，或外置 JTAG 的非持久 SRAM 加载。
它尚不能运行完整 A20OS 内核；拿到带 UF2 固件的板子，只需 USB 数据线即可上传并观察 LED。

## 准备物品

| 目标 | 所需物品 |
|---|---|
| USB 上传、LED 初验 | M0Sense、支持数据的 USB-C 线、带原厂 UF2 bootloader 的板卡 |
| 完整启动日志、UART 收发校验 | 另外准备 3.3 V USB-UART、3 根线、已焊接或可靠接触的 GPIO14/15/GND |
| 没有 UF2 卷时恢复原厂 loader | USB 数据线、短接硬件 BOOT 焊盘与 3.3 V 的导线、官方 DevCube |
| SRAM 调试 | 外置 JTAG 调试器与对应线材；不是普通 USB-C 线 |

Linux 主机需要 Python 3.11+、GNU make、`riscv64-unknown-elf-gcc` 和同前缀 objcopy。Debian/Ubuntu 安装：

```sh
sudo apt-get install build-essential git python3 python3-venv \
  gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf
```

探针由 C/汇编构建，不下载或链接厂商 SDK，也不编译 Rust 内核。仅使用外置 JTAG 路径时还需支持 BL702 RISC-V 调试的 OpenOCD；本页后文给出配置与加载命令。下列命令均在 A20OS 源码根目录执行，首次使用先运行 `git submodule update --init --recursive`。

## 一、构建与核对产物

```sh
tools/a20 check instances/m0sense-bl702-probe.toml
tools/a20 package m0sense-bl702-probe
# 等价底层目标：make m0sense-probe
```

输出 `build/m0sense-bl702/handoff/`：

| 文件 | 用途 |
|---|---|
| `probe.uf2`、`probe.json` | **原厂 M0SENSE UF2 loader 的应用上传文件**、格式/来源/哈希记录 |
| `probe-xip.elf`、`probe-app.bin` | 应用入口 `0x23000000`；先复制 text/data 到 SRAM，再进入探针 |
| `probe.elf`、`probe.bin`、`probe.map` | SRAM 调试产物；`probe.bin` 仅代码段，不可直接烧录 |
| `deploy-uf2.py` | 无外部 Python 依赖的 USB 应用上传脚本 |
| `openocd.cfg`、`load-probe.sh` | 外置 JTAG 加载 SRAM 的替代方案 |
| `README.md` | 随包提供的操作与验收说明 |

`probe-app.bin` 是**没有 ROM bootheader 的应用**。这与 Sipeed 官方示例及其 `uf2_conv.c` 相同；
UF2 地址从 0 递增，含义是原厂 loader 的**应用相对地址**，不是要求把应用写到整片 Flash 的 0 地址。
不要给通用 UF2 转换器指定绝对地址，也不要把这些 BIN 当作整片 Flash 镜像。

本仓库的打包器检查 ELF32/RISC-V/软浮点 ABI、入口、PT_LOAD 范围、完整 BIN 内容，并输出
`family=0x11de784a`、`flags=0x2001`、256 字节 payload 的 UF2。UF2 的格式定义见下方官方转换器与 family/flag 参考。

## 二、用 USB-C 上传

1. 接上 USB **数据**线。按住板上的软件 **BOOT 按钮**，按一下 RESET，等 `M0SENSE` 移动卷出现后放开 BOOT。
   普通上电没有 U 盘不一定是故障；需要这个按键组合。
2. 查看卷根目录的 `INFO_UF2.TXT`，应包含：

   ```text
   UF2 Bootloader v1.0
   Model: Sipeed M0SENSE BL702
   Board-ID: m0sense-bl702
   ```

3. 先预览上传计划：

   ```sh
   tools/a20 deploy m0sense-bl702-probe --no-build --no-console --dry-run
   ```

4. 上传：

   ```sh
   tools/a20 deploy m0sense-bl702-probe --no-console
   # 多块板或需要指定卷时：
   tools/a20 deploy m0sense-bl702-probe --no-console --uf2-mount /media/<用户名>/M0SENSE
   ```

   脚本在 Linux 自动发现已挂载、可写的 FAT 卷，核对 `INFO_UF2.TXT` 的 Model 与 Board-ID，核对
   UF2 格式及 `probe.json` 的 SHA256，然后只复制 `A20PROBE.UF2`。它不打开原始块设备，不擦除
   bootloader，也不调用原始 Flash 烧录器。**当前应用会被这个探针替换**。
5. 原厂 loader 通常自动卸载卷并重启。探针通过初始化数据、BSS 和小数组 RAM 校验后，关闭红/绿 LED，
   进入**蓝 LED 周期闪烁**。这是仅有 USB-C 时的初步检查；上传进度完成不等于已经启动成功。

Linux 若未自动挂载，先在桌面文件管理器打开 M0SENSE，再指定实际挂载目录。Windows 可以直接把
`probe.uf2` 拖到已确认身份的 M0SENSE 卷；当前 Python 自动卷发现主要支持 Linux/macOS。
已打包后可加 `--no-build` 使用现有产物。随包独立入口
`python3 build/m0sense-bl702/handoff/deploy-uf2.py` 使用同样的身份与哈希检查，支持 `--mount` 和 `--dry-run`。

脚本若在最后一块后遇到设备自动断开、fsync 错误，会报告失败/不确定，不把它记录为 PASS。
先观察 LED 和日志，再重新进入 bootloader 检查；不要因为复制异常自动执行全片擦除。
若目录中已有 `A20PROBE.UF2`，脚本也不覆盖已有文件；重新进入原厂卷后再尝试。

## 三、串口验收（推荐）

连接外部 **3.3 V UART**：GPIO14（TX）→适配器 RX，GPIO15（RX）→适配器 TX，共地。
波特率 115200 8N1。USB-C 只用于上传，本探针没有实现 USB CDC，因此不会提供原厂固件的 USB 虚拟串口。

在上传前开始监听，以免错过一次性的启动信息：

```sh
tools/a20 console m0sense-bl702-probe
```

使用 `tools/a20 deploy m0sense-bl702-probe --serial /dev/ttyUSB0` 可把上传和串口验收串起来：
工具会先打开外置 UART，再复制 UF2，复用同一连接读取自动重启后的标记。USB-only 场景使用上一节的
`--no-console`。不要让另一个串口终端同时占用适配器。

预期：

```text
A20 BL702 M-mode SRAM probe
MISA=0x........ HART=0x........
A20 BL702 PROBE READY
```

输入 `i` 输出 CPU 信息，`m` 执行小数组 RAM 校验并返回 `RAM PASS`，其他字符回显。
异常输出 `A20 BL702 TRAP` 并停止；调试器也可读 `probe_stage`：1=UART 前，2=通过 RAM 校验，
`0xdead....`=异常。蓝灯是轮询循环产生的心跳，不代表已经实现中断调度或精确定时器。

## 四、没有 M0SENSE 卷：恢复原厂 UF2 loader

Sipeed 明确说明部分出厂固件没有 U 盘功能。软件 BOOT 按钮连接 GPIO2，依赖原厂 loader；
**板上丝印 BOOT 的硬件 strap 焊盘与这个按钮不同**。硬件 ROM 下载模式仍可用于恢复。

先准备官方文件，本脚本只下载并验证，不连接硬件、不烧录：

```sh
tools/a20 prepare m0sense-bl702-probe --dry-run
tools/a20 prepare m0sense-bl702-probe
# 独立脚本：python3 tools/m0sense_recovery_assets.py
# 等价：make m0sense-recovery-assets
```

文件在 `build/m0sense-bl702/recovery/`，固定来自 Sipeed 仓库提交
`97156083b1ee3a94b7808bfaafd8a28ec78ed466`，脚本验证：

| 文件 | SHA256 |
|---|---|
| `m0sense_bootloader_v0.9.9.bin` | `ce92ce6c7ef9c36fe4f95853dc9f2b68c691dc25633a8d9d911f5ce8bf53604b` |
| `blink_baremetal.uf2` | `65a56b13cdd0775a11078a9ae5210cbd6afcbc4e5b6e8d40eb5eb4642ce3cb60` |

按官方 ROM 恢复步骤操作：

1. 保留自己原有应用和固件备份；如果仍能进 UF2 卷，保存可读的说明/导出文件。不要把应用级 UF2 导出
   当作整片 Flash 的完整备份。本节会更换原有固件。
2. 从 [Bouffalo 官方下载页](https://dev.bouffalolab.com/download) 获取 DevCube，在 Linux 用
   `BLDevCube-ubuntu`。按照 [Sipeed 带图的 BOOT 引脚说明](https://wiki.sipeed.com/hardware/en/maixzero/sense/start.html#Burn-bin-file)
   核对板版本与 3.3 V/BOOT 焊盘。
3. 断电时将 **硬件 BOOT strap 焊盘**接到 **3.3 V**，再用 USB-C 上电。应枚举 ROM 下载串口。
   不要把丝印 BOOT 与 GPIO2 软件按钮混用，也不要短接到 5 V。
4. DevCube 选择 **BL702**、**MCU** 模式，Firmware BIN 选择下载并校验过的
   `m0sense_bootloader_v0.9.9.bin`。Refresh 后选新出现的 ROM 串口，UART rate 选 **2000000**。
5. 使用 **Create & Download**，由官方工具按芯片/Flash 配置生成 bootheader 并烧录。
   **这里不能选择 A20 的 `probe.bin` 或 `probe-app.bin`**，它们不是 ROM 恢复镜像。
6. 成功后断电，移除硬件 BOOT 短接，再上电。按软件 BOOT+RESET，应出现 M0SENSE 卷。
7. 可先拖入官方 `blink_baremetal.uf2` 验证原厂路径，再重复第二节上传 A20 探针。

恢复保留为显式的官方工具操作；本仓库不根据猜测的分区偏移自动擦写原厂 loader。

## 五、可选 JTAG SRAM 路径

需要外置 JTAG 调试器、对应 OpenOCD interface 配置，以及已核对的板级 JTAG 引脚与 pinmux：

```sh
build/m0sense-bl702/handoff/load-probe.sh /absolute/path/to/interface.cfg
```

脚本 halt、load ELF、verify、resume，不擦 Flash。BL702 target ID 来自官方 SDK；不要同时把
GPIO14/15 作为 JTAG 和 UART。USB-C 本身不是 JTAG 调试器。

## 六、系统支持范围

BL702 是 MCU，不能套用 `qemu-riscv32-nommu`。SDK 为 E24 选择 RV32IMAF(C)/ILP32F；现有 QEMU
RV32 内核使用 D 扩展、S-mode CSR、SBI 和 PLIC。探针用更小的 **RV32IMC+Zicsr+Zifencei、ILP32**，
不使用 A/F/D、SBI、S-mode 或 PLIC。

两种镜像最终执行相同的 SRAM 探针：代码窗口 `0x22010000..0x22020000`（64 KiB），数据
`0x42020000..0x42024000`（16 KiB，含 4 KiB 栈）。XIP 入口只负责把 text/data 复制进去，之后才切换
RC32M 和 UART 时钟，避免从 Flash 执行时重配 Flash 依赖的时钟。链接器检查栈与数据不重叠。
Sipeed 标称 132 KiB RAM 不意味着一整段 DDR 可随意分配；本探针只使用 SDK 已描述的子集。

当前缺少 M-mode 上下文切换、E24 CLIC/机器定时器、RAM 受限的完整内核配置与进程模型。
本探针始终屏蔽中断，不宣称调度、存储、BLE、USB CDC 或完整用户态已经可用。

## 参考资料

- [Sipeed 规格](https://wiki.sipeed.com/hardware/en/maixzero/sense/maix_zero_sense.html)
- [Sipeed UF2 按键、出厂固件差异与 ROM 恢复图解](https://wiki.sipeed.com/hardware/en/maixzero/sense/start.html)
- [固定版本官方构建脚本](https://github.com/sipeed/M0sense_BL702_example/blob/97156083b1ee3a94b7808bfaafd8a28ec78ed466/build.sh)
- [官方 UF2 转换代码](https://github.com/sipeed/M0sense_BL702_example/blob/97156083b1ee3a94b7808bfaafd8a28ec78ed466/misc/utils/uf2_conv.c)
  与 [family/flag 定义](https://github.com/sipeed/M0sense_BL702_example/blob/97156083b1ee3a94b7808bfaafd8a28ec78ed466/libs/uf2_format/uf2_format.h)
- [官方示例所固定 SDK 的 XIP 链接图](https://github.com/bouffalolab/bouffalo_sdk/blob/4b4e4c95449e902206e0d92025c6dc05f3e355d5/drivers/bl702_driver/bl702_flash.ld)
- [当前 SDK 的 RAM 链接图](https://github.com/bouffalolab/bouffalo_sdk/blob/1cd1fd514df7ed0dfc1221be871f5aae03f31bbc/bsp/board/bl702dk/bl702_ram.ld)
- [M0Sense RGB 引脚](https://github.com/sipeed/M0sense_BL702_example/blob/97156083b1ee3a94b7808bfaafd8a28ec78ed466/m0sense_apps/blink/blink_baremetal/io_def.h)

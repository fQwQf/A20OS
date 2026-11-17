# 平台与硬件支持文档索引

本目录汇集 A20OS 支持的真实硬件开发板、整机设备与虚拟机环境的移植与操作参考。每篇独立说明适用硬件、当前设备支持范围、固件依赖、构建产物、启动部署与排障验证。

---

## 移植与通用指南

| 文档 | 说明 |
|---|---|
| [porting-guide.md](porting-guide.md) | **板级移植指南**：架构抽象层、Board 接口、串口与时钟接入 |
| [physical-boards.md](physical-boards.md) | **物理开发板总览**：多架构开发板烧录、部署与外设验证原则 |
| [not-yet-ported.md](not-yet-ported.md) | 待移植与计划中板卡边界说明 |

---

## 硬件与开发板列表

### 1. RISC-V 架构平台

| 平台文档 | SoC / 芯片 | 架构 / Profile | 说明 |
|---|---|---|---|
| [visionfive2-boot.md](visionfive2-boot.md) | StarFive JH7110 | RV64 (MMU) | 昉·星光 2 (VisionFive 2) 硬件引导与外设接入 |
| [licheerv-nano.md](licheerv-nano.md) | Sophgo SG2002 | RV64 (MMU) | 矽速 LicheeRV Nano 算能 RISC-V 核心板 |
| [milk-v-duo.md](milk-v-duo.md) | Sophgo CV1800B | RV64 (MMU) | Milk-V Duo 开发板与轻量级运行时 |
| [ox64.md](ox64.md) | Bouffalo BL808 | RV64 (MMU/NOMMU) | Pine64 Ox64 双核 RISC-V 开发板 |
| [m0sense-bl702.md](m0sense-bl702.md) | Bouffalo BL702 | RV32 (MCU) | M0sense RISC-V 极简开发板与探针 |
| [esp32-c3.md](esp32-c3.md) | ESP32-C3 | RV32 (MCU) | 乐鑫 ESP32-C3 裸机与轻量内核 |
| [d1s-f133.md](d1s-f133.md) | Allwinner D1s / F133 | RV64 (XuanTie C906) | 全志 D1s 哪吒开发板支持状态 |
| [k230.md](k230.md) | Kendryte K230 | RV64 (Dual Core) | 嘉楠堪智 K230 勘智开发板 |

### 2. ARM / AArch64 架构平台

| 平台文档 | SoC / 芯片 | 架构 / Profile | 说明 |
|---|---|---|---|
| [raspberrypi4.md](raspberrypi4.md) | Broadcom BCM2711 | AArch64 (ARMv8-A) | 树莓派 4B (Raspberry Pi 4) 原生端口与 SMP 支持 |
| [pine-a64.md](pine-a64.md) | Allwinner A64 | AArch64 (ARMv8-A) | Pine A64(+) 单板计算机 |
| [quartz64-zero.md](quartz64-zero.md) | Rockchip RK3566 | AArch64 (ARMv8-A) | Quartz64 Zero / Pine64 开发板 |
| [rk3328.md](rk3328.md) | Rockchip RK3328 | AArch64 (ARMv8-A) | 瑞芯微 RK3328 平台接入 |
| [sun50i-h616.md](sun50i-h616.md) | Allwinner H616 | AArch64 (ARMv8-A) | 全志 H616 电视盒子/单板计算机 |
| [lichee-pi-zero.md](lichee-pi-zero.md) | Allwinner V3s | ARM32 (ARMv7-A) | 荔枝派 Zero ARM32 紧凑型开发板 |
| [stm32f103-port.md](stm32f103-port.md) | STM32F103 | ARMv7-M (MCU) | STM32 微控制器嵌入式内核与集群叶子 |
| [hi3861.md](hi3861.md) | HiSilicon Hi3861 | RISC-V / LiteOS | 华为海思 Hi3861 WLAN IoT 模块接入 |

### 3. LoongArch 龙芯架构平台

| 平台文档 | 处理器 | 架构 / 位宽 | 说明 |
|---|---|---|---|
| [2k0300.md](2k0300.md) | 龙芯 LS2K0300 | LoongArch 64 | 龙芯蜂鸟 LS2K0300 单板计算机接入 |
| [2k1000.md](2k1000.md) | 龙芯 LS2K1000 | LoongArch 64 | 龙芯 2K1000 双核板卡移植参考 |

### 4. x86_64 PC 与工控设备

| 平台文档 | 设备形态 | 特性与用途 |
|---|---|---|
| [x86_64-pc.md](x86_64-pc.md) | 通用 PC / 服务器 | UEFI GPT 引导、多核 SMP、AHCI/NVMe 与标准串口 |
| [x86_64-laptop.md](x86_64-laptop.md) | 无 COM1 的 UEFI 笔记本 / 迷你主机 | 默认启动 XFCE Wayland 桌面（GOP/simplefb 软件渲染）、USB 承载 distro；内部键盘真机是 USB HID ITE 8910，尚未通过 |
| [x86_64-laptop-handoff.md](x86_64-laptop-handoff.md) | 真机交接与证据 | A20LOG 已验证；xHCI Address Device -17 阻塞键盘/USB/GUI 的完整记录与下一步 |
| [huawei-ct5100.md](huawei-ct5100.md) | 华为 CT5100 瘦客户机 | x86_64 物理整机实测、UEFI 双盘与 GUI 桌面验证 |
| [j1900.md](j1900.md) | Intel Celeron J1900 | 工控主板、多网卡与无显示器串口控制台 |

### 5. 虚拟机环境 (VirtualBox)

| 平台文档 | 架构 | 说明 |
|---|---|---|
| [virtualbox.md](virtualbox.md) | 通用 | VirtualBox 虚拟机运行环境总览与配置 |
| [virtualbox-x86_64.md](virtualbox-x86_64.md) | x86_64 | x86_64 架构 VirtualBox 镜像制作与网络调试 |
| [virtualbox-aarch64.md](virtualbox-aarch64.md) | AArch64 | AArch64 架构 VirtualBox 虚拟化测试 |

---

## 快速查询与运行

```sh
tools/a20 boards                # 查询所有已知板级定义
tools/a20 list                  # 查询所有声明式实例
tools/a20 show <实例名>         # 查看指定实例配置
tools/a20 run <实例名>          # 启动仿真或部署运行
```

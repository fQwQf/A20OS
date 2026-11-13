# 在另一台 x86 电脑复用实机驱动

本指南对应 2026-11-13 的 PC 驱动整理。Lenovo 83EG 是验收样机，不是驱动选择条件。
先读[核心模型](core-model.md)、[运行时契约](runtime-contracts.md)和[提交规范](../meta/testing-and-submission.md)。
上游代码成熟程度不能替代 A20OS 的资源所有权、类接口、失败回滚和异步退出契约。

## 组件与边界

| 组件 | 源码位置 | 身份及复用边界 |
|---|---|---|
| PCI/USB 发现 | `kernel/drivers/bus/`、`kernel/drivers/usb/core/` | 从固件和总线枚举资源；设备驱动不重新扫描机器 |
| xHCI、USB HID/Hub/Storage | `kernel/drivers/usb/host/`、`class/` | 同一控制器协议、接口描述符与设备类复用；ITE `048d:c102` 的键盘报告差异按设备 ID 处理 |
| NVMe | `kernel/drivers/block/nvme.c` | PCI class/设备资源、DMA 与 block class；embedded 与模块包装共享同一实现 |
| PS/2 i8042 | `kernel/drivers/input/ps2.c` | 单 ISA 控制器；端口操作走 hwapi，IRQ 路由经固件接口交给架构，重复 probe 明确拒绝 |
| I²C 传输服务 | `kernel/drivers/i2c/i2c-core.c` | 全架构共享注册表；查询获得引用，`i2c_adapter_put()` 配对；每次传输也持有引用 |
| DesignWare I²C | `kernel/drivers/i2c/i2c-designware.c` | ACPI `AMDI0010/AMDI0011`、DT compatible；MMIO 和控制器归属来自固件 |
| HID-over-I²C | `kernel/drivers/hid/i2c-hid.c` | `PNP0C50`/明确 HID；消费父控制器和 `_CRS`、`_DSM`，输出统一 input class |
| Synaptics CE78 差异 | `kernel/drivers/hid/i2c-hid-synaptics.h` | 驱动私有实现，由描述符 VID/PID 和报告布局选择；不按电脑型号选择 |
| 固件 GPIO 输入适配 | `kernel/drivers/gpio/firmware_input.c` | 目前只理解 `AMDI0030` 的只读 PinStatus；其他控制器返回不支持，HID 可按无 attention 的方式轮询 |
| 固件 framebuffer 控制台 | `kernel/drivers/char/fbcon.c` | 渲染通用字符网格；映射、缓存属性在 x86 架构层，DRM 负责显示交接 |
| R8169 | `kernel/drivers/net/r8169.c` | 每设备状态和 DMA ring；模块自动选择目前限定 `10ec:8168`，其他静态注册 ID 不等于已验证硅片兼容 |
| RTW8852BE 传输端 | `kernel/drivers/net/rtw89_broker.c` | PCI 资源、用户 MM 归属、DMA 撤销和 Ethernet class；当前明确只允许一个无线设备/服务所有者 |
| 上游无线协议服务 | `user/wifi/` | 独立 Linux-ABI 进程；通过公开 `uapi/a20/wifi.h` 操作受控传输端，不把 Linux/macOS 内核环境搬进 A20OS 内核 |
| 桌面配置/诊断 | `packages/overlay/xfce/`、`user/diagnostics/` | OpenRC、IBus、libinput、会话日志和输入诊断；不属于无线协议或硬件驱动 |
| 存储压力工具 | `user/cmds/stress/nvme_stress.c` | 显式选择测试设备；不在普通 probe 中破坏性写盘 |

## 同硬件新机器的接入顺序

1. 在宿主系统保存 PCI vendor/device/revision/subsystem、ACPI HID/CID、`_CRS`、`_DSM` 和描述符。
   相同营销名称或同厂商不能证明寄存器协议相同。不要在通用驱动增加机器名分支。
2. 确认 provider 已创建设备：PCI BAR/IRQ、I²C ResourceSource 指向的控制器、GPIO provider
   及其有效资源范围。固件适配放在 `kernel/firmware/`，页表/缓存/端口指令放在 `arch/`。
3. 先跑现有精确匹配。新增变体使用 `firmware_ids`、`id_table` 或驱动私有 quirk；记录资料和证据。
   没有有效 7-bit 地址时 I²C-HID 拒绝 probe，不通过试探整条总线猜地址。
4. 用同一通用驱动建立类接口，先确认基本 I/O，再启动原版 libevdev/libinput 或网络消费者。
5. 新机器的通过记录单独保存；本机 QEMU 或 Lenovo 的通过不能转写为另一台机器已通过。

### I²C 时序和生命周期

DesignWare 优先读取控制器节点的 `clock-frequency`、`i2c-bus-frequency` 属性。
保留的 `a20.i2c.ic_clk=`、`a20.i2c.speed=` 是调试覆盖/缺省策略，不是所有 AMD 控制器的硬件事实；
当前缺省 100 MHz/400 kHz，复用时必须确认输入时钟和目标从机频率。
HID 地址及描述符寄存器优先来自 `_CRS`/`_DSM`；`a20.i2chid.addr=`、`a20.i2chid.poll_ms=`
仅用于有依据的诊断，不应成为另一台机器的永久固件替代品。

`i2c_adapter_get*()` 成功后必须 put。传输约定是 **全部消息成功返回0，失败返回负 errno**。
注册、引用和移除在注册表锁内完成，控制器的 transfer 在锁外执行；remove 遇到引用返回 `-EBUSY`，
控制器不得继续释放私有状态。adapter 编号不是硬件身份，客户端通过控制器设备寻找传输端。

每个 HID 设备拥有独立状态、工作队列和轮询 work。早期 probe 返回 `-EPROBE_DEFER`，
调度器准备后重试。remove 顺序是停止生产者、在 `io_lock` 外等待队列退出、归还 work/adapter，
最后释放缓冲和实例。等待超时保留所有上下文供重试；不能先 free 再等待回调。
GPIO 输入适配不写寄存器、不改方向或 IRQ，也不是完整的 GPIO 控制器驱动。其只读映射与不可变
固件节点同为启动期生命周期；需要可卸载 GPIO provider 或中断输入时，应实现相应资源/IRQ provider。

### 部署与上游复用

- PC 实机 profile 使用 `DRIVER_DEPLOYMENT=embedded`，但仍使用统一 `device_t/driver_t` 和类接口。
  DesignWare/I²C-HID 目前只在 embedded 账本中；尚未提供 ACPI 身份驱动包自动选择，不能声称 generic 已支持。
- NVMe、PS/2、R8169 的 `kernel/drvmod/examples/` 文件只保留描述符和入口，
  实现位于 `kernel/drivers/<class>/`；模块和 embedded 不维护两份协议代码。
  PS/2 退出先停止/排空 IRQ 再关闭控制器，模块导入需通过实际编译与导出表检查。
- 已有模块包装继续通过 `components/drivers.toml`、生成的 `components/drivers.mk` 和
  `tools/mk/driver-modules.mk` 发布。新设备实现不得靠目录通配偷偷进入内核。
- `WIFI_RTW89=1` 当前限定 x86_64、embedded、RAMFS_USER；`rtw89` submodule 固定为
  `90b6cfc5a69c271688fad188857cfaeb58053dc5`，构建脚本检查版本、干净状态和固件哈希。
  上游源码、兼容运行时和内核传输端有独立许可边界；许可证与 notices 随服务发布。
- 使用原版 OpenRC 监督服务，Wi-Fi GUI 和网页认证属于用户态；扫描按信号排列，不给某个校园 SSID 特权。
- `RAMFS_DESKTOP_TOOLS=1` 独立打包会话/输入诊断与桌面配置，默认跟随 `RAMFS_DISTRO`。
  它不要求 Wi-Fi；`WIFI_RTW89=0 RAMFS_DISTRO=1` 也应能启动桌面和执行正常电源流程。

## 验证和提交

```sh
make check-pc-drivers
make check-component-registry
make check-firmware-model
make ARCH=x86_64 BOARD=x86_64-pc ABI=linux DRIVER_DEPLOYMENT=embedded \
  RAMFS_USER=1 RAMFS_DISTRO=1 WIFI_RTW89=1 NR_CPUS=16 \
  ALLOW_UNVERIFIED_SMP=1 CONFIG_UBSAN=0 NET_STACK=bsd x86_64-uefi-loader -j8
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux \
  DRIVER_DEPLOYMENT=embedded CONFIG_UBSAN=0 kernel-only -j8
```

构建工具链与外部源码按[依赖说明](../../dev/external-dependencies.md)准备；BSD backend需要可用的Clang。
若宿主没有默认 `clang`，通过 `BSD_CLANG=/实际路径/clang` 指定；本轮使用本地Clang 19工具链。

PC host 门禁同时编译 NVMe/PS/2/R8169 模块并检查导出依赖，覆盖注册表引用、传输中移除、工作队列退出失败重试、GPIO ID/边界/极性、USB
完成事件所有权、HID 报告及就绪通知、MT、无线 DMA/回调、GUI/门户与会话电源。
I²C 协议、FIFO 和报告解析还有使用生产路径的启动自检。
AArch64 编译只能证明共享代码的编译层级，不代表 AMD/Synaptics 硬件在 ARM 已运行。

本轮独立源码树通过44项PC测试、371项固件/驱动测试、sleep ABI及112实例的manifest检查，
x86 EFI与AArch64内核构建通过，RISC-V生命周期和平台IRQ回退门禁通过。16 CPU/8 GiB测试中，USB桌面图形关机与未启用无线的
NVMe桌面普通重启均进入ACPI，退出后fsck=0、clean、RECOVER=0、last_orphan=0；
独立源码构建的USB桌面还执行了真实rtw89服务自检。

原第18轮实机验收仍对应整理前的 EFI：1320条连续日志、clean，触控板6566个有效报告，
错误/恢复/丢帧为0。最终同步仍约105 s，ordered 阶段约88 s，保留为性能限制。
本轮工作队列与打包拆分属于新改动，实机效果需下一轮确认；已验收 U盘保持原样。
USB HID 的既有热拔插完成回调退出窗口仍未关闭，不能据此宣称已支持任意热拔插；
R8169 的有线设备和其他新机器仍需各自实机验收。
证据与精确构建、镜像、测试结果见 `build/evidence/lenovo-83eg-2026-11-13/driver-consolidation/`。

# x86_64 笔记本 / 无 COM1 的 UEFI 整机

> **真机状态（汇总至 2026-11-14 第 19 轮，2026-11-16 更新范围）**已有启动、键盘/触控板和 WLAN 关联/DHCP 记录；仍有输入/应用阻塞及慢写回，完整认证未完成。新增设备能力与整机认证安排到后续版本，现有内存、写回及硬件资源安全问题仍按对应契约修复。详细结果见 [实机交接](x86_64-laptop-handoff.md)，发布范围见 [§2.4](../releases/v1.0.md#24-功能强化移出-10)。

本页有两个实例，共用 `ARCH=x86_64 BOARD=x86_64-pc`：

| 实例 | 启动结果 | 用途 |
| --- | --- | --- |
| `x86_64-laptop-uefi`（默认） | UEFI → XFCE Wayland 桌面 | 裸机日常使用；桌面用户态来自 Alpine `xfce` world |
| `x86_64-laptop-shell` | UEFI → GOP 文本 shell | 内核/驱动排障；不依赖桌面镜像 |

两者都不需要安装到内置硬盘：启动介质就是一块可丢弃的 U 盘。默认实例的 U 盘同时承载 ESP、ext4 桌面 rootfs 和独立的 `"A20OS log"` FAT32 分区； shell 实例也有 ESP 和日志分区，只是没有 rootfs。

## 启动链

```text
固件 (UEFI, Secure Boot 关闭)
  └─ ESP: EFI/BOOT/BOOTX64.EFI        A20OS UEFI loader
       ├─ 保存 ACPI RSDP 与当前 GOP 模式
       └─ 复制内核到 32 MiB，交接 Multiboot 信息块
            └─ A20OS kernel
                 ├─ fbcon: 早期启动日志画在 GOP 扫描区
                 ├─ xHCI + usb-storage: 重新枚举启动 U 盘
                 ├─ 找到 GPT 分区 "A20OS distro" (ext4)
                 ├─ 挂载到 /extra，chroot，执行 /sbin/init
                 ├─ "A20OS log" (FAT32) 挂到 /var/log/a20 并持续写 JSONL
                 └─ labwc 通过 /dev/dri/card0 接管扫描区
```

内置 NVMe 在默认实例中仍会被探测，但桌面 rootfs 保留在测试 U 盘。早期 NVMe INTx panic 已修复（见下文）；第 13 轮继续验证完整浏览器与关机负载。实测介质性能支持后续迁往 SSD，但当前无现成空闲分区，缩分区和实际安装需要另行安排。

## 平台能力

| 能力 | 实现 | 行为 |
| --- | --- | --- |
| 屏幕文本控制台 | `kernel/drivers/char/fbcon.c` | 内核/tty 输出渲染到 UEFI GOP 扫描区；首个 DRM present 后自动让出扫描区，panic 时收回并显示 |
| 无 COM1 启动 | `x86_64/platform/console.c` | 16550 scratch/loopback 探测；无串口时跳过初始化、IRQ 与轮询 |
| 键盘/鼠标 | `drvmod/examples/ps2.c` + USB HID | i8042 → tty 与 `/dev/event0`；GSI1/GSI12 优先路由到 IOAPIC，缺少 PS/2 鼠标不影响键盘。Lenovo 83EG 的内置 ITE 8910 USB HID 键盘已在文本 shell 完成实机验收。 |
| 内部磁盘 | `drvmod/examples/nvme.c`（embedded） | 发布块设备；早期 INTx panic 已修复，见下方验证范围 |
| USB 存储 | `drivers/usb/*`（embedded） | 启动 U 盘在 ExitBootServices 后重新枚举，承载 `A20OS distro` 分区 |
| 显示 | `simplefb` + `drivers/gpu/drm.c` | 保留的 GOP 扫描区作为 DRM card0，dumb buffer + flush；无 Radeon 硬件加速，桌面为软件渲染 |
| 高于 4 GiB 的 MMIO | `x86_64/platform/device_mmio.c` | 64 位 PCI BAR 按需建立 2 MiB 非缓存别名；GOP 扫描区高于 4 GiB 时同样可用 |

用户态有两个来源：内核 RAMFS（`init`、`sh`（BusyBox）与核心工具，作为回退）和`/extra` 上的 Alpine world。`user/init.c` 只在 `/extra/etc/a20-distro` 与`/extra/sbin/init` 都存在时 chroot；否则留在 RAM shell。

## 构建

### 桌面镜像（默认）

需要一块预先构建好的桌面 rootfs，或者可访问 Alpine 镜像的构建网络：

```sh
# 复用已有 4 GB 桌面 rootfs（推荐，不在每次构建时重跑 apk）
tools/a20 package x86_64-laptop-uefi -- PREBUILT_WORLD=1

# 或者从包仓库重建 world（慢，需要网络）
tools/a20 package x86_64-laptop-uefi
```

产物：

```text
build/x86_64-pc/a20os-laptop-uefi.img   # GPT: ESP + A20OS distro (ext4) + A20OS log (FAT32)
```

`PREBUILT_WORLD=1` 使用 `build/images/xfce-x86_64.img`。构建规则会校验它是 ext4、容量与镜像一致，然后按固定偏移写入第二个分区。

### 文本 shell 镜像（排障）

```sh
git submodule update --init user/external/apps/fastfetch
tools/a20 package x86_64-laptop-shell
# build/x86_64-pc/a20os-laptop-shell.img
```

进入 shell 后可直接运行 `fastfetch`。程序从固定版本的子模块源码编译，以静态 ELF 形式内嵌到 RAM 文件系统的 `/bin/fastfetch`。

## 写入 U 盘与启动

必须选择**整盘**设备，不能是分区；该步骤覆盖 U 盘现有内容：

```sh
lsblk -o NAME,SIZE,MODEL,SERIAL,MOUNTPOINTS
tools/a20 deploy x86_64-laptop-uefi \
  --media-device /dev/disk/by-id/实际USB整盘设备 --dry-run --no-console
tools/a20 deploy x86_64-laptop-uefi \
  --media-device /dev/disk/by-id/实际USB整盘设备 --no-console
```

关闭 Secure Boot，从固件的 USB 启动项进入。U 盘容量至少 5 GB（桌面镜像约 4.3 GB）。首次启动会解包/写入 ext4 的日志与桌面缓存，可能比后续启动慢。

## 预期结果

屏幕先显示 `A20OS: UEFI: loading kernel` 和内核横幅，随后文本控制台停止，出现 XFCE 桌面（壁纸、顶部面板、底部 dock）。可用键盘/触摸板操作；`poweroff` 或桌面的关机菜单走 ACPI S5。

串口（如果接了 USB-TTL）应出现：

```text
[DISTRO] disk 0 A20OS distro at /extra; embedded /bin retained
[INIT] Chrooted to /extra
[DRM] present handle=... 1280x800 ...
```

## 自动验证

```sh
make smoke-laptop-gui     # 默认桌面形态
make smoke-laptop-shell   # 文本 shell 形态
make smoke-laptop-esp-log # 无专用日志分区 + 强制断电后的内置 ESP 日志
```

`smoke-laptop-gui` 把镜像作为 USB 盘挂到 OVMF/q35（无 virtio-gpu），等待`[DISTRO]` 与 `[DRM] present`，轮询 `screendump` 直到出现足够大的多色桌面，并检查内核没有 panic。它覆盖：ESP 启动、xHCI 重新枚举、ext4 distro 挂载、 labwc 走 simplefb/DRM、软件渲染出真实桌面。`smoke-laptop-shell` 覆盖文本控制台、PS/2 输入、NVMe identify，以及 `nvstress` 的两页 PRP/完成中断回归。`check_laptop_gui.py --storage nvme` 另外验证整盘走内置 NVMe 的桌面。

这两个门禁都是**模拟器**结论：不证明具体笔记本的固件、USB 控制器、GOP 模式或触摸板可用。

## 已知问题

### UEFI 退出和 xHCI 根端口

旧版真机日志中的 `ADDRESS_DEVICE ... -17` 是 xHCI Parameter Error。 CSZ=1 时 Input Control、Slot 和 EP0 的偏移分别为 0、64、128。 PORTSC 的 PED 位写 1 会禁用端口，清 change bits 时对 PED 写 0。修订版在日志中区分完成码和等待超时，并在参数错误时输出输入上下文。

关闭 i8042、接入 USB 键盘的 OVMF 测试曾在 `ExitBootServices` 返回`EFI_INVALID_PARAMETER`。修订版在最终取内存映射之前完成日志写入，之后只处理内存映射和退出请求。该测试通过 `--usb-keyboard` 运行，用 USB 键盘的按键事件和 shell 回显验证输入链路。

本机的启动 U 盘位于 RTS5411 下游。修订版为 hub 下游设备生成 Route String，使用各自的 EP0 传输，并按 hub 端口状态检测设备速率。`check_laptop_uefi.py --usb-hub` 和 `check_laptop_gui.py --storage usb-hub`分别验证 QEMU hub 下游的键盘与桌面启动盘。

### NVMe 桌面 I/O panic（已修复）

早期版本把桌面镜像挂到 NVMe 上启动时，桌面初始化会 panic；同样的镜像走 USB 或 virtio-blk 正常。根因在 NVMe 驱动的 INTx 完成路径，不在调度器、分配器或通用块层：

- INTx 是**电平触发**：设备在 completion queue entry 被摘除前一直拉住中断线。原 handler 只唤醒等待者、不碰队列状态，因此无法消抖；它返回后中断线仍然有效。
- 内核在 IRQ 返回处有一个抢占点 `kernel_preempt_at_irq_return()`，它会**重新打开中断**再调用 `proc_yield()`。于是仍被拉住的电平线立刻再次进入中断入口，在同一个内核栈上嵌套下去；64 KiB 内核栈很快被压穿，把栈底部保存的 `task_context_t` 覆盖成返回地址。随后切换回该任务时`context_switch_locked()` 看到 `saved sp` 是内核代码地址（例如 `0xffff8000020d0342`）而 panic。
- 这也解释了复现特征：只有“设备中断打开”时才会坏。纯轮询（不注册 IRQ、 CQ 不使能中断）跑 1000 次 8 KiB 读完全正常；一旦让设备拉中断线， 1000～2000 次读后必现。
- 修复（`kernel/drvmod/examples/nvme.c`）：handler 里先`irq_disable()` 屏蔽该向量，唤醒等待者；提交者在摘除 completion 后`irq_enable()` 重新武装。屏蔽期间到达的 completion 会让电平线保持有效，重新武装时立即重新投递，因此不会丢唤醒；最长等待由 50 ms 的 park chunk 兜底。
- 分配器侧曾怀疑 `pfa_alloc(order=1)` 的内页 `refcount=0`。该行为是**有意**的（内页由 order 块首帧拥有，`pfa_free(order)` 统一回收），复现期间没有观察到任何对块内页的单独释放，因此不是本次根因；暂不改动。

验证（QEMU/OVMF，全部通过）：

| 场景 | 证据 |
| --- | --- |
| 驱动压力 | `nvstress /dev/disk0 4000 8192`、`2000 16384`、`8000 4096`（约 96 MiB）连续无 panic |
| 中断确实是活的 | 2000 次 8 KiB 读期间 handler 计数按“每命令一次”增长，不是风暴 |
| 文本门禁 | `smoke-laptop-shell`，其中新增 `nvstress 1200 8192` 回归步骤 |
| 内置盘桌面 | `check_laptop_gui.py --storage nvme`：`[DISTRO] disk 0`、`[SMP] 4/4`、`[DRM] present`，桌面截图 1.02M 亮点/512 色，无 panic |

默认产物仍把 distro 放在 USB（便于整盘替换与实机排障），但**内置 NVMe 的桌面路径现在是经过验证的**，不再是已知阻塞项。

### 其他未验证项

- 真实 AMD Phoenix 的 GOP 模式、MCFG/DSDT、IOMMU 行为；
- 触摸板：本机是 **I2C HID**（Linux: `SYNA2BA6:00 06CB:CE78`，挂在 AMD `AMDI0010` I2C 控制器）。驱动已实现：`i2c-designware.c`（轮询主机，校验 `IC_COMP_TYPE`、按 Linux 公式算 SCL 时序、传输期间 disable/enable 以锁存 `IC_TAR`）+ `i2c-hid.c`（HID-over-I2C 命令/寄存器框架、report descriptor 解析和多点触控 `ABS_MT_*`）。后台线程每 8 ms 检查 attention GPIO，在线路有效时读取报告。本机固件给出地址 `0x2c`、400 kHz 和描述符寄存器 `0x0020`。第 7 次已在实机读取描述符并注册 evdev。修订后的移动和点击待实机验证。 IC_CLK 缺省按 100 MHz 计，可用 `a20.i2c.ic_clk=` / `a20.i2c.speed=` 校正；启动自检覆盖解析器、协议框架和多点触控（`[I2C-HID] {parser,protocol,multitouch} self-test: PASS`），`smoke-laptop-shell`/`smoke-laptop-gui` 都断言这些行。
- 有线网卡：RTL8111/8168 已实现 `r8169.c`，寄存器语义对齐 Linux `r8169_main.c`（`RxConfig`/`TxConfig`/`TxPoll` 门铃/`RxMaxSize`/MAR 只写两组、随机 MAC 回退、`PHYstatus` 读 carrier）。协议栈现在 1 Hz 复查`link_up`，所以拔插网线会真正改变接口状态。**QEMU 没有 r8168 模型，未做运行时验证**；描述符位运算有启动自检（`[SELFTEST] r8169: PASS`），可用 `a20.r8169.poll=1` 强制轮询排障。
- 无线网卡：RTL8852BE 需要固件与完整 802.11 栈，不在当前范围；
- 桌面为软件渲染，1080p 以上分辨率可能明显卡顿；
- 多核：桌面实例为 `smp = 16`。OVMF 下已验证 16 个逻辑处理器在线， Lenovo 实机上一版验证了 4 个；
- USB 键盘：xHCI 枚举后，boot-protocol HID 键盘在文本 shell 中会经`uart_receive_char()` 进入 `/dev/tty`；GUI 下仍走 evdev。非 boot-protocol 的 HID 键盘（需要解析 report descriptor）仍未支持。

## 日志与黑屏取证

U 盘镜像带一个独立的 64 MiB `"A20OS log"` FAT32 分区。loader 在跳入内核前就会写 `early-<bootcount>.jsonl`（同一分区的结构化阶段记录）和 ESP 的`/A20OS/boot.log`；内核在 VFS/distro 挂载后、网络初始化前挂载日志分区，并立即把 ring 里从第一行开始的积压记录补写成 `boot-<id>.jsonl`。屏幕完全不亮时，先拔下 U 盘读这两个位置，而不是只看串口。完整说明、JSONL 字段和`tools/a20log` 用法见[kernel/logging/persistent-logging.md](../kernel/logging/persistent-logging.md)。

### 没有专用日志分区时

`mode=auto` 在找不到 `"A20OS log"` 分区时，会跳过 USB 块设备，在内置 NVMe/AHCI 的 ESP 根目录创建 `A20LOG/` 并写同一套 JSONL。Linux 上通常是：

```sh
ls -la /boot/efi/A20LOG
cat /boot/efi/A20LOG/CURRENT
tools/a20log show --dir /boot/efi/A20LOG --level warn
```

这解决的是“xHCI 失效 → U 盘日志分区也不可用”的循环依赖。内核只写`A20LOG/`，不改动 ESP 里原有的 `EFI/`、`A20OS/` 或其他文件；强制断电最多丢最后一次未提交的尾部，已提交记录不要求安全关机。若内置盘没有 ESP 或 ESP 只读，仍会退回 rootfs，并在 `[LOG]` 行里说明。

### 屏幕诊断模式

Boot Report 已推广为全平台功能，普通启动不再在 shell 后自动重放。键盘不可用、无法滚动查看早期输出时，在打包时显式启用：

```sh
tools/a20 package x86_64-laptop-shell -- LOG_REPORT=always
```

也可通过 `[log] report = "error"` 只在启动窗口内检测到错误时输出，或由在已经设置 `boot_report=true` 构建的镜像上由 bootargs 设置 `a20.log.report=always`。完整规则见[Boot Report 配置与屏幕诊断](../kernel/logging/boot-report.md)。

报告包含板型、启动阶段、日志存储/写入状态、时间戳和最多 48 条诊断记录，优先保留错误。原始 PS/2 和 xHCI 记录仍可出现，但报告不再固定输出仅适用于笔记本的摘要字段。没有键盘时可直接拍这一屏；输入可用时随时执行：

```sh
cat /proc/a20/boot_report
```

RAM 日志明确标为易失；磁盘记录仍在前述 `A20LOG/` 或日志分区中。报告是一份保留记录的快照，不会把自身再写回日志。

板级还会打印一行 `[PS2] board: firmware=... platform=...`。如果 ACPI 的 PNP0303 落在非 platform 总线，旧代码会跳过静态 i8042 回退，导致 `ps2`驱动根本不 probe；现在只有 `platform=1` 才复用固件设备，否则补建静态 platform 设备。

如果 `[PS2] init` 的 `selftest=1 kbd_reset=1` 正常，但路由行是：

```text
[PS2] keyboard route: gsi=1 unavailable (-22), pic=33
```

说明键盘控制器本身健康，问题是 ACPI 已经用不同触发方式占用了 GSI1，`arch_firmware_irq_map()` 拒绝改动，驱动被留在 8259 PIC 上。新驱动会强制把 GSI1 写回 edge/active-high 并让 `request_irq()` unmask，日志变成：

```text
[PS2] keyboard route: gsi=1 vector=33 (ioapic, forced; firmware map returned -22)
```

如果仍是 `pic=33`，把完整 `[PS2]` 和 `[IOAPIC]` 行从 `A20LOG` 发回来。

## 排障

| 现象 | 检查 |
| --- | --- |
| 固件菜单看不到 U 盘 | 写盘完整性、USB 启动项、Secure Boot |
| 屏幕完全不亮 | 读 ESP `/A20OS/boot.log` 的最后一条 `STAGE`，以及 `"A20OS log"` 分区的 `early-<bootcount>.jsonl`；有内核日志时再看最新 `boot-*.jsonl` |
| `[FIRMWARE] ... _CRS/IRQ: -E...; continuing` | 单个 ACPI 节点资源解析失败，枚举会继续；记录节点名和错误码，通常不影响启动 |
| 以前出现 `boot: firmware device enumeration failed` | 已改为记录 `[FIRMWARE]` 后继续；如果仍无设备，回传该行节点名 |
| 停在 `A20OS: UEFI: loading kernel` | 32 MiB 目标区被固件保留；loader 会在屏幕上报 destination descriptor，并在 `boot.log` 写 `FAIL` |
| 有日志但没有 `state=clean` | 断电或 panic；看最后一条 `mono_ns` 记录与 `CURRENT` 的 `state` |
| 控制台一行要等很久 / 像死锁 | 新镜像应使用 TUI 式整屏刷新、WC framebuffer 和非阻塞控制台锁；若仍慢，记录 `[GPU] firmware framebuffer` 的物理地址与分辨率 |
| 屏幕在启动早期一直黑，之后突然出现整屏 | 这是 WC alias 就绪前只更新网格、不刷屏的正常行为；若始终不出现，读 loader/内核日志 |
| 文本 shell 出现但没有桌面 | `/extra/etc/a20-distro` 是否存在；串口是否有 `[DISTRO]`；U 盘第二个分区是否完整 |
| `[DISTRO] no unique ...` | 镜像不是由 `mk_uefi_fat_image.sh` 生成，或第二个分区名/GUID 不对 |
| 有桌面但键盘无输入 | 同时检查 `[PS2]` 的初始化与 IRQ1 路由，以及 `048d:c102` 的枚举、`[USB-HID]` 绑定和按键事件 |
| `[ERR] [XHCI] ...` | `wanted=32` 表示 Transfer Event 等待超时，`wanted=33` 表示 Command Completion Event 等待超时。`ADDRESS_DEVICE` 的 `cc=17` 应结合打印的 Slot 和 EP0 字段定位 |
| 桌面黑屏但面板可见 | 软件渲染重绘中；等待数秒。持续黑屏看串口 `[DRM] present`/panic |
| `poweroff` 不断电 | 记录主板型号、固件版本与屏幕/串口最后一行，属于 S5 验收缺口 |

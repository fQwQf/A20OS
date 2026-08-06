# A20OS USB 子系统设计

> 状态：已实现（HID + Mass Storage + xHCI）。本设计文档记录了最初的架构决策；当前实现为`kernel/drivers/usb/`（core / host/xhci / class/usb-hid / class/usb-storage）。

## 1. 背景与目标

A20OS 目前仅有一个窄用途驱动 `kernel/drivers/input/xhci_hid.c`（1165 行）：硬编码绑定 Intel 8086:1e31 XHCI 控制器，只解析 HID 1.11**boot protocol** 键盘/鼠标，轮询工作，单控制器单实例（静态 `g_xhci`）。它没有 USB 设备树、没有 URB 抽象、不支持存储设备、不支持热插拔。它发布 input class 设备，事件经 `/dev/event0` mux（`kernel/drivers/input/input_mux.c`）与 virtio-input 等输入源聚合。

目标：引入一个**通用 USB 子系统**，使任意 USB 宿主控制器上的 HID 与Mass Storage 设备通过统一模型工作，并接入现有输入/块设备类接口。

## 2. 现状评估（A20OS 已有基础）

| 已有能力 | 位置 | 说明 |
|----------|------|------|
| 驱动模型 | `kernel/drivers/core/` | `driver_t`/`device_t`/`bus_type_t`，`match`/`probe`/`remove` |
| 总线热插拔钩子 | `bus_type_t.hotplug(dev, BUS_EVENT_ADD/REMOVE)` | 已预留，尚未被驱动使用 |
| PCI 枚举 | `kernel/drivers/bus/pci_bus.c` | 宿主控制器可作为 PCI 设备绑定 |
| 输入类接口 | `input_dev_ops_t`（read/ioctl/poll）+ `DEV_CLASS_INPUT` | HID 接入点，已存在 |
| 块类接口 | `block_dev_ops_t`（read/write/flush/capacity/sector_size）+ `DEV_CLASS_BLOCK` | MSC 接入点，已存在 |
| XHCI 底层 | `xhci_hid.c` | MMIO、TRB/事件环、输入/输出上下文、命令与控制传输、端口枚举、轮询——全部已有，只是与 HID 解析耦合 |
| 内核线程 | `init_kthread` 等 | 可承载枚举/轮询工作 |

**结论**：A20OS 的驱动模型已经预见了热插拔总线，输入/块类接口开箱可用。移植通用 USB **不需要改动现有驱动模型**，但**需要新增三个东西**：

1. **USB 核心子系统**（URB 抽象 + 设备/接口/端点模型 + 枚举流程）；
2. **HCD（宿主控制器驱动）抽象**（把 `xhci_hid.c` 的控制器逻辑重构为 与协议解析解耦的可复用 HCD，暴露 URB 提交 + 根集线器端口管理）；
3. **动态设备生命周期**（端口变化检测 → 枚举 → 总线热插拔发布 → 类驱动绑定， 以及移除路径）。

## 3. 总体架构

```text
+-------------------------------------------------------------------+
|  VFS / devfs                     input 子系统 (evdev 风格)         |
|  block 层 (virtio_blk 等)  ←block_dev_ops_t        input_dev_ops_t|
+-------------------------------------------------------------------+
   |                                      |
   | class_device publish                 | class_device publish
+-------------------------------------------------------------------+
|                    USB 核心 (kernel/drivers/usb/core)              |
|  · usb_bus_type (bus_type "usb")  +  hotplug(BUS_EVENT_ADD/REMOVE) |
|  · usb_device/usb_interface/usb_endpoint（从描述符构建）            |
|  · URB 抽象（struct urb：端点 + 传输类型 + 缓冲区 + 完成回调）       |
|  · 枚举流程：地址分配 → 配置描述符 → 接口解析 → 类驱动匹配          |
|  · 类驱动注册表：class/subclass/protocol → driver_t                 |
+-------------------------------------------------------------------+
   | URB 提交 / 完成                  | URB 提交 / 完成
+-------------------------------------------------------------------+
|      HCD 层 (kernel/drivers/usb/host)                               |
|  · usb_hcd_t：transfer(urb), poll_ports(), ring/event 队列, DMA     |
|  · xhci_hcd（由 xhci_hid 重构，独立实例 per-controller）             |
|  · （远期）ehci/uhci/ohci_hcd                                       |
+-------------------------------------------------------------------+
   | PCI / platform 绑定
   bus_type "pci" → 宿主控制器 device_t
```

### 关键分层原则（遵循 A20OS 现有约定）

- **HCD 只负责传输**，不解析 USB 协议；协议解析（HID/MSC）在类驱动。
- **USB 核心不依赖任何具体 HCD**，只依赖 `usb_hcd_t` 接口。
- 类驱动通过 `device_id_t`（vendor=USB class<<16|subclass<<8|protocol） 在 usb 总线上匹配，沿用现有 `driver_t` 生命周期。
- 不引入全局锁；每个控制器/接口私有锁，锁序固定（设备锁 > URB 锁 > 环锁）。

## 4. USB 核心设计

### 4.1 URB（USB Request Block）

```c
typedef struct urb {
    usb_endpoint_t  *ep;        /* 目标端点 */
    uint8_t          transfer_type; /* CTRL/INT/BULK/ISO */
    uint8_t          direction;     /* IN/OUT */
    void            *buf;        /* 传输缓冲（DMA 一致或 bounce） */
    size_t           len;
    uint64_t         interval;   /* INT/ISO 轮询间隔 */
    int              status;
    void           (*complete)(struct urb *urb);
    struct device   *usb_dev;    /* 所属 usb 接口设备 */
    struct urb      *next;       /* 待办队列（每端点一个头节点） */
} urb_t;
```

- 每个端点一个 URB 待办队列，由 HCD 轮询/中断驱动 drain。
- 中断传输（HID 报告）用**循环提交**（complete 里重新提交），沿用 xhci_hid 现有的轮询报告路径。
- 控制传输走核心封装：`usb_control_msg()`（标准请求 set/get，包 setup + data + status 三个阶段）。

### 4.2 设备树模型

```c
usb_device_t     /* 逻辑设备：地址、速度、device descriptor、配置 */└─ usb_interface_t[]  /* 来自配置描述符；每个 interface 一个 device_t */
       └─ usb_endpoint_t[]  /* 端点描述符 */
```

- 每个 `usb_interface_t` 发布一个 `device_t`（bus = "usb"， hardware_id = { class, subclass, protocol, interface_number }）。
- 类驱动按 interface 的 class/subclass/protocol 匹配。

### 4.3 枚举流程

1. HCD 轮询根集线器端口：`XHCI_PORTSC` 的 CSC/CCS 变化 → 复位端口。
2. 新连接：地址 0 复位设备 → `SET_ADDRESS` 分配地址 → 读 device descriptor → 读配置描述符 → 解析 interface/endpoint。
3. 为每个 interface 构建 `device_t` → `device_register()` → `bus_probe_device()`（触发 usb 总线上的类驱动匹配）。
4. 热插拔：`bus_type.hotplug(dev, BUS_EVENT_ADD/REMOVE)` 通知核心， 由 devfs/sysfs 反映状态；移除时 `driver->remove()` + URB 队列清空。

### 4.4 多控制器

- 每控制器独立 `xhci_controller_t`（由 `device_t->drv_priv` 持有）， 替换当前单例 `g_xhci`。`xhci_hid` 的 `id_table` 泛化为支持更多 XHCI PCI ID（`VENDOR_ANY` + 类匹配或通用 XHCI PCI class 0x0C0330）。

## 5. HCD 接口（由 xhci_hid 重构而来）

```c
typedef struct usb_hcd {
    int  (*start)(struct usb_hcd *hcd);
    int  (*stop)(struct usb_hcd *hcd);
    int  (*submit_urb)(struct usb_hcd *hcd, struct urb *urb);
    int  (*poll)(struct usb_hcd *hcd);            /* 轮询端口+事件环 */
    int  (*reset_port)(struct usb_hcd *hcd, int port);
    void (*enable_slot)(...);                     /* 内部：xHCI 槽管理 */
} usb_hcd_t;
```

重构要点（从 `xhci_hid.c` 抽取，行为不变、协议解耦）：

- `xhci_ring/enqueue/dispatch/wait_event/command/control` → HCD 内部；
- `hid_keyboard_report/mouse_report/tablet_report` → 移入 usb-hid 类驱动；
- `xhci_parse_hid/configure_hid` → 由 usb 核心的 interface 枚举替代， 端点/接口信息从描述符读取而非 HID 专属解析；
- 轮询入口保留（`xhci_poll_locked`），新增中断路径（可选）。

## 6. 类驱动

### 6.1 usb-hid（DEV_CLASS_INPUT）

- 匹配 class=3（HID）的 interface。
- 仅先支持 boot protocol 键盘/鼠标（沿用 xhci_hid 已证实的解析）， HID report descriptor 完整解析为后续项。
- 产出 `input_dev_ops_t`，事件经现有 input 子系统（与 xhci_hid 当前 `/dev/input` 路径一致）。

### 6.2 usb-storage / BOT（DEV_CLASS_BLOCK）

- 匹配 class=8（Mass Storage）、subclass=6（SCSI transparent）、 protocol=0x50（BOT）。
- Bulk-only transport：CBW/CSW 包 + bulk-in/out 端点。
- 转发 SCSI 命令（INQUIRY/READ10/WRITE10/READ_CAPACITY），映射到 `block_dev_ops_t`（read/write 以 LBA 计）。
- 复用 VFS `block_cache` + 现有文件系统（fat32/ext4/ntfs 可挂载）。

### 6.3 集线器

- 早期：仅根集线器（HCD 内置），无外部 hub 级联（`-ENOTSUP`）。
- 中期：通用 hub 驱动（class=9），递归枚举。这是设备树完整性的关键， 但复杂度高，单独一个里程碑。

## 7. 线程 / 并发模型

- 枚举与端口轮询放在**每控制器内核线程**（如 `usb_hcd_kthread`）， 与现有 `xhci_hid` 的调用方轮询（`input` read/poll 里 poll）并存：
  - 枚举：控制器线程负责；
  - HID 报告：保留现有"读时轮询"模型（低延迟、无中断依赖）， 后续可选中断驱动。
- URB 完成处理在 HCD 轮询上下文（控制器线程或 read/poll 路径）。
- 锁序：`usb_device.lock → usb_interface.lock → ep->lock → hcd->lock`， 禁止反向。

## 8. 内存 / DMA 模型

- 传输缓冲：HCD 分配 DMA 一致内存（现有 `xhci_ring` 已用同一套页分配）， 类驱动经 URB 缓冲拷贝；避免类驱动直接碰 DMA 地址。
- 描述符解析缓冲为内核堆，校验长度后使用（沿用 xhci_hid 的边界检查风格）。

## 9. 集成点与 Makefile

- 新增目录：`kernel/drivers/usb/{core,host,class}/`，随 `$(wildcard $(KERNEL_DIR)/drivers/*/*.c)` 自动编译（Makefile 无需改动， 现有通配已覆盖 `drivers/usb/core|host|class` 三层需确认展开）。
- `bus_type "usb"` 注册在 `driver_core_init` 之后（现有总线注册序列）。
- 类驱动 `DRIVER_REGISTER()` 自动进入 `.driver_init`。

## 10. 测试计划（QEMU）

| 用例 | QEMU 参数 | 验收 |
|------|-----------|------|
| xhci + usb 键盘 | `-device qemu-xhci -device usb-kbd` | `/dev/input` 出现键盘事件 |
| xhci + usb 鼠标/平板 | `-device usb-tablet` | 鼠标事件 |
| xhci + usb 存储 | `-drive ... -device usb-storage,drive=x` | 块设备 `/dev/vdX`，可挂载 fat32/ext4 |
| 无 HID 控制器 | 无 usb 设备 | 正常启动，无 panic |
| 回归 | 现有 `smoke-riscv64` 等 | 不退化 |

新增 `smoke-usb-*` 目标（沿用现有 smoke 框架：QEMU 启动 + grep PASS）。

## 11. 里程碑

**Phase 1（本次实现范围建议）——XHCI HCD 重构 + USB 核心 + HID boot**
- 抽取 `xhci_hid.c` 控制器逻辑为 `usb/host/xhci.c`（usb_hcd_t）。
- 新建 `usb/core/`：URB、usb_device/interface/endpoint、枚举、 `usb` 总线、控制传输封装。
- 新建 `usb/class/hid.c`：boot 键盘/鼠标 → `input_dev_ops_t`。
- QEMU `usb-kbd/usb-tablet` 通过；现有 xhci_hid 行为不回归。
- 预计：~2500–3500 行（重构 + 新增）。

**Phase 2——BOT 存储**
- `usb/class/storage.c`：BOT + SCSI 子集 → `block_dev_ops_t`。
- QEMU `usb-storage` 可挂载文件系统。
- 预计：~1200–1800 行。

**Phase 3——hub 与其它 HCD**
- 通用 hub 驱动（递归枚举）。
- EHCI/UHCI/OHCI（净室，参考规范）。可选。

## 12. 风险

- **xHCI 规范复杂**：槽/端点上下文、TRB 状态机；从 xhci_hid 重构时 必须保持传输语义不变，否则键盘立即失效。重构后跑 `smoke-*` 兜底。
- **热插拔竞态**：设备移除与 URB 提交并发 → URB 队列与 class_device 生命周期需严格配对（沿用 class_device `online` 标记）。
- **无硬件回退**：仅 QEMU `qemu-xhci` 验证；真机 PCI ID 需扩展 `id_table`。
- **BOT 命令超时**：需命令超时/重试，避免卡死块 I/O。

## 13. 与"设计原则"的一致性

- 分层薄接口、无 ABI 互相包装依赖（USB core 不碰 Linux/native ABI）。
- 不引入 POSIX 异步信号/回调式中断风暴；HID 沿用读时轮询（与现有 xhci_hid 一致），不新增中断驱动复杂度。
- 能力/权限、时间约束等 native 概念不进入 USB core。

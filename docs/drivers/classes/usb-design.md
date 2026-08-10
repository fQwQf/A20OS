# A20OS USB 子系统设计

> 状态：已实现（HID + Mass Storage + xHCI）。本设计文档同时保留最初的架构决策和明确标注的未来计划；当前共享实现为 `kernel/drivers/usb/`（`core/usb_core.c`、`host/xhci.c`、`class/usb_hid.c`、`class/usb_storage.c`）。generic 在 `tools/driver-modules.mk` 实际列出的 x86_64/aarch64/loongarch64 上通过 `xhci.a20drv`、`usb-hid.a20drv`、`usb-storage.a20drv` 部署；embedded 静态链接这些共享源码。

## 1. 背景与目标

A20OS 最初的 USB 驱动是窄用途的 `kernel/drivers/input/xhci_hid.c`（1165 行）：硬编码绑定 Intel 8086:1e31 XHCI 控制器，只解析 HID 1.11**boot protocol** 键盘/鼠标，轮询工作，单控制器单实例（静态 `g_xhci`）。它没有 USB 设备树、没有 URB 抽象、不支持存储设备、不支持热插拔。该文件已随通用 USB 子系统落地而移除，HID 事件面迁入 `kernel/drivers/usb/class/usb_hid.c`（经 `/dev/event0` mux `kernel/drivers/input/input_mux.c` 与 virtio-input 等输入源聚合）。

目标：引入一个**通用 USB 子系统**，使任意 USB 宿主控制器上的 HID 与Mass Storage 设备通过统一模型工作，并接入现有输入/块设备类接口。

## 2. 现状评估（A20OS 已有基础）

| 已有能力 | 位置 | 说明 |
|----------|------|------|
| 驱动模型 | `kernel/drivers/core/` | `driver_t`/`device_t`/`bus_type_t`，`match`/`probe`/`remove` |
| 总线热插拔钩子 | `device_hotplug(dev, BUS_EVENT_ADD/REMOVE)` | USB core 已用于 interface 发布和移除 |
| PCI 枚举 | `kernel/drivers/bus/pci_bus.c` | 宿主控制器可作为 PCI 设备绑定 |
| 输入类接口 | `input_dev_ops_t`（read/ioctl/poll）+ `DEV_CLASS_INPUT` | HID 接入点，已存在 |
| 块类接口 | `block_dev_ops_t`（read/write/flush/capacity/sector_size）+ `DEV_CLASS_BLOCK` | MSC 接入点，已存在 |
| XHCI 底层 | 历史 `xhci_hid.c`（已移除） | MMIO、TRB/事件环、输入/输出上下文、命令与控制传输、端口枚举、轮询——重构为 `kernel/drivers/usb/host/xhci.c` 的 `usb_hcd_t` |
| 内核线程 | `init_kthread` 等 | 可承载枚举/轮询工作 |

**历史结论**：落地通用 USB 时，既有驱动模型和输入/块类接口可复用，当时需要新增以下三部分；它们现在都已经实现：

1. **USB 核心子系统**（URB 抽象 + 设备/接口/端点模型 + 枚举流程）；
2. **HCD（宿主控制器驱动）抽象**（把 `xhci_hid.c` 的控制器逻辑重构为与协议解析解耦的可复用 HCD，暴露 URB 提交 + 根集线器端口管理）；
3. **动态设备生命周期**（端口变化检测 → 枚举 → 总线热插拔发布 → 类驱动绑定，以及移除路径）。

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
|  · URB 抽象（usb_urb_t：端点 + 传输类型 + 缓冲区 + 完成回调）      |
|  · 枚举流程：地址分配 → 配置描述符 → 接口解析 → 类驱动匹配          |
|  · 类驱动注册表：class/subclass/protocol → driver_t                 |
+-------------------------------------------------------------------+
   | URB 提交 / 完成                  | URB 提交 / 完成
+-------------------------------------------------------------------+
|      HCD 层 (kernel/drivers/usb/host)                               |
|  · usb_hcd_t：transfer(urb), poll_ports(), ring/event 队列, DMA     |
|  · xhci_hcd（由 xhci_hid 重构，独立实例 per-controller）             |
|  ·（远期）ehci/uhci/ohci_hcd                                       |
+-------------------------------------------------------------------+
   | PCI / platform 绑定
   bus_type "pci" → 宿主控制器 device_t
```

### 关键分层原则（遵循 A20OS 现有约定）

- **HCD 只负责传输**，不解析 USB 协议；协议解析（HID/MSC）在类驱动。
- **USB 核心不依赖任何具体 HCD**，只依赖 `usb_hcd_t` 接口。
- 类驱动通过 `device_id_t`（vendor=USB class<<16|subclass<<8|protocol）在 usb 总线上匹配，沿用现有 `driver_t` 生命周期。
- 当前没有已实现的“设备锁 > URB 锁 > 环锁”层级。USB core 由 process-context progress poll 驱动；HID/storage 有各自 class lock，而 `xhci->lock` 目前只初始化、未实际获取。若未来并行提交 URB 或并行 hotplug，必须先实现 controller 级同步并在[锁顺序契约](../guide/lock-order.md)记录单向顺序。

## 4. USB 核心设计

### 4.1 URB（USB Request Block）

```c
typedef struct usb_urb {
    usb_device_t    *dev;
    usb_endpoint_t  *ep;        /* 目标端点 */
    uint8_t          transfer_type; /* CTRL/INT/BULK/ISO */
    uint8_t          direction;     /* IN/OUT */
    void            *buf;        /* 传输缓冲（DMA 一致或 bounce）*/
    size_t           len;
    int              status;
    void           (*complete)(struct usb_urb *urb);
    void            *ctx;        /* class-driver context */
} usb_urb_t;
```

- xHCI endpoint 保存当前 pending URB，HCD 轮询 event ring 并完成它；当前不是通用多项端点待办队列。
- 中断传输（HID 报告）由 class 驱动重新提交，沿用轮询 event ring 的进展路径。
- 控制传输走核心封装：`usb_control_msg()`（标准请求 set/get，包 setup + data + status 三个阶段）。

### 4.2 设备树模型

```c
usb_device_t             /* 逻辑设备：地址、速度、descriptor、配置 */
└─ usb_interface_t[]     /* 每个 interface 发布一个 device_t */
   └─ usb_endpoint_t[]   /* 端点描述符 */
```

- 每个 `usb_interface_t` 发布一个 `device_t`（bus = "usb"，hardware_id = { class, subclass, protocol, interface_number }）。
- 类驱动按 interface 的 class/subclass/protocol 匹配。

### 4.3 枚举流程

1. HCD 轮询根集线器端口：`XHCI_PORTSC` 的 CSC/CCS 变化 → 复位端口。
2. 新连接：地址 0 复位设备 → `SET_ADDRESS` 分配地址 → 读 device descriptor → 读配置描述符 → 解析 interface/endpoint。
3. 为每个 interface 构建 `device_t`，调用 `device_hotplug(..., BUS_EVENT_ADD)`，由统一核心注册、匹配并 probe class driver。
4. 断开时调用 `device_hotplug(..., BUS_EVENT_REMOVE)`，先下线 class/devfs/sysfs，再执行 `driver->remove()`；随后 HCD `abort_slot` 并释放 interface/endpoint/device 存储。

### 4.4 多控制器

- 每控制器动态分配独立 `xhci_controller_t`（由 `device_t->drv_priv` 持有），USB core 最多登记四个 HCD。xHCI 使用 ANY PCI ID 表再由 `.match` 精确检查 PCI class `0x0C0330`。

## 5. HCD 接口（由 xhci_hid 重构而来）

```c
typedef struct usb_hcd {
    const usb_hcd_ops_t *ops;
    device_t *hcd_dev;
    unsigned max_ports;
    uint8_t *port_state;
    usb_device_t **port_devices;
    void *priv;
} usb_hcd_t;
```

当前 `usb_hcd_ops_t` 由 `start`、`poll`、端口状态/复位、slot 初始化、EP0 control/descriptor、endpoint 配置、interrupt/bulk 提交和 `abort_slot` 组成；权威声明是 `kernel/include/drivers/usb/usb.h`，不存在文中旧草案的单一 `submit_urb` 或 `stop` 回调。

历史重构要点（已完成）：

- `xhci_ring/enqueue/dispatch/wait_event/command/control` → HCD 内部；
- `hid_keyboard_report/mouse_report/tablet_report` → 移入 usb-hid 类驱动；
- `xhci_parse_hid/configure_hid` → 由 usb 核心的 interface 枚举替代，端点/接口信息从描述符读取而非 HID 专属解析；
- 轮询入口保留（`xhci_poll_locked`）；当前没有 IRQ completion 路径。

## 6. 类驱动

### 6.1 usb-hid（DEV_CLASS_INPUT）

- 匹配 class=3（HID）的 interface。
- **当前限制**：只支持 boot protocol 键盘/鼠标。
- **未来计划**：增加完整 HID report descriptor 解析。
- 产出 `input_dev_ops_t`，事件经 input class 聚合到 `/dev/event0`。

### 6.2 usb-storage / BOT（DEV_CLASS_BLOCK）

- 匹配 class=8（Mass Storage）、subclass=6（SCSI transparent）、protocol=0x50（BOT）。
- Bulk-only transport：CBW/CSW 包 + bulk-in/out 端点。
- 转发 SCSI 命令（INQUIRY/READ10/WRITE10/READ_CAPACITY），映射到 `block_dev_ops_t`（read/write 以 LBA 计）。
- 复用 VFS `block_cache` + 现有文件系统（fat32/ext4/ntfs 可挂载）。

### 6.3 集线器

- **当前限制**：仅根集线器（HCD 内置），无外部 hub 级联。
- **未来计划**：增加通用 hub 驱动（class=9）和递归枚举；这不是当前已实现能力。

## 7. 线程 / 并发模型

- 当前没有 per-controller 内核线程。`kernel_progress_poll()` 调用按 250 ms 节流的 `usb_core_poll()`，后者扫描 HCD 端口并推进枚举/移除；input read/poll 还会推进 HID 事件。
- URB 完成处理在 HCD 轮询上下文。
- **未来计划**：若改为 controller worker 或 IRQ completion，必须补 controller/endpoint 并发状态机和锁契约；不得沿用旧草案中尚未实现的锁序。

## 8. 内存 / DMA 模型

- 传输缓冲：HCD 分配 DMA 一致内存（现有 `xhci_ring` 已用同一套页分配），类驱动经 URB 缓冲拷贝；避免类驱动直接碰 DMA 地址。
- 描述符解析缓冲为内核堆，校验长度后使用（沿用 xhci_hid 的边界检查风格）。

## 9. 集成点与 Makefile

- 共享目录是 `kernel/drivers/usb/{core,host,class}/`。`usb_core.c` 显式列入 `GENERIC_KERNEL_SERVICE_SRCS`；xHCI/HID/storage 显式列入 `EMBEDDED_DEVICE_DRIVER_SRCS`，generic 则由 `tools/driver-modules.mk` 的架构包清单构建。不存在递归 wildcard 自动纳入。
- `bus_type "usb"` 注册在 `driver_core_init` 之后（现有总线注册序列）。
- 类驱动 `DRIVER_REGISTER()` 自动进入 `.driver_init`。

## 10. QEMU 验证矩阵

| 用例 | QEMU 参数 | 验收 |
|------|-----------|------|
| xhci + usb 键盘 | `-device qemu-xhci -device usb-kbd` | `/dev/event0` 出现键盘事件 |
| xhci + usb 鼠标/平板 | `-device usb-tablet` | 鼠标事件 |
| xhci + usb 存储 | `-drive ... -device usb-storage,drive=x` | 动态块设备 `/dev/diskN`，可挂载 fat32/ext4 |
| 无 HID 控制器 | 无 usb 设备 | 正常启动，无 panic |
| 回归 | 现有 `smoke-riscv64` 等 | 不退化 |

仓库定义了 `make smoke-usb-x86_64`，但该目标只挂载键盘和鼠标并检查 HID 枚举，不覆盖 storage mount。其他表项是手工或未来扩展矩阵，不能从表格推断已有同名自动目标。

## 11. 历史里程碑与未来计划

**Phase 1（历史计划，已实现）——XHCI HCD 重构 + USB 核心 + HID boot**
- 抽取 `xhci_hid.c` 控制器逻辑为 `usb/host/xhci.c`（usb_hcd_t）。
- 新建 `usb/core/`：URB、usb_device/interface/endpoint、枚举、`usb` 总线、控制传输封装。
- 新建 `usb/class/usb_hid.c`：boot 键盘/鼠标 → `input_dev_ops_t`。
- QEMU `usb-kbd/usb-tablet` 通过；现有 xhci_hid 行为不回归。
- 预计：~2500–3500 行（重构 + 新增）。

> **现状**：Phase 1 与 Phase 2 已实现——`kernel/drivers/usb/host/xhci.c`、`kernel/drivers/usb/core/usb_core.c`、`kernel/drivers/usb/class/usb_hid.c`、`kernel/drivers/usb/class/usb_storage.c` 均已落地，`xhci_hid.c` 已删除；drvmod 侧 `xhci.c`/`usb_hid.c`/`usb_storage.c` 以模块形式提供（`smoke-usb-x86_64`）。

**Phase 2（历史计划，已实现）——BOT 存储**
- `usb/class/usb_storage.c`：BOT + SCSI 子集 → `block_dev_ops_t`。
- QEMU `usb-storage` 可挂载文件系统。
- 预计：~1200–1800 行。

**Phase 3（未来计划，未实现）——hub 与其它 HCD**
- 通用 hub 驱动（递归枚举）。
- EHCI/UHCI/OHCI（净室，参考规范）。可选。

## 12. 风险

- **xHCI 规范复杂**：槽/端点上下文、TRB 状态机；从 xhci_hid 重构时必须保持传输语义不变，否则键盘立即失效。重构后跑 `smoke-*` 兜底。
- **热插拔竞态**：设备移除与 URB 提交并发 → URB 队列与 class_device 生命周期需严格配对（沿用 class_device `online` 标记）。
- **当前锁缺口**：xHCI controller lock 未实际获取；HID completion 可由全局 poll 或其他 xHCI wait 在未持有 HID lock 时调用；USB storage 又跨同步 bulk wait 持有 spinlock。详见[锁顺序契约](../guide/lock-order.md)。这些是现状限制，不是未来并发设计。
- **硬件覆盖有限**：当前按 PCI xHCI class `0x0c0330` 匹配，并在 QEMU/VirtualBox 模拟控制器上验证；真实控制器的固件、IOMMU、cache coherency 和错误恢复仍需单独验证。
- **BOT 错误恢复**：xHCI 的同步 transfer wait 有有界超时，storage 失败后会发送 class reset，但没有完整的 endpoint halt 清理、REQUEST SENSE 或重试状态机。

## 13. 与"设计原则"的一致性

- 分层薄接口、无 ABI 互相包装依赖（USB core 不碰 Linux/native ABI）。
- 不引入 POSIX 异步信号/回调式中断风暴；HID 沿用读时轮询（与现有 xhci_hid 一致），不新增中断驱动复杂度。
- 能力/权限、时间约束等 native 概念不进入 USB core。

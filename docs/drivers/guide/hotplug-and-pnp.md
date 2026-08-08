# 热插拔与即插即用

本章说明 A20OS 中设备在运行期出现和消失时的生命周期，以及驱动如何安全适配。这里的“即插即用”指设备被总线发现后，内核根据总线身份自动匹配并调用驱动 `probe`；它不表示设备可以跳过资源初始化或错误处理。

## 统一事件路径

所有可热插拔设备必须经过驱动核心，而不是直接调用某个驱动的 `probe` 或 `remove`：

```text
bus detects add
  -> device_hotplug(dev, BUS_EVENT_ADD)
  -> device_register
  -> bus match + optional driver match
  -> driver probe
  -> class device online / devfs published

bus detects remove
  -> device_hotplug(dev, BUS_EVENT_REMOVE)
  -> device_unregister
  -> class device offline
  -> driver remove
  -> bus releases its device record
```

`device_hotplug()` 位于 `kernel/drivers/core/driver_core.c`。ADD 成功后才调用可选的 `bus_type_t.hotplug(..., BUS_EVENT_ADD)`；REMOVE 则在驱动 `remove` 完成后通知总线。因此 bus 回调只处理总线自己的记录或事件传播，不能取代驱动生命周期。

注册会同步执行匹配和 `probe`，注销会同步执行 `remove`。所以只能从可睡眠的进程、worker 或调度/idle 下半部调用，绝不能直接从硬件中断处理程序调用。

## 当前总线支持

### USB

USB 是当前端到端支持的热插拔总线：

1. `usb_core_poll()` 在进程上下文按 250 ms 节流执行；它由 kernel progress bridge 调用。
2. 每个 HCD 轮询事件环并读取每个 root port 的连接状态。
3. 新连接端口经过 reset、slot/address 分配、descriptor/configuration 解析；每个 USB interface 被发布为一个 `device_t`，USB class driver 自动匹配并 probe。
4. 连接消失时，核心先对每个 interface 发出 REMOVE，等待 class driver 停止 URB、IRQ/DMA 和 I/O，然后调用 HCD 的 `abort_slot`，最后释放 interface、endpoint 与 `usb_device_t` 存储。

USB HCD 应准确实现 `port_connected` 和 `abort_slot`。`abort_slot` 必须取消该 slot 的未完成传输，保证在它返回后不会再回调已经释放的 `usb_urb_t` 或 interface 私有状态。

### PCI

PCI 枚举器维护以 BDF（bus/device/function）为身份的稳定 `device_t` 记录。`pci_rescan()` 将当前 ECAM 配置空间与已发布设备集合对账：新 function 自动 ADD，消失的 function 自动 REMOVE，同一 BDF 的 vendor/device 改变则先 REMOVE 再 ADD。

PCIe slot controller 或平台热插拔通知的处理者应在进程上下文调用 `pci_rescan()`，而非在 IRQ 中扫描 ECAM。目前通用 PCI 层不假定所有平台都有统一的 PCIe hot-plug controller，因此不自行高频轮询完整 ECAM 范围。

VirtIO-MMIO 与固定 platform device 通常不是物理可热插拔总线；若平台确实支持设备动态出现，平台枚举器应创建长期有效的 `device_t` 和资源数组后调用 `device_hotplug`。

## 驱动编写要求

热插拔正确性取决于 `remove` 与 `probe` 对称且幂等。推荐的 `remove` 顺序：

1. 取得实例锁，将 `ready`/`removing` 置为不可用，拒绝新 I/O。
2. 在设备侧屏蔽中断，注销 IRQ，取消或有界等待进行中的请求。
3. 停止 DMA 和队列，解除 class 或子系统注册。
4. 释放 DMA、MMIO 映射及私有内存，清空 `dev->drv_priv`。

不要在 `remove` 后使用 `dev`、`dev->plat_data` 或 `drv_priv` 保存的指针。总线拥有 `device_t`、资源和 `plat_data`；它可以在 REMOVE 返回后立即销毁这些对象。反过来，驱动拥有 `drv_priv`，核心只会清空它，不能替驱动释放其中资源。

对 USB class driver，所有 URB completion 都必须先检查实例是否仍处于 ready 状态。对 PCI/平台驱动，DMA completion 和 IRQ top half 必须能和 remove 并发发生，且不会在资源释放后访问寄存器或 ring。

可加载 `.a20drv` 模块的 `DriverEntry` 仍然只注册 `driver_t`。模块在设备不存在时保持已注册状态；以后设备热插入时，`device_hotplug(ADD)` 会自动匹配并调用它的 `probe`。当前已成功注册的模块会被 pin，不支持运行期卸载或替换模块本身；热插拔针对设备实例，不等同于模块热更新。

## 验证建议

至少验证以下序列：

1. 设备缺席启动，插入后确认枚举、匹配、`probe` 和 devfs/class 发布。
2. 已打开或有请求进行时拔出，确认新 I/O 返回 `-ENODEV` 或类约定的断开错误，不出现 UAF、DMA 到释放页或遗留 IRQ。
3. 重新插入相同设备，确认能创建新实例并再次绑定；不能依赖上次的 `drv_priv`。
4. 对 probe 的每一个失败点做一次插拔循环，确认失败不会留下 class 节点、IRQ、DMA 或注册表项。

USB 的基础验证可在 xHCI 已加载后插拔 HID 或 mass-storage 设备，检查 `[USB] port ... disconnected`、对应驱动的 remove 日志，以及再次插入后的 probe 日志。PCIe 平台还应从其 slot-change worker 调用 `pci_rescan()`，覆盖 ADD、REMOVE 与同 BDF 替换三种情况。

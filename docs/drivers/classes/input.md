# 输入子系统

A20OS 的输入路径分成三层：**设备驱动**（drvmod 模块，发布 input class 设备）、**mux 服务**（内核侧 `/dev/event0` 的 devfs 服务）、**用户接口**（EVIOCG* ioctl 面 + `struct input_event` 读取）。

## 架构

```text
virtio-input 设备 (MMIO slot / PCI 1af4:1052)
        │  vinput.drv（drvmod 模块，四架构）
        │  状态迁移 + 事件 virtqueue + IRQ + input class 设备
        ▼
input class 设备（DEV_CLASS_INPUT，统一核心发布）
        │  input_read_class_devices() 经 input_dev_ops.read 消费
        ▼
/ dev/event0 mux（kernel/drivers/input/input_mux.c）
        │  EVIOCG* ioctl + evdev 状态跟踪（键/ABS）
        ▼
用户空间（weston、evdev_stress、shell）
```

- 驱动与 mux 的职责分离：驱动只负责把硬件事件变成 `struct input_event` 填入 class ring；mux 负责聚合、ioctl 面与等待语义。任何 input class 设备（VBox xHCI HID、PCI/MMIO virtio-input）都自动并入 `/dev/event0`。
- 参考实现：`kernel/drvmod/examples/vinput.c`（驱动侧）、`kernel/drivers/input/input_mux.c`（mux 侧）、`user/svc/uinputd.c`（用户态双驻留侧）。
- 唤醒：mux 读路径在 `g_input_waiters` 上 park；模块 ISR 入队事件后经框架导出 `input_mux_wake()` 唤醒。IRQ 掩码/不可用的平台由 mux 的轮询兜底（遍历 class 设备的 `poll` 钩子，模块的 poll 顺带 drain used ring）。

## 驱动侧（vinput.drv）

`kernel/drvmod/examples/vinput.c` 是完整 virtio-input 驱动模板：

- 注册标准 `driver_t`（`bus = NULL`，`id_table` 同时匹配 virtio-mmio 协议类型 18 与 PCI `1af4:1052`/`1af4:1012`），经 `drv_driver_register` 进入统一核心后由核心重探已枚举设备；
- probe 自包含构造 `virtio_transport_t`：PCI 路径用 `pci_virtio_transport_init()`，MMIO 路径用 `drv_device_get_resource()` 取 MMIO/IRQ 资源 + 模块内 read32/write32 包装；
- 事件队列 DMA 区（desc/avail/used/events）是单次 `dma_alloc_coherent_aligned` 分配，描述符地址由 DMA handle 计算（模块内不需要 `va_to_pa`）；
- ISR 经 `request_irq`（hwapi 共享链支持 PCI INTx 共用线）注册，处理 used ring → 填充 256 项 user_ring → `input_mux_wake()`；
- class ops：`read`（drain + ring 拷贝）、`poll`（drain + 就绪）、`ioctl`（-ENOSYS，ioctl 面在 mux）；
- 日志保持 `[INPUT] virtio-input ready (irq=N)` 与 `[INPUT] event type=... code=... value=...` 格式（GUI smoke 依赖），模块内走 `drv_log`。

## 双驻留协调

udriver 白名单（`kernel/drivers/core/udriver.c` 的 `g_mmio_windows`）把 virtio-mmio slot 5 标记为 user-owned；`virtio_mmio_enumerate()` 跳过该槽，vinput.drv 因此只绑定其余 virtio-input 设备。slot 5 的完整驱动归用户态 `uinputd`（user/svc/uinputd.c，共享协议 `kernel/include/drivers/dual/`），内核侧对它的只读身份探针是 `vinput-probe.drv`。详见 [04-dual-placement](../../hybrid-kernel/04-dual-placement.md)。

## 用户接口

- `/dev/event0` 是 transport-independent evdev mux（devfs 静态节点，`g_devfs_input_ops` 在 `input_mux.c`）；
- 事件格式 `struct input_event`（64 位时间戳变体），读取按整事件返回；
- ioctl 面：EVIOCGVERSION/EVIOCGID/EVIOCGNAME/EVIOCGBIT/EVIOCGKEY/EVIOCGABS/EVIOCGRAB 等；key/ABS 状态由 mux 跟踪（跨设备聚合）；
- 非阻塞空读返回 -EAGAIN。

## 验证

- `smoke-dual-input`（riscv64）：uinputd 两次运行（user placement）与 vinput-probe（kernel placement）读到同一设备身份 + sendkey 注入按键事件；
- `evdev_stress`（`smoke-evdev-stress`）：/dev/event0 的 ioctl 面与空读语义；
- QEMU 手动验证事件流：`cat /dev/event0 &` + monitor `sendkey a` → 内核日志 `[INPUT] event type=1 code=30 value=1`（EV_KEY/KEY_A/press）与 EV_SYN；
- GUI smoke（`tools/smoke_qemu_gui.py`）：要求 `[INPUT] virtio-input ready` ×2（键盘+鼠标）与 sendkey 后事件计数增加。

# 热插拔与即插即用

A20OS 中设备在运行期出现和消失时的生命周期，以及驱动如何安全适配：这里的“即插即用”指设备被总线发现后，内核根据总线身份自动匹配并调用驱动 `probe`；它不表示设备可以跳过资源初始化或错误处理。

## 统一事件路径

所有可热插拔设备必须经过驱动核心，而不是直接调用某个驱动的 `probe` 或 `remove`：

```text
bus detects add
  -> device_hotplug(dev, BUS_EVENT_ADD)
  -> device_register
  -> bus match + optional driver match
  -> driver probe
  -> endpoint / class publication online；适用的 devfs 节点发布

bus detects remove
  -> device_hotplug(dev, BUS_EVENT_REMOVE)
  -> device_unregister
  -> optional pre-consumer prepare -> consumer/child unbind
  -> publication offline -> fallible execution stop
  -> quiesce -> active-call drain -> driver remove
     failure: keep offline binding/resources, return error for explicit retry
     success: release managed resources and dependencies
  -> bus drops its creator reference
  -> final device_put invokes release
```

`device_hotplug()` 位于 `kernel/drivers/core/driver_core.c`。ADD 成功后才调用可选的 `bus_type_t.hotplug(..., BUS_EVENT_ADD)`；失败 probe 事先选择 `device_probe_use_remove()`，或经 execution action/native queue 自动选择后，由核心执行部分初始化安全的 stop/quiesce/drain/remove；清理失败保留资源，显式注销重试。未选择的旧 probe 仍自行回滚。REMOVE 则仅在驱动 `remove` 返回 0、资源回收与注销成功后通知总线；通知期间额外保留 device 引用。非零错误继续保留注册/绑定/资源，不通知完成，调用者必须处理并显式重试。因此 bus 回调只处理总线自己的记录或事件传播，不能取代驱动生命周期。

普通注册会同步尝试匹配和 `probe`；discovery 事务期间先发布完整设备集合，事务结束后由统一 probe/retry 路径绑定。注销会同步执行 `remove`。所以只能从可睡眠的进程、worker 或调度/idle 下半部调用，绝不能直接从硬件中断处理程序调用。

## 当前总线支持

### USB

USB 是当前端到端支持的热插拔总线：

1. `usb_core_poll()` 在进程上下文按约 20 ms（50 Hz）节流执行；它由 kernel progress bridge 调用。
2. 每个 HCD 轮询事件环并读取每个 root port 的连接状态。
3. 新连接先分配端口清理对象，再经过 reset、slot/address 分配、descriptor/configuration 解析；每个 USB interface 被发布为一个 `device_t`，USB class driver 自动匹配并 probe。
4. 连接消失时先将 interface class 置 offline，再调用 HCD 的 `abort_slot`。只有返回 0 才继续 interface REMOVE；abort/remove 失败均保留已发布端口的 address、协议存储和 HCD 登记。已经成功停止的 slot 不重复 abort；失败端口在重连后的新枚举前也会重试。全部 interface 注销成功后才释放 endpoint/`usb_device_t`，独立 `device_t` 记录按引用计数延迟释放。

USB HCD 应准确实现 `port_connected` 和 `abort_slot`。`abort_slot` 必须取消该 slot 的未完成传输，只有返回 0 才确认不会再回调已释放的 `usb_urb_t` 或 interface 私有状态；失败时保留上下文供重试。`usb_core_unregister_hcd()` 返回错误，HCD/hub remove 必须传播，不能继续释放实例。部分 slot 初始化和枚举/接口发布失败同样走这一清理路径；错误返回但非零 `slot.hcd` 仍转交清理所有权，失败端口不可覆盖。扫描先重试停止，确认成功后才允许重新 reset；扫描对同一连接的三次枚举预算跨清理重试保留，物理断开后重置。xHCI 自身 start/probe 失败已接入可失败提前准备：先确认控制器停止，再允许核心解绑子接口；已停的控制器只做 slot 软件回收。未知 slot 仍不允许单独猜测完成，USB scan/enumerate/unregister 已由原生生命周期协调器保护，HID 进度忙时跳过；xHCI 锁外回调用计数保留，待清理端口阻止同一物理控制器编号复用。HID/hub 独立退出也已接入 Stop/Drop endpoint 及回调排空，可在根控制器运行时重绑；storage、其余 HCD 和错误阶段需要各自协议。详见 [部分枚举所有权](native-runtime.md#部分-usb-枚举的所有权交还)。

### PCI

PCI 枚举器维护以 BDF（bus/device/function）为身份的稳定 `device_t` 记录。`pci_rescan()` 将当前 ECAM 配置空间与已发布设备集合对账：新 function 自动 ADD，消失的 function 自动 REMOVE，同一 BDF 的 vendor/device 改变则先 REMOVE 再 ADD。REMOVE 失败时保留旧记录和占用，不将槽标为空闲或发布新身份；修复停止条件后再次 rescan 可重试。

PCIe slot controller 或平台热插拔通知的处理者应在进程上下文调用 `pci_rescan()`，而非在 IRQ 中扫描 ECAM。目前通用 PCI 层不假定所有平台都有统一的 PCIe hot-plug controller，因此不自行高频轮询完整 ECAM 范围。

VirtIO-MMIO 与固定 platform device 通常不是物理可热插拔总线；若平台确实支持设备动态出现，平台枚举器应创建长期有效的 `device_t` 和资源数组后调用 `device_hotplug`。

## 驱动编写要求

热插拔复用[核心生命周期](core-model.md#生命周期的精确语义)。probe 在启动硬件前建立部分实例、managed rollback 和停止所有权；pure synchronous driver 可没有 execution action，异步 producer 用 native queue 或 device_add_execution_action。

1. consumer 借用的硬件存储需要更早保护时，用 pre-consumer prepare 冻结；USB slot/endpoint 有自身协议，不能只依赖一般 call drain。
2. consumer/child 退出后 publication offline，execution stop 关闭提交并确认 DMA、IRQ 与后台回调停止，安全退休请求；错误直接返回，保留绑定供重试。
3. 可选 quiesce 之后，核心排空已进入的 CPU 调用；remove 再交还剩余资源/子系统登记，成功后才回收 devres。
4. 注册表成员关系、device 引用、offline 和 fence terminal 均不能单独证明硬件已停。停止/失效失败不能释放页面或复用同一硬件身份。

总线检查 REMOVE 返回值，失败不丢弃记录、协议存储或地址占用。成功后也不直接 free 仍有引用的 device；最终 device_put 执行 release，解绑后清除已失效的借用指针。IRQ/URB producer 停止后，还须确认在途回调已返回；在回调中只检查 ready 不能代替代码/context 和 request 的寿命保证。

可加载 `.a20drv` 模块的 `DriverEntry` 仍然只注册 `driver_t`。模块在设备不存在时保持已注册状态；以后设备热插入时，`device_hotplug(ADD)` 会自动匹配并调用它的 `probe`。当前已成功注册的模块会被 pin，不支持运行期卸载或替换模块本身；热插拔针对设备实例，不等同于模块热更新。

## 块设备身份预算

块类每次成功绑定取得独立索引，单次启动最多 256 个身份，撤销后不循环复用。借用的 block adapter 使用同样的有界预算；解绑后释放其设备和类引用，旧 adapter 保持离线并返回 `-ENODEV`。旧文件系统或 UXFS 服务保存的磁盘编号不能指向后来插入的另一块盘。预算耗尽时新绑定明确失败。

## 验证建议

至少验证以下序列：

1. 设备缺席启动，插入后确认枚举、匹配、`probe` 和 devfs/class 发布。
2. 已打开或有请求进行时拔出，确认新 I/O 返回 `-ENODEV` 或类约定的断开错误，不出现 UAF、DMA 到释放页或遗留 IRQ。
3. 重新插入相同设备，确认能创建新实例并再次绑定；不能依赖上次的 `drv_priv`。
4. 注入初始化、停止和失效错误：清理成功才回收；停止未确认时保留离线实例/IRQ/DMA/记录，修复后重试。逻辑注销/重绑、设备模型 hot-unplug 与物理突然拔除分别记录，不相互替代。

USB 的基础验证可在 xHCI 已加载后插拔 HID 或 mass-storage 设备，检查 `[USB] port ... disconnected`、对应驱动的 remove 日志，以及再次插入后的 probe 日志。PCIe 平台还应从其 slot-change worker 调用 `pci_rescan()`，覆盖 ADD、REMOVE 与同 BDF 替换三种情况。

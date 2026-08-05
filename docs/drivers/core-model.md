# 核心设备与驱动模型

这份文档说明 `device_t`、`driver_t`、`bus_type_t` 三个对象如何配合，帮助你掌握 probe/remove 的完整回滚流程，并拿到一份可直接编译的 PCI 网络驱动骨架。具体声明以 `kernel/drivers/core/driver_core.h`、`driver_class.h`、`driver_register.h` 为准。

## 三个核心对象

`device_t` 代表一个硬件实例，由总线或板级枚举器创建。创建者拥有 `device_t` 本身、设备名、资源数组和 `plat_data` 的存储，这些内存必须活到 `device_unregister()` 完成。不要在枚举函数栈上分配它们。

`driver_t` 代表一种驱动实现，通常是一个文件内的静态对象。一份驱动可以绑定多个设备。`class_type` 和 `class_ops` 向块层、网络栈、输入层、显示层或音频层提供稳定接口。

`bus_type_t` 决定某个 `device_t` 是否匹配某个 `driver_t`。PCI、VirtIO-MMIO 和 platform bus 已有公共实现。固定设备由板级文件填写 `platform_device_t` 的 ID 与资源并调用 `platform_device_register()`，不应再为每个设备定义私有 bus。

## 四层边界

设备支持按以下四层组织。上层只能依赖下层公开的能力，不能跨层读取板级常量：

| 层 | 负责内容 | 不负责内容 |
|---|---|---|
| CPU 架构层 | MMIO/port I/O 原语、内存屏障、cache sync、虚实地址转换、异常与中断入口 | PCI class、NVMe queue、HDA widget 等设备协议 |
| 平台/board 层 | RAM 与设备窗口、ECAM 地址和 bus 范围、IRQ controller/路由、固定 platform device 资源 | 解析通用 PCI function 或实现协议 I/O |
| 总线层 | 枚举 function、读取 ID、BAR sizing/assignment、资源发布和第一阶段 ID 匹配 | 控制器复位、命令队列和 class 语义 |
| 协议驱动层 | 通过公共 PCI/MMIO/DMA/IRQ API 实现寄存器协议、probe/remove 和 class ops | 硬编码 ECAM、物理 BAR 窗口、页表偏移或 CPU 指令 |

因此 NVMe 和 HDA 属于 PCI 协议驱动：源码不应由 `CONFIG_X86_64` 包围。它们能否在某个平台实际工作，取决于该平台是否已经提供可访问的 ECAM、BAR 资源、DMA 地址转换和必要的 cache 一致性。PC Speaker 直接依赖 x86 port I/O 与 PIT，保留架构门禁是正确的。

“架构无关”不等于“所有平台已可运行”。驱动在某架构成功编译，只证明没有静态架构依赖；必须在该平台观察到枚举、绑定和真实 DMA/I/O 完成后，才能标记运行验证。

probe 成功后，核心自动创建一个 `class_device_t`。该对象负责稳定 class 编号、devt、devfs/sysfs 发布和断开状态。驱动不得自行创建硬件节点。当前 char、block、audio 分别自动发布为 `/dev/charN`、`/dev/diskN`、`/dev/audioN`；所有 class 都出现在 `/sys/class/<class>/`。

绑定后的关系如下：

```text
device_t                         driver_tname                             namebus ---------------------------> busdrv ---------------------------> driverdrv_priv ----> per-device state class_ops ----> typed ops tableplat_data ---> bus-owned datares[] ------> MMIO/IRQ/DMA/MEM resourcesmatched_id --> matched id_table entry
```

`plat_data` 属于总线或板级代码，驱动不得释放。`drv_priv` 属于驱动，probe 时创建或选定，remove 时清理。类操作只接收 `device_t *`，通过 `dev->drv_priv` 找到实例。remove 前核心先把 class device 标记为 offline，阻止新调用，并等待已经进入驱动的 class 调用退出；已打开的文件随后返回 `-ENODEV`。

## 数据结构字段

### `device_id_t`

```c
typedef struct device_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint64_t driver_data;
} device_id_t;
```

ID 表以 `{ 0 }` 结束。PCI 驱动对不限制的子系统字段必须写 `VENDOR_ANY` 和 `DEVICE_ANY`，不能省略为零。匹配成功后，总线把 `dev->matched_id` 指向对应项，驱动可以读取 `driver_data` 选择硬件变体。

### `resource_t`

资源类型包括 `RES_IRQ`、`RES_MMIO`、`RES_DMA`、`RES_MEM`。`start` 和 `end` 都包含在范围内，所以长度计算为：

```c
uint64_t size = res->end - res->start + 1;
```

先验证 `end >= start`，再计算长度。通过 `device_get_resource(dev, type, index)` 按类型取资源；PCI 的物理 BAR 编号必须用 `pci_get_bar_resource(dev, bar)`，因为 64 位 BAR 会让资源数组压缩，不能假定 BAR2 等于 `RES_MMIO` 索引 2。

### `driver_t`

`probe`、`remove`、`suspend`、`resume` 是生命周期回调。`class_ops` 的真实类型由 `class_type` 决定。内建驱动的 `module` 必须为 `NULL`，该字段由核心保留，不属于当前驱动开发接口。

匹配分两阶段完成：总线 `bus_type_t.match(dev, drv)` 先按 transport identity 匹配并设置 `dev->matched_id`；可选的 `driver_t.match(dev)` 再按协议字段缩小范围。例如 HDA 检查 `04:03:00`，NVMe 检查 `01:08:02`。第二阶段只能读取已经枚举的身份信息，不得访问设备寄存器、分配 BAR/DMA 或改变硬件；拒绝时核心清除 `matched_id`，不会调用 probe。

宽泛 PCI class 驱动使用 ANY ID 表加 `.match`，不能把 class 检查只放在 probe 中。精确 vendor/device 驱动通常不需要 `.match`。

## 注册与链接

驱动文件必须包含：

```c
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
```

然后在文件末尾使用：

```c
DRIVER_REGISTER(example_driver);
```

宏把 `driver_t *` 放进 `.driver_init` 链接段。各架构/平台链接脚本以 `KEEP(*(.driver_init))` 保留它，`driver_core_init()` 启动时遍历并注册。不要手动从 `kernel_main` 调用 probe，也不要自行修改链接表。

Makefile 对 full profile 使用 `kernel/drivers/<class>/*.c` 通配，所以放入已有类目录后通常不需要追加源文件。新增子目录或 MCU profile 文件必须显式更新 Makefile。

## 生命周期的精确语义

```text
UNINIT --match--> probe
                    | 0
                    v
                  PROBED + class_device online
                    |
        remove/device_unregister/driver_unregister
                    v
          REMOVING -> class_device offline -> REMOVED

probe < 0 -> core 清空 drv、drv_priv，状态回到 UNINIT
```

核心只会清空指针，不知道驱动已经申请了哪些硬件资源。因此 probe 失败前的所有回滚由驱动负责。`remove` 在 driver registry spinlock 之外调用，可以释放 IRQ/DMA，也可以执行有界等待，但仍应尽快停止设备。

### 推荐 probe 顺序

1. 验证 `dev`、`dev->bus`、`matched_id` 和必需资源。
2. 选择并清零一个实例槽，初始化锁，但暂不设置全局 ready 标志。
3. 映射/启用总线资源；PCI 驱动调用 `pci_enable_and_assign_bars()`。
4. 复位硬件并验证版本/能力。
5. 分配 DMA、队列和普通内存。
6. 建立 descriptor/ring，进行 feature negotiation。
7. 清除旧中断状态；注册 IRQ 或明确选择轮询模式。
8. 启动设备并执行最小健康检查。
9. 设置 `dev->drv_priv`。
10. 如有类级 registry（例如 display），注册后再设置实例 ready；最后返回 `0`。

### 推荐 remove 顺序

1. 在实例锁下把 `removing/ready` 置为不可用，阻止新 I/O。
2. 屏蔽设备中断，再调用 `free_irq()`。
3. 停止队列/DMA；必要时等待当前请求有界结束。
4. 从 display 等类级 registry 注销。
5. 释放 DMA、映射和普通内存。
6. 清空 `dev->drv_priv` 和实例槽。核心随后再次清空绑定字段，因此清理必须幂等。

> 不要这样做
> 在枚举函数栈上创建 `device_t` 或 `resource_t`；从 `kernel_main` 手动调用 `probe`；把所有错误返回裸 `-1`；或者在同一平台已经接入总线模型后，又通过 `arch_virtio_*_probe()` 扫描 PCI 并重新分配 BAR。这些做法都会破坏已绑定驱动的 MMIO/notify 地址，导致设备 status 归零、queue 停止。

## 可编译的 PCI 网络驱动骨架

以下骨架展示结构，不包含具体寄存器协议：

```c
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"

typedef struct {
    uintptr_t regs;
    spinlock_t lock;
    uint8_t mac[6];
    int ready;
} example_priv_t;

static example_priv_t g_example; /* 多实例驱动应改为槽数组或动态分配 */

static int example_send(device_t *dev, const void *data, size_t len) {
    example_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->ready || !data || !len)
        return -EINVAL;
    /* 提交成功必须返回 len，队列满返回 -EAGAIN。 */
    return (int)len;
}

static int example_recv(device_t *dev, void *data, size_t capacity) {
    example_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->ready || !data || !capacity)
        return -EINVAL;
    return 0; /* 当前网络类约定：无包返回 0。 */
}

static const uint8_t *example_mac(device_t *dev) {
    example_priv_t *p = dev ? dev->drv_priv : NULL;
    return p && p->ready ? p->mac : NULL;
}

static const net_dev_ops_t example_ops = {
    .send = example_send,
    .recv = example_recv,
    .mac = example_mac,
};

static int example_probe(device_t *dev) {
    int ret = pci_enable_and_assign_bars(dev);
    if (ret < 0)
        return ret;
    resource_t *bar0 = pci_get_bar_resource(dev, 0);
    if (!bar0 || bar0->end < bar0->start ||
        bar0->end - bar0->start + 1 < 0x1000)
        return -ENODEV;

    example_priv_t *p = &g_example;
    memset(p, 0, sizeof(*p));
    spin_init(&p->lock);
    p->regs = (uintptr_t)bar0->start;
    /* reset, identify, allocate DMA, initialize rings */
    dev->drv_priv = p;
    p->ready = 1;
    return 0;
}

static int example_remove(device_t *dev) {
    example_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p)
        return 0;
    p->ready = 0;
    /* mask IRQ, stop DMA, free_irq, dma_free_coherent */
    dev->drv_priv = NULL;
    memset(p, 0, sizeof(*p));
    return 0;
}

static const device_id_t example_ids[] = {
    { .vendor = VENDOR_ANY, .device = DEVICE_ANY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static int example_match(device_t *dev) {
    return pci_class_code(dev) == 0x020000U;
}

static driver_t example_driver = {
    .name = "example-net",
    .id_table = example_ids,
    .bus = &pci_bus,
    .match = example_match,
    .probe = example_probe,
    .remove = example_remove,
    .class_ops = &example_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(example_driver);
```

## 返回错误

驱动内部包含 `core/errno.h`，返回负 errno。常用值为 `-EINVAL`（参数或范围）、`-ENODEV`（硬件/资源不匹配）、`-ENOMEM`（分配失败）、`-EAGAIN`（暂时无资源）、`-ETIMEDOUT`（有界等待到期）、`-EIO`（硬件或协议错误）、`-EOPNOTSUPP`（协商能力不支持）。新驱动不得用裸 `-1` 覆盖所有错误。

生命周期操作成功返回 `0`。I/O 操作的成功返回值由设备类规定，不得自行选择。块类 `read/write` 成功返回 `0`，表示请求中的全部扇区已经完成；它们不是返回完成扇区数。

注册函数会拒绝不完整对象（`-EINVAL`）和同一指针的重复注册（`-EEXIST`）。注册 driver/device 可能同步执行 `probe`，注销可能同步执行 `remove`，所以调用者在函数返回前必须保持对象及其资源有效。核心以可睡眠操作锁串行化注册、probe、remove 和注销；生命周期回调不得递归调用这些注册 API，也不得从中断上下文调用。registry spinlock 只保护数组元数据，不跨任何驱动回调。

## 唯一枚举所有权

每个硬件 function 只能由一个 bus enumerator 创建并配置一次。路径是：

`board.enumerate_devices -> bus enumerator -> device_register -> driver probe`

class 消费者只能通过 `device_find_by_class()` 或类适配层使用已绑定设备。禁止在 mount、network init、devfs open 或兼容 getter 中再次调用架构私有扫描器。

尤其禁止“统一 PCI 枚举已完成，但 `arch_virtio_*_probe()` 又扫描 PCI 并重新分配 BAR”的双路径。重新写 BAR 会使已绑定驱动保存的 MMIO/notify 地址失效，表现为设备 status 归零、queue 停止。arch probe hook 只允许服务尚未接入 bus model 的平台；同一平台一旦由 bus model 枚举，该 hook 必须删除或不可达。

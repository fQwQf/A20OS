# 核心设备与驱动模型

`device_t`、`driver_t`、`bus_type_t` 三个对象如何配合，probe/remove 的完整回滚流程，以及一份可直接编译的 PCI 网络驱动骨架。具体声明以 `kernel/drivers/core/driver_core.h`、`driver_class.h`、`driver_register.h` 为准。

## 三个核心对象

`device_t` 代表一个硬件实例，由总线或板级枚举器创建。创建者拥有设备名、资源数组和 `plat_data` 的存储。注册表、父子关系、依赖和进度快照会持有设备引用；动态对象通过 `release()` 在最终 `device_put()` 时回收，不能在 `device_unregister()` 返回时直接释放。不要在枚举函数栈上分配设备描述。

`driver_t` 代表一种驱动实现，通常是一个文件内的静态对象。一份驱动可以绑定多个设备。`class_type` 和 `class_ops` 向块层、网络栈、输入层、显示层或音频层提供稳定接口。

`bus_type_t` 决定某个 `device_t` 是否匹配某个 `driver_t`。PCI、VirtIO-MMIO 和 platform bus 已有公共实现。固件设备通过统一 `fw_node_t` 枚举器发布；没有 DT/ACPI 的固定硬件使用静态 provider。原生总线负责自身子设备，不应为每个设备定义私有 bus。启动与完整发现契约见 [启动模型](../../dev/boot-model.md)。

## 四层边界

设备支持按以下四层组织。上层只能依赖下层公开的能力，不能跨层读取板级常量：

| 层 | 负责内容 | 不负责内容 |
|---|---|---|
| CPU 架构层 | MMIO/port I/O 原语、内存屏障、cache sync、虚实地址转换、异常与中断入口 | PCI class、NVMe queue、HDA widget 等设备协议 |
| 平台/board 层 | RAM 与设备窗口、ECAM 地址和 bus 范围、IRQ controller/路由、固定 platform device 资源 | 解析通用 PCI function 或实现协议 I/O |
| 总线层 | 枚举 function、读取 ID、BAR sizing/assignment、资源发布和第一阶段 ID 匹配 | 控制器复位、命令队列和 class 语义 |
| 协议驱动层 | 通过公共 PCI/MMIO/DMA/IRQ API 实现寄存器协议、probe/remove 和 class ops | 硬编码 ECAM、物理 BAR 窗口、页表偏移或 CPU 指令 |

因此 NVMe 和 HDA 属于 PCI 协议驱动：源码不应由 `CONFIG_X86_64` 包围。它们能否在某个平台实际工作，取决于该平台是否已经提供可访问的 ECAM、BAR 资源、DMA 地址转换和必要的 cache 一致性。PC Speaker 消费端口 I/O 能力和 PIT 协议；端口指令由架构后端实现，设备可用性由 ACPI 或静态硬件描述决定。

“架构无关”不等于“所有平台已可运行”。驱动在某架构成功编译，只证明没有静态架构依赖；必须在该平台观察到枚举、绑定和真实 DMA/I/O 完成后，才能标记运行验证。

probe 成功后，核心自动创建一个 `class_device_t`。该对象负责稳定 class 编号、devt、devfs/sysfs 发布和断开状态。驱动不得自行创建硬件节点。当前 char、block、audio 分别自动发布为 `/dev/charN`、`/dev/diskN`、`/dev/audioN`；所有 class 都出现在 `/sys/class/<class>/`。

绑定后的关系如下：

```text
device_t                         driver_t
name                             name
bus ---------------------------> bus
drv ---------------------------> driver
drv_priv ----> per-device state  class_ops ----> typed ops table
plat_data ---> bus-owned data
res[] ------> MMIO/IRQ/DMA/MEM resources
matched_id --> matched id_table entry
```

`plat_data` 属于总线或板级代码，驱动不得释放。`drv_priv` 属于驱动，probe 时创建或选定，remove 时清理。类操作只接收 `device_t *`，通过 `dev->drv_priv` 找到实例。解除绑定时，核心先标记 class device offline，执行可选的 `quiesce` 停止新提交并结束软件请求，再等待已进入驱动的 class 调用退出，最后调用可失败的 `remove`：仅返回 0 后才释放受管存储、依赖和绑定；非零保留 `REMOVING` 绑定及注册引用，旧 class 句柄保持离线，调用者显式重试。已打开的文件随后返回 `-ENODEV`。原生异步请求的完整契约见 [原生驱动运行时](native-runtime.md)。

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

资源类型包括 `RES_IRQ`、`RES_MMIO`、`RES_IOPORT`、`RES_DMA`、`RES_MEM`。`RES_MMIO` 始终存 CPU 物理地址，通过 `device_map_mmio()`、`device_map_resource()` 或 `pci_map_bar()` 取得映射；禁止直接解引用 `start`。`start` 和 `end` 都包含在范围内，所以长度计算为：

```c
uint64_t size = res->end - res->start + 1;
```

先验证 `end >= start`，再计算长度。通过 `device_get_resource(dev, type, index)` 按类型取资源；PCI 的物理 BAR 编号必须用 `pci_get_bar_resource(dev, bar)`，因为 64 位 BAR 会让资源数组压缩，不能假定 BAR2 等于 `RES_MMIO` 索引 2。

受管 MMIO 映射现在同时申请设备独占的地址区间；`device_map_mmio()`、`device_map_resource()` 和 `pci_map_bar()` 使用同一路径。不同设备的重叠申请失败，同一设备可重复申请，各自保留独立生命周期。需要区分冲突与分配错误时用 `device_map_resource_exclusive(dev, res, &mapping)`，失败清空输出；资源必须属于 `dev->res`。映射回收先执行 architecture unmap，再释放区间占用。

`device_claim_resource(dev, res)` 可仅申请受管 MMIO/port I/O 占用；`driver_resource_request()` / `driver_resource_release()` 则提供显式句柄，适合需要跨 unbind 保留资源的恢复路径。显式句柄保留设备引用但不保留绑定，须由调用者管理 stopping 状态和释放时机。占用登记只覆盖使用这些 API 的访问者。hosted 启动还以独立存储对象保护受管 RAM 与 ACPI 表页；其余固件/RAM 和 PCI window 预留尚未完整导入；完整契约见 [原生驱动运行时](native-runtime.md)。

物理存储使用独立的 `driver_memory_reserve()` / `driver_memory_release()` 句柄，复制区间并跨设备解绑保留。它阻止基本/checked MMIO claim、受管映射和冲突设备发布，不能通过同 owner 绕过；不为 PFA 分配或预留页面，也不承担 DMA 隔离。详细语义见 [物理存储保留对象](native-runtime.md#物理存储保留对象)。

需要核对未绑定设备的地址声明时，使用 `driver_resource_request_checked()`：owner 必须已注册，核心在生命周期锁内核对其他已发布设备的 MMIO/`RES_MEM`/port 声明及 active claims，再建立句柄。句柄存续时，新设备不能发布冲突声明；owner 注销后保留的句柄仍然生效。此接口不推断 parent/window 的共享例外，资源归属歧义须显式解决。

`device_is_registered(dev)` 仅观察核心注册表成员关系，不增加引用，也不表示驱动在线。失败解绑仍保留注册关系，但 class 已离线；持有 device 引用只保护对象存储，不能绕过注销；需要把检查与后续操作协调时，调用者须持有生命周期事务。

PCI 作用域通过 `pci_device_scope_get()` 返回带引用的 client/上游桥观察结果，不另建 Linux group 或隐式修改设备 parent。身份替换、窗口重叠、缺失桥和循环不会被当成有效路径；device 引用不固定硬件或绑定。详见 [PCI 桥路径与 AMD Range 补充查询](native-runtime.md#pci-桥路径与-amd-range-补充查询)。

PCI 的 `pci_requester_reserve/release` 显式复制并原子占用 requester 集合，同 owner 无共享例外；owner 注销或 BDF 替换后仍保留，直至显式释放。它不推断 alias 或证明隔离，见 [requester 集合占用](native-runtime.md#pci-requester-集合占用)。

AMD-Vi 保留路由通过 `amd_iommu_route_acquire/validate/requesters/reserve/memory/release` 显式组合固件策略、PCI 路径与控制器上下文租约。仅 acquire 是观察，显式 requesters 查询完整固件声明集合，reserve 才在全部成员已发布、路径有效、内核所有且 BME 关闭后原子占用，并保留各成员身份/anchor。成员失效不自动释放占用；各驱动停止 DMA 仍由调用者协调。解绑使旧路由失效，占用随 reserve 的句柄保留，控制器资源保留到最后一份路由释放。它不改变 parent/dependency，也不授予 DMA。详见 [保留路由对象](native-runtime.md#保留路由对象与控制器生命周期)。

RISC-V 的原生和用户 DMA domain 已共同接入上述 PCI requester 登记，在发布硬件上下文前取得占用；失效失败保留设备、页表和占用。原生 shutdown 确认 detach 后，占用仍随 managed context 保留至最后 put，在映射锁之外释放。该后端只声明一个 RID，不推断 alias。

DMA 内存约束使用独立的 `driver_dma_policy` 不可变对象，不借用 IVRS 或设备上下文。带 policy 的 managed domain 在整批 frame pin/provider 调用前检查物理权限，并将 Unity 地址违规纳入可失败回滚；引用随清理和 shutdown 保留。AMD 的 route policy 转换不授予硬件启用权限。见 [物理约束](native-runtime.md#不可变物理约束与映射入口检查)。

总线修改地址前，须在 `driver_discovery_begin/end` 事务内查询资源并完成硬件和元数据更新。PCI BAR 分配已接入该事务：分配新地址跳过声明和占用区间，已有 BAR 在 reachability/mapping hook 前检查 active claims 与受保护存储；全部 BAR 规划通过后才写入新地址。调用者仍须先停止设备，配置写入失败/硬件消失的恢复不由此接口保证。

### `driver_t`

platform 驱动通过 `firmware_ids` 匹配 DT compatible、ACPI HID/CID 或静态 hardware-id，匹配项保存在 `firmware_match`；总线私有的数字 ID 保存在 `matched_id`。`probe`、`remove`、`suspend`、`resume` 是生命周期回调。新驱动可在硬件初始化前显式 `device_probe_use_remove()`，让失败 probe 使用可失败 remove 管线；部分初始化回调、保留/重试与 PROBE_CLEANUP 状态见 [原生运行时](native-runtime.md#显式选择失败-probe-的清理所有权)。remove 失败必须保留实例和硬件可达存储；`device_unregister()`/`driver_unregister()` 返回错误，不能忽略后继续释放设备或模块。quiesce 每次退出只执行一次，之后仅重试 remove。需要先冻结控制器再释放 child 存储的驱动可显式登记 `device_add_remove_prepare()`；PREPARING 保留图和资源，成功的准备动作会被记住，详见 [提前准备协议](native-runtime.md#在-consumer-解绑前准备退出)。USB HID/hub 在 remove 中进一步逐项交还 endpoint，失败保留 URB 和绑定；当前 USB vtable 扩展需要模块 ABI 10。总线私有拓扑通过 `driver_lifecycle_begin/end` 与核心绑定协调，进度使用 `driver_lifecycle_try_begin` 忙时返回；普通协调不抑制 probe，也不固定对象引用，见 [拓扑协调](native-runtime.md#总线拓扑的原生生命周期协调)。`class_ops` 的真实类型由 `class_type` 决定。内建驱动的 `module` 必须为 `NULL`，该字段由核心保留，不属于当前驱动开发接口。

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

Makefile 不通配收集 full profile 的设备驱动。新增源码必须进入 `tools/mk/driver-sources.mk` 的适用显式账本；generic 包还必须在 `tools/mk/driver-modules.mk` 增加模块包装规则和适用架构的 `DRVMOD_MODULES` 项。MCU 的 `BOARD_DRIVER_DIR` 仍按板目录通配，是独立例外。

## 生命周期的精确语义

```text
UNINIT --match--> probe
                    | 0
                    v
                  PROBED + class_device online
                    |
        remove/device_unregister/driver_unregister
                    v
          REMOVING -> class offline -> quiesce -> active calls drain
                    -> remove -> managed resources release -> REMOVED

probe < 0 -> core 清空 drv、drv_priv，状态回到 UNINIT
```

核心在 probe 失败和 remove 后逆序执行受管资源动作并清理依赖；MMIO 映射 helper 自动登记回收。驱动必须回滚未托管的分配，停止 DMA 并撤销 IRQ。返回 `-EPROBE_DEFER` 表示 provider 尚未就绪；使用 `device_add_dependency()` 建立依赖后，核心负责有界重试及 provider 撤销时先解除 consumer 绑定。`quiesce/remove` 在 driver registry spinlock 之外调用，但仍持有 lifecycle mutex，不得等待需要该 mutex 的操作。成功 probe 后类发布失败也会调用 quiesce/remove；失败的 probe 自行停止已启动的硬件。

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

### 推荐 quiesce 与 remove 顺序

1. quiesce 在实例锁下设置 stopping 状态，禁止新提交；核心此前已拒绝新的 class 调用。
2. 排空队列，必要时停止/复位 DMA。确认硬件不再使用请求缓冲区后，使未完成请求成功或以错误结束，唤醒等待者。
3. 同步中断并取消后台工作。可以在 quiesce 中释放 IRQ，必须保留仍在途调用可能访问的实例存储。
4. 核心等待在途 class 调用结束，然后进入 remove。
5. remove 注销 display 等私有 registry，解除 DMA mapping 并释放未托管存储；受管映射及动作随后由核心回收。
6. 清空 `dev->drv_priv` 和实例槽。核心随后再次清空绑定字段。

只有同步有界操作的驱动可以保持 quiesce 为空；需要取消等待的驱动不能将全部停止逻辑留到 remove。

> 不要在枚举函数栈上创建 `device_t` 或 `resource_t`；不要从 `kernel_main` 手动调用 `probe`；不要把所有错误返回裸 `-1`；也不要在同一平台已经接入总线模型后，又通过 `arch_virtio_*_probe()` 扫描 PCI 并重新分配 BAR。这些做法都会破坏已绑定驱动的 MMIO/notify 地址，导致设备 status 归零、queue 停止。

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
    /* 提交成功必须返回 len，队列满返回 -EAGAIN。*/
    return (int)len;
}

static int example_recv(device_t *dev, void *data, size_t capacity) {
    example_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->ready || !data || !capacity)
        return -EINVAL;
    return 0; /* 当前网络类约定：无包返回 0。*/
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
    p->regs = (uintptr_t)pci_map_bar(dev, 0);
    if (!p->regs)
        return -ENODEV;
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

运行期发现或拔除设备时，bus/平台必须调用 `device_hotplug(dev, BUS_EVENT_ADD)` 或 `device_hotplug(dev, BUS_EVENT_REMOVE)`，不能绕过核心直接调用驱动回调。它把设备事件接入上述同一套注册、匹配、class offline 和 remove 路径；详情见[热插拔与即插即用](hotplug-and-pnp.md)。

## 唯一枚举所有权

每个硬件 function 只能由一个 bus enumerator 创建并配置一次。路径是：

`firmware_device_enumerate / board.enumerate_buses -> device_register -> driver probe`

class 消费者只能通过 `device_find_get_by_class()` 或类适配层使用已绑定设备。禁止在 mount、network init、devfs open 或兼容 getter 中再次调用架构私有扫描器。

尤其禁止“统一 PCI 枚举已完成，但 `arch_virtio_*_probe()` 又扫描 PCI 并重新分配 BAR”的双路径。重新写 BAR 会使已绑定驱动保存的 MMIO/notify 地址失效，表现为设备 status 归零、queue 停止。当前活跃平台已移除这些 arch probe hook，设备统一由固件与原生总线枚举器发布。

## 模块化驱动注册

可加载驱动模块（drvmod）复用同一套统一核心：模块的 `DriverEntry` 调用框架导出 `drv_driver_register(&driver_t)` 后，核心按与内建驱动相同的匹配/重探路径对已枚举设备执行 probe（`driver_register` 会同步重探现有未绑定设备），`class_ops`/`class_type` 照常发布 class 设备。

模块化驱动的 `driver_t` 与内建完全同构（`bus`/`id_table`/`probe`/`remove`/`class_ops`/`class_type`），因此同一份驱动代码既可内建也可模块化；迁移时的差异仅在入口（`DriverEntry` + `drv_driver_register` 替代 `DRIVER_REGISTER`）与内核 API 依赖面（模块只能引用框架导出表，见 [可安装内核驱动](kernel-modules.md)）。参考实现：`nvme.a20drv`（PCI class）、`hda.a20drv`（PCI class）、`vinput.a20drv`（NULL-bus id_table 同时匹配 MMIO 与 PCI）。

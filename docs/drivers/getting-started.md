# 从零开发第一个 A20OS 驱动

这份教程以 PCI 网卡 `acme-net` 为例，带你从硬件资料走到一个可编译、可注册、可被 lwIP 使用的驱动。固定 SoC 设备把 PCI 发现换成平台资源枚举；块、输入、显示设备只替换类操作表。

## 1. 准备工作

### 编译环境

A20OS 是 freestanding C99 内核，没有宿主 libc。驱动只能调用内核已有接口。准备 Git、GNU Make 和对应架构的 GCC/binutils；制作镜像还需要文件系统或引导工具。

先只构建内核，把镜像依赖和驱动问题分开：

```sh
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=both kernel-only -j4
```

目标平台不是 AArch64 时替换 `ARCH` 和 `BOARD`。已有平台命令见 [构建、测试与提交](testing-and-submission.md)。在修改源码前，记录一份未修改时的成功构建日志，后续失败才能判断是不是驱动引入。

### 需要认识的目录

```text
kernel/drivers/core/                 核心对象、注册、硬件访问 APIkernel/include/drivers/bus/          PCI 等总线公共声明kernel/include/drivers/<class>/      各功能类公共声明kernel/drivers/bus/                  总线枚举和 transportkernel/drivers/block|net|gpu|input/  通用功能驱动kernel/platform/<board>/             板级地址、固件发现和固定设备kernel/include/core/errno.h          内核负 errnokernel/include/core/lock.h           spinlock APIkernel/include/mm/slab.h             kmalloc/kfree
```

核心模型头在 `kernel/drivers/core/`，包含写法是 `#include "drivers/core/driver_core.h"`。不要复制同名结构到驱动私有头。

## 2. 写一张硬件契约表

在写代码前，从数据手册、PCI ID 和目标机器枚举日志中确认以下内容。无法确认的项目不能靠试写寄存器来猜。

| 项目 | 必须确认的内容 |
|---|---|
| 身份 | bus 类型，vendor/device，必要的 subsystem ID 或 compatible |
| 寄存器 | BAR/固定地址、最小长度、访问宽度、endianness、W1C/读清副作用 |
| DMA | 地址宽度、对齐、descriptor 格式、ownership 位、最大长度、cache 一致性 |
| IRQ | 来源、触发类型、状态确认、清除顺序、是否共享 |
| 复位 | 进入 reset、完成条件、超时、失败后的安全状态 |
| 能力 | 版本、feature 位、队列数、MTU/扇区/像素格式等限制 |
| 生命周期 | probe 前状态、启动点、停止 DMA 的方法、remove 后状态 |

VirtualBox PCI 设备可从串口的 `[BUS] pci` 和 BAR 行取得身份与资源。物理设备应保留固件/PCI 枚举证据。一个驱动只匹配自己真正实现过的硬件变体。

## 3. 数据路径

```text
板级代码/总线发现硬件-> 创建并注册 device_t-> bus.match 对照 driver.id_table-> driver.probe 初始化一个实例-> driver.class_ops 发布统一功能-> VFS、lwIP、块层、input 或 framebuffer 调用 class_ops-> class op 从 dev->drv_priv 找到实例并访问硬件-> device_unregister/driver_unregister 调用 remove
```

枚举器描述“机器上有什么”，驱动描述“怎样操作这种硬件”，class 描述“内核消费者怎样使用”。通用驱动不写板级物理地址，平台代码不复制设备协议。

## 4. 选择接入方式

| 设备连接方式 | 开发者要做什么 | 不需要做什么 |
|---|---|---|
| 已枚举的 PCI/PCIe | 写 ID 表和功能驱动 | 不创建 `device_t`，不扫描 ECAM |
| 已枚举的 VirtIO-MMIO | 写 VirtIO type 驱动或扩展现有驱动 | 不硬编码 slot 地址 |
| 固定 SoC 外设 | 在 platform 枚举资源，再写功能驱动 | 不把板地址写入可复用驱动 |
| 新总线 | 写 bus 的发现/match/resource，再写子设备驱动 | 不让每个子驱动各自扫描总线 |
| LED、蜂鸣器、单值传感器 | 可以使用板级轻量 API | 不必为无复用需求强建完整模型 |

PCI 开发者直接跳到下一节。固定设备开发者还要完成 [总线与平台](bus-and-platform.md) 中的静态 `resource_t`/`device_t` 枚举模板。

## 5. 建立驱动源文件

以网络设备 `acme-net` 为例，新建 `kernel/drivers/net/acme_net.c`。full profile 通过 Makefile 通配自动编译该文件。只被本文件使用的寄存器、descriptor 和私有状态都留在 `.c`；只有其他模块需要调用的接口才放到 `kernel/include/drivers/net/`。

最小包含集合：

```c
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"    /* wait_queue_t、mutex_t */
#include "core/timer.h"   /* timer_get_ticks、MS_TO_TICKS/US_TO_TICKS */
#include "mm/slab.h"
#include "proc/proc.h"    /* park/wake 协议 */
```

驱动不直接包含 `kernel/arch/` 或某块板的头。x86 port I/O 等真正架构限定协议是例外，但应把架构操作封装成小型 HAL，而不是让完整功能驱动散布架构条件。

## 6. 定义寄存器和每实例状态

```c
#define ACME_REG_STATUS       0x0000U
#define ACME_REG_CONTROL      0x0004U
#define ACME_REG_INT_STATUS   0x0008U
#define ACME_REG_INT_ENABLE   0x000cU
#define ACME_CONTROL_ENABLE   (1U << 0)
#define ACME_STATUS_READY     (1U << 0)
#define ACME_MIN_BAR0_SIZE    0x1000U

typedef struct {
    uintptr_t regs;       /* resource 已转换好的内核 MMIO 地址 */
    spinlock_t lock;      /* 保护下面的数据面状态 */
    uint8_t mac[6];
    uint64_t ring_dma;
    void *ring;
    size_t ring_size;
    wait_queue_t waiters; /* IRQ top-half 唤醒的完成等待者，内部自锁 */
    int irq;
    int irq_registered;
    int ready;
    int removing;
} acme_priv_t;
```

所有会因设备实例不同而变化的状态都放在 `drv_priv` 指向的对象中。禁止用一组无保护的全局 queue index 支撑多个设备。若当前内存限制只能支持一个静态实例，probe 必须拒绝第二个实例，并在 [实现状态](implementation-status.md) 记录限制；优先使用 `kcalloc(1, sizeof(*p))` 创建实例。等待命令完成时使用 `wait_queue_t` + park（见第 9 节和 [标准完成模型](runtime-contracts.md#标准完成模型irq-hybrid)），不要 busy-poll 到硬件超时。

寄存器 helper 明确使用 MMIO API：

```c
static uint32_t acme_read(acme_priv_t *p, uint32_t reg){
    return readl((const volatile void *)(p->regs + reg));
}

static void acme_write(acme_priv_t *p, uint32_t reg, uint32_t value){
    writel(value, (volatile void *)(p->regs + reg));
}

static int acme_irq(int irq, void *priv){
    acme_priv_t *p = priv;
    if (!p)
        return 0;
    /* top-half：确认中断属于本设备，清中断源，唤醒等待者后立即返回。
     * 不触碰提交者拥有的 ring 状态；共享线上无 pending 时直接返回。 */
    uint32_t is = acme_read(p, ACME_REG_INT_STATUS);
    if (!is)
        return 0;
    acme_write(p, ACME_REG_INT_STATUS, is);
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_all(&p->waiters, 0, PROC_WAKE_EVENT,
                                 &wake_q, NULL);
    (void)proc_wake_q_flush(&wake_q);
    (void)irq;
    return 0;
}
```

不要直接解引用普通指针，也不要把 MMIO 地址交给 `va_to_pa()`。`readl/writel` 已提供访问顺序；只有硬件协议允许时才使用 relaxed 版本。

## 7. 定义精确的匹配表

```c
static const device_id_t acme_ids[] = {
    { .vendor = 0x1234U, .device = 0x5678U,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY,
      .driver_data = 0 },
    { 0 },
};
```

末尾 `{ 0 }` 是哨兵，不代表可匹配设备。PCI 表中未限制的 subsystem 字段必须显式写 `VENDOR_ANY/DEVICE_ANY`；零表示真实 ID 0，而不是通配。多个变体共享协议时，为每个 ID 写一项并用 `driver_data` 选择差异，禁止 probe 再靠板名猜型号。

## 8. 实现类操作

实现目标类前，先阅读 [设备类接口](device-classes.md) 中该类操作的单位和返回值。网络类的核心操作如下：

```c
static int acme_send(device_t *dev, const void *packet, size_t length){
    acme_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->ready || p->removing)
        return -ENODEV;
    if (!packet || length == 0 || length > 1514)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    /* 回收已完成项；队列满时解锁并返回 -EAGAIN。 */
    /* 复制/同步 packet，填写 descriptor，屏障后写 doorbell。 */
    spin_unlock_irqrestore(&p->lock, flags);
    return (int)length;
}

static int acme_recv(device_t *dev, void *buffer, size_t capacity){
    acme_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->ready || p->removing)
        return -ENODEV;
    if (!buffer || capacity == 0)
        return -EINVAL;
    /* 无完整帧返回 0；成功返回一帧字节数。 */
    return 0;
}

static const uint8_t *acme_mac(device_t *dev){
    acme_priv_t *p = dev ? dev->drv_priv : NULL;
    return p && p->ready && !p->removing ? p->mac : NULL;
}

static const net_dev_ops_t acme_ops = {
    .send = acme_send,
    .recv = acme_recv,
    .mac = acme_mac,
};
```

类操作接收的是内核 buffer，不是用户指针。不得调用 `copy_from_user`。网络 `send` 成功返回字节数；block `read/write` 则是整次成功返回 `0`。这是最常见的移植错误，不能用统一的“成功返回 0”套用所有类。

## 9. 实现 probe 和逆序回滚

probe 把“匹配的硬件”变成“完全可用的类实例”。任何中途失败都必须恢复为未绑定状态，且设备不能继续 DMA。下面的控制流可直接作为结构模板：

```c
static int acme_probe(device_t *dev){
    int ret;
    acme_priv_t *p = NULL;
    resource_t *bar0;

    if (!dev || !dev->matched_id)
        return -EINVAL;

    ret = pci_enable_and_assign_bars(dev);
    if (ret < 0)
        return ret;

    bar0 = pci_get_bar_resource(dev, 0);
    if (!bar0 || bar0->end < bar0->start ||
        bar0->end - bar0->start + 1U < ACME_MIN_BAR0_SIZE)
        return -ENODEV;

    p = kcalloc(1, sizeof(*p));
    if (!p)
        return -ENOMEM;
    spin_init(&p->lock);
    wait_queue_init(&p->waiters);
    p->regs = (uintptr_t)bar0->start;
    p->irq = -1;

    /* 先复位并验证版本/能力；每个等待同时用 tick 和迭代上限。 */
    acme_write(p, ACME_REG_CONTROL, 0);
    if (acme_read(p, ACME_REG_STATUS) == 0xffffffffU) {
        ret = -ENODEV;
        goto fail_priv;
    }

    p->ring_size = 4096;
    p->ring = dma_alloc_coherent(p->ring_size, &p->ring_dma);
    if (!p->ring) {
        ret = -ENOMEM;
        goto fail_priv;
    }
    /* 验证 ring_dma 满足设备地址位宽和对齐，再写 DMA 寄存器。 */

    /* IRQ 解析：PCI 用 pci_intx_irq()（平台路由后的标识）；MMIO/平台
     * 设备用 RES_IRQ 资源。PCI 配置空间的 IRQ Line 寄存器不是可用
     * 标识，禁止注册。解析失败（-1）进入轮询降级。 */
    p->irq = pci_intx_irq(dev);
    if (p->irq >= 0) {
        /* PCI INTx 可共享，必须带 IRQF_SHARED。注册失败显式降级：
         * 实例不得保持“看似 IRQ 驱动”的状态。 */
        ret = request_irq((uint32_t)p->irq, acme_irq, IRQF_SHARED, p);
        if (ret == 0) {
            p->irq_registered = 1;
        } else {
            p->irq = -1;
            ret = 0;
        }
    }

    /* 清 pending 状态，启动 RX/TX，最后才发布实例。设备级中断使能
     * 只在 handler 注册成功后打开；轮询降级时保持屏蔽。 */
    if (p->irq_registered)
        acme_write(p, ACME_REG_INT_ENABLE, 0xffffffffU);
    acme_write(p, ACME_REG_CONTROL, ACME_CONTROL_ENABLE);
    dev->drv_priv = p;
    p->ready = 1;
    return 0;

fail_priv:
    /* IRQ 注册失败是降级而非回滚；一旦注册成功，后续失败必须先
     * 屏蔽设备中断，再 free_irq，最后释放 DMA 和实例内存。 */
    kfree(p);
    return ret;
}
```

示例中的 `acme_irq` 符合 `int handler(int irq, void *priv)`。真实 handler 必须确认并清设备中断源，且不得分配、睡眠、访问 VFS 或长轮询。若硬件在 IRQ 注册前已可能触发中断，顺序应改为：屏蔽设备中断、清 pending、注册 handler、启动设备、最后解除屏蔽。

`pci_enable_and_assign_bars()` 当前没有配对的 disable helper，因此它之前的失败无需由功能驱动释放 BAR；驱动自己取得的 DMA、IRQ、类 registry 和普通内存必须全部回滚。

命令完成等待遵循 [标准完成模型](runtime-contracts.md#标准完成模型irq-hybrid)：有界预轮询窗口后 park 在 `p->waiters` 上（有界 park 块，link 后重查），IRQ top-half 唤醒；平台无中断路由时退化到有界轮询，且必须在 [实现状态](implementation-status.md) 记录适用平台与解除轮询的条件。无 IRQ 又无 `.poll` 进展入口的设备不得让 probe 成功。

 注意：probe 中如果先设置 `dev->drv_priv = p` 再启动设备，或者先释放资源再 `goto` 到错误标签，remove 或失败回滚会访问未初始化或已释放的内存。保持“取得资源 → 失败则逆序释放 → 成功最后才发布 ready/drv_priv”的顺序。

## 10. 实现 remove

```c
static int acme_remove(device_t *dev){
    acme_priv_t *p = dev ? dev->drv_priv : NULL;
    if (!p)
        return 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->removing = 1;
    p->ready = 0;
    spin_unlock_irqrestore(&p->lock, flags);

    /* 先屏蔽设备中断（或复位设备），再 free_irq，最后释放 DMA。
     * free_irq 返回时在途 handler 已退出，之后才能释放实例内存。 */
    acme_write(p, ACME_REG_INT_ENABLE, 0);
    acme_write(p, ACME_REG_CONTROL, 0);
    if (p->irq_registered)
        free_irq((uint32_t)p->irq, p);

    dma_free_coherent(p->ring, p->ring_size, p->ring_dma);
    dev->drv_priv = NULL;
    kfree(p);
    return 0;
}
```

顺序不能颠倒：先阻止新 I/O，再让设备失去 DMA buffer，最后释放内存。若有 waiter，remove 还要唤醒它们并使其返回 `-ENODEV`。display 驱动在释放 backing 前调用 `gpu_device_unregister()`。remove 应容忍 `drv_priv == NULL`，并支持 probe-remove-probe 再次使用设备。

## 11. 注册驱动

```c
static driver_t acme_driver = {
    .name = "acme-net",
    .id_table = acme_ids,
    .bus = &pci_bus,
    .probe = acme_probe,
    .remove = acme_remove,
    .class_ops = &acme_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(acme_driver);
```

宏把驱动指针放入 `.driver_init`。不需要也不得从 `kernel_main` 手动调用 probe。平台链接脚本必须 `KEEP(*(.driver_init))`；现有正式平台已经具备该段，新平台作者按 [总线与平台](bus-and-platform.md) 补齐。

## 12. 确认消费者能够找到设备

probe 成功不等于用户可见：

| 类 | 当前消费者/出口 |
|---|---|
| NET | lwIP 用 `device_find_by_class(DEV_CLASS_NET, n)` 枚举，无 `/dev` 节点 |
| DISPLAY | probe 另调用 display registry，用户经 `/dev/fb0` 使用 |
| INPUT | `/dev/event0` 聚合 `DEV_CLASS_INPUT` |
| BLOCK | 文件系统适配器消费 class；驱动不发布私有 getter |
| CHAR | 尚无通用动态 devfs 注册，不能在驱动里私建 vnode |

需要新增用户 ABI 时参考 [用户接口与 devfs](userspace-and-devfs.md)。硬件驱动只实现 class，用户地址检查、节点命名和 ioctl 版本由通用适配层负责。

## 13. 第一次构建和启动

先做静态门禁：

```sh
git diff --checkmake check-driver-core-modelmake check-doc-driftmake ARCH=<arch> BOARD=<board> ABI=both kernel-only -j4
```

启动后按顺序寻找日志：

```text
[DRIVER] registered driver 'acme-net' (class=3)[BUS] pci ... id=1234:5678 ...[DRIVER] registered device 'pci-...'[ACME] ready: ...[DRIVER] device 'pci-...' bound to driver 'acme-net'[LWIP] ... attached ...
```

没有 driver 注册日志：检查源文件是否进入构建、`DRIVER_REGISTER` 和链接段。没有 PCI 行：检查 VM/固件/ECAM，而不是 probe。看到 PCI 行但 probe 未调用：检查 bus 指针、ID 和 subsystem 通配。probe 超时：检查 BAR 号/长度、地址是否已是内核 VA、访问宽度、reset 顺序。probe 成功但无功能：检查 class 类型、ops 返回值和消费者桥。

## 14. 必做的失败测试

外部贡献至少验证以下情况，而不只验证一次成功启动：

1. 不提供目标设备时，驱动不误绑定其他硬件。
2. 模拟缺失/过小资源，probe 返回负 errno，`drv_priv` 保持空。
3. 在每次 DMA/内存分配处模拟失败，之前资源全部释放。
4. 设备不置 ready 时，等待在超时后返回且不会卡死开机。
5. 队列满、空队列、descriptor wrap 和异常完成长度不会越界。
6. 调用 remove 时，新 I/O 返回 `-ENODEV`，IRQ/DMA 不再访问旧内存。
7. remove 后重新 probe，静态槽、默认 registry 和 IRQ 能再次使用。
8. 目标平台和至少一个共享基础设施回归平台构建成功。
9. IRQ 路径必须证实中断真实触发（启动日志计数或调试观察），而不是只有预轮询窗口掩盖了永远不到来的中断；共享线场景下两个设备都要完成 I/O。
10. request_irq 失败的注入路径：实例必须显式降级为轮询且设备中断保持屏蔽，不能 park 在不会到来的中断上。

测试向量和提交证据格式见 [../testing/testing-gates.md](../testing/testing-gates.md)。

## 15. 下一步读什么

| 当前问题 | 阅读 |
|---|---|
| 不清楚对象字段、状态和完整 PCI 骨架 | [核心模型](core-model.md) |
| 固定地址设备、新板或新总线 | [总线与平台](bus-and-platform.md) |
| class 操作该返回什么 | [设备类接口](device-classes.md) |
| BAR、VirtIO feature/queue | [PCI 与 VirtIO](pci-and-virtio.md) |
| cache、DMA、IRQ、屏障、标准完成模型 | [运行时契约](runtime-contracts.md) |
| 锁可以怎样嵌套 | [锁顺序](lock-order.md) |
| framebuffer 映射和刷新 | [Display/Framebuffer](display.md) |
| VirtualBox 的真实发现链和设备 | [VirtualBox 驱动栈](../platforms/virtualbox.md) |

## 16. 评审前的自检

一个驱动达到“可供外部维护”的标准，应当能仅凭源码内寄存器命名、硬件契约表和本文档回答：设备怎样被发现；为什么会匹配；每项资源由谁拥有；哪个时刻设备可以 DMA；每个 class 返回值的单位；哪些字段由哪把锁保护；每个失败点怎样回滚；remove 后调用者看到什么；怎样在目标平台复现成功和失败。任一问题无法回答，都表示接口或文档仍不完整。

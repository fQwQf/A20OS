# 驱动运行时契约

本文列出所有使用 MMIO、IRQ、DMA、并发或等待的驱动必须遵守的规则。公共 API 在 `kernel/drivers/core/driver_hwapi.h`；驱动包含该头，不直接包含架构私有寄存器头。

## MMIO

使用 `readb/readw/readl/readq` 和 `writeb/writew/writel/writeq`。这些操作保留 volatile 访问并带读/写屏障。`*_relaxed` 不带屏障，只能在协议明确允许、且外层已有正确屏障的紧密循环中使用。

寄存器访问必须按硬件规定宽度，不能把 8 位 W1C 寄存器用 32 位 read-modify-write。W1C/W0C、读清除、写入触发等副作用要在寄存器定义旁说明。轮询循环必须同时具备硬件条件和超时：

```c
uint64_t deadline = clock_get_ticks() + clock_ticks_per_sec();
uint32_t spins = 1000000;
while (!(readl(status) & READY)) {
    if (clock_get_ticks() >= deadline || --spins == 0)
        return -ETIMEDOUT;
    arch_cpu_relax();
}
```

迭代上限对 VirtualBox ARM 软件 timer 和早期启动尤其重要。

## 内存屏障和设备所有权

CPU 写 descriptor/buffer 后，先执行 DMA for-device 同步，再用 `wmb()` 保证 descriptor 和索引顺序，最后敲 doorbell。设备完成后，先观察完成索引，再执行 for-CPU 同步，之后才能读取 response/data。

```text
CPU fill buffer
 -> dma_sync_for_device(buffer)
 -> fill descriptor
 -> dma_sync_for_device(descriptor)
 -> wmb()
 -> publish index / doorbell

observe completion
 -> dma_sync_for_cpu(used/descriptor)
 -> validate id and length
 -> dma_sync_for_cpu(buffer)
 -> consume data
```

`mb/rmb/wmb` 只约束 CPU/设备可见顺序，不替代 cache maintenance。DMA sync 也不替代发布顺序。

## DMA API

```c
void *dma_alloc_coherent(size_t size, uint64_t *dma_handle);
void dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle);
void dma_sync_for_device(void *vaddr, size_t size);
void dma_sync_for_cpu(void *vaddr, size_t size);
```

`dma_alloc_coherent` 返回清零的 CPU 地址，并在 `dma_handle` 返回设备使用的物理/DMA 地址。释放时必须传回同一地址、大小和 handle。当前分配器基于 `kmalloc`，不接受设备 DMA mask、IOMMU domain 或显式大对齐；需要 32 位 DMA、页对齐或大连续区的设备必须在 probe 中验证 handle/对齐，必要时使用页分配器并记录限制。

静态 ring/buffer 使用 `va_to_pa()` 生成 DMA 地址，并必须在各架构线性映射范围内。不得把栈上对象交给异步 DMA；同步命令也只有在确认完成后才能离开栈帧。设备超时后可能仍持有 buffer，必须先复位/停止设备，再复用或释放。

> 注意
> 不要把栈上变量或临时结构传给 DMA，即使你认为命令会很快完成。设备超时后仍可能访问该内存，而栈帧早已释放。超时后也不要立即复用同一块 buffer，先停止或复位设备。

## IRQ API

```c
int request_irq(uint32_t irq, irq_handler_t handler,
                unsigned long flags, void *priv);
void free_irq(uint32_t irq, void *priv);
```

成功注册后由驱动拥有，remove 和 probe 回滚必须成对 `free_irq`。`priv` 应是设备实例，释放时必须传回同一值。重复注册已占用 IRQ 返回 `-EBUSY`；驱动只有在协议明确支持轮询时才可降级。

handler 返回 `int`，当前 dispatch 不使用返回值。handler 必须：确认设备中断源；清硬件状态；完成有界 ring 推进；记录/唤醒后立即返回。不得分配、访问 VFS、睡眠或长时间轮询。ack/eoi 由核心/板级 irqchip 处理。

当前限制：IRQ 表固定 256 项；每 IRQ 一个 handler；`IRQF_SHARED` 没有通用 handler 链；VirtualBox ARM 的 PCI 驱动目前多数采用轮询，直到 ACPI MSI/INTx 路由完整。

`priv` 同时是 handler 参数和 IRQ 所有权 token。`free_irq(irq, priv)` 的 `priv` 必须与 `request_irq` 完全相同；不匹配不会移除他人的 handler。成功释放会先从 dispatch 表摘除 handler、屏蔽中断线，并等待已取得快照的在途 handler 返回，因此随后才能释放 `priv` 指向的内存。不得从该 IRQ 自己的 handler 中调用 `free_irq`。

## 锁

实例共享状态使用 `spinlock_t`。获取方式通常为：

```c
uint64_t flags = spin_lock_irqsave(&priv->lock);
/* 只做不会睡眠、不会分配的短操作 */
spin_unlock_irqrestore(&priv->lock, flags);
```

全局顺序为 `driver registry/IRQ locks -> device-private locks`。driver core 不得持有 registry 锁调用生命周期回调。设备锁通常最内层；不得在其下调用 VFS、`kmalloc`、调度或长 busy wait。网络另有 `g_lwip_lock -> device lock`。现有例外与具体锁保护字段见 [锁顺序](lock-order.md)。

自旋锁只能保护请求分配、descriptor 发布和完成回收。命令等待必须在锁外使用 completion/wait queue，或使用具备明确超时且不会关闭抢占的串行化机制；不得用自旋锁包住完整硬件超时周期。

## 等待与唤醒

避免“检查为空 -> IRQ 到来 -> 登记 waiter -> 永久睡眠”的丢唤醒。基本循环是：持锁检查条件；在同一锁下登记 waiter/队列；解锁；调度；醒来后重新检查。多个读者必须使用 wait queue，不能只放单个 `task_t *waiter`。

remove 必须先禁止新等待，再唤醒所有等待者并让其返回 `-ENODEV`。中断 handler 只唤醒，不在锁下执行使用者逻辑。

## 错误、超时和日志

包含 `core/errno.h`，返回最具体的负 errno。probe 中使用一条 `goto fail_*` 逆序清理链比多个裸 return 更可靠。日志前缀使用稳定驱动名，例如 `[AHCI]`、`[E1000]`；成功 probe 至少打印设备身份和关键容量/模式，失败打印阶段和错误，不打印每个数据包或每次寄存器轮询。

不可恢复协议错误应先停止/复位设备，再返回 `-EIO`。超时返回 `-ETIMEDOUT`。暂时队列满用 `-EAGAIN`，不要伪装成硬件错误。

## 用户指针

类驱动只处理内核 buffer。VFS/ABI 适配层负责 `copy_to_user/copy_from_user`。驱动 ioctl 不得直接解引用用户地址。所有结构长度、枚举值、位图范围、乘加溢出和映射边界必须在适配层验证。

违反这些契约通常会在 `make check-concurrency-foundation` 或 smoke 测试里暴露，详情见 [测试门禁](../testing/testing-gates.md)。

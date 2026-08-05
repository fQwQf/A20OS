# 驱动运行时契约

本文列出所有使用 MMIO、IRQ、DMA、并发或等待的驱动必须遵守的规则。公共 API 在 `kernel/drivers/core/driver_hwapi.h`；驱动包含该头，不直接包含架构私有寄存器头。

## MMIO

使用 `readb/readw/readl/readq` 和 `writeb/writew/writel/writeq`。这些操作保留 volatile 访问并带读/写屏障。`*_relaxed` 不带屏障，只能在协议明确允许、且外层已有正确屏障的紧密循环中使用。

寄存器访问必须按硬件规定宽度，不能把 8 位 W1C 寄存器用 32 位 read-modify-write。W1C/W0C、读清除、写入触发等副作用要在寄存器定义旁说明。轮询循环必须同时具备硬件条件和超时：

```c
uint64_t deadline = clock_get_ticks() + clock_ticks_per_sec();uint32_t spins = 1000000;while (!(readl(status) & READY)) {
    if (clock_get_ticks() >= deadline || --spins == 0)
        return -ETIMEDOUT;
    arch_cpu_relax();
}
```

迭代上限对 VirtualBox ARM 软件 timer 和早期启动尤其重要。

## 内存屏障和设备所有权

CPU 写 descriptor/buffer 后，先执行 DMA for-device 同步，再用 `wmb()` 保证 descriptor 和索引顺序，最后敲 doorbell。设备完成后，先观察完成索引，再执行 for-CPU 同步，之后才能读取 response/data。

```text
CPU fill buffer-> dma_sync_for_device(buffer)-> fill descriptor-> dma_sync_for_device(descriptor)-> wmb()-> publish index / doorbell

observe completion-> dma_sync_for_cpu(used/descriptor)-> validate id and length-> dma_sync_for_cpu(buffer)-> consume data
```

`mb/rmb/wmb` 只约束 CPU/设备可见顺序，不替代 cache maintenance。DMA sync 也不替代发布顺序。

## DMA API

```c
void *dma_alloc_coherent(size_t size, uint64_t *dma_handle);void dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle);void dma_sync_for_device(void *vaddr, size_t size);void dma_sync_for_cpu(void *vaddr, size_t size);
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

成功注册后由驱动拥有，remove 和 probe 回滚必须成对 `free_irq`。`priv` 应是设备实例，释放时必须传回同一值。

handler 返回 `int`，当前 dispatch 不使用返回值。handler 必须：确认设备中断源；清硬件状态；完成有界 ring 推进；记录/唤醒后立即返回。不得分配、访问 VFS、睡眠或长时间轮询。ack/eoi 由核心/板级 irqchip 处理。

当前限制：IRQ 表固定 256 项。`IRQF_SHARED` 已实现通用 handler 链：已有注册与新请求都带 `IRQF_SHARED` 时同线追加 handler，dispatch 依次调用全部 handler，因此共享电平线的每个设备都必须确认并清自己的中断源；任何一方不带 `IRQF_SHARED` 则第二个注册返回 `-EBUSY`。x86_64 QEMU（q35）PCI INTx 已实现 IOAPIC 路由（GSI 20-23，`arch_pci_intx_irq` 覆盖，路由即屏蔽、`request_irq` 自动解掩，i440fx 因缺少 ACPI _PRT 保持轮询）；VirtualBox ARM 的 PCI 驱动目前多数采用轮询，直到 ACPI MSI/INTx 路由完整。

`priv` 同时是 handler 参数和 IRQ 所有权 token。`free_irq(irq, priv)` 的 `priv` 必须与 `request_irq` 完全相同；不匹配不会移除他人的 handler。成功释放会先从 dispatch 表摘除 handler、屏蔽中断线，并等待已取得快照的在途 handler 返回，因此随后才能释放 `priv` 指向的内存。不得从该 IRQ 自己的 handler 中调用 `free_irq`。

## 标准完成模型（IRQ hybrid）

IRQ 驱动是 A20OS 的默认数据面范式。新驱动不得把"全程 busy-poll 直到硬件超时"作为完成模型；只有在平台确实无法提供中断时，才允许明确的轮询降级。参考实现：`kernel/drivers/block/virtio_blk.c`（`VIRTIO_BLK_IRQ_MODEL`）、`kernel/drivers/block/nvme.c`（`NVME_IRQ_MODEL`）、`kernel/drivers/block/ahci.c`（`AHCI_IRQ_MODEL`）。

标准模型由四部分组成：

1. **IRQ 解析**。PCI 驱动用 `pci_intx_irq(dev)` 取得平台路由后的中断标识；VirtIO 驱动使用 transport 的 `vt.irq`（PCI transport 由 `pci_virtio_transport_init` 填充并置 `shared_irq=1`）；MMIO/platform 驱动使用 `RES_IRQ` 资源。**禁止**把 PCI 配置空间 0x3C 的 IRQ Line 寄存器当作可用中断标识——它是固件遗留值，与平台中断控制器无关。解析结果为 `-1` 表示平台无路由，进入轮询降级。

2. **注册与设备中断使能的顺序**。`request_irq()` 必须在设备能够产生中断之前完成；**设备级中断使能位（PxIE、IMS、INTMC、queue IEN 等）只在 handler 注册成功后才打开**。反向顺序会让无 handler 的中断源一直保持共享电平线有效，形成中断风暴。PCI INTx 注册必须带 `IRQF_SHARED`。注册失败必须把实例显式标记为轮询模式（如 `vt->irq = -1`）并保持设备中断屏蔽——绝不允许"注册失败但结构看起来仍是 IRQ 驱动"，等待路径会 park 在一个永远不会到来的中断上。

3. **hybrid 等待**。提交路径先在有界窗口内轮询完成（通常数百微秒，覆盖快速完成，省去一次中断往返和睡眠切换），仍未完成则 park 在实例的 `wait_queue_t` 上，由 IRQ top-half 唤醒：

```c
uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(CMD_TIMEOUT_MS);uint64_t pre_poll_until = timer_get_ticks() + US_TO_TICKS(800);for (;;) {
    int ret = consume_completion_locked_or_lockfree(priv);
    if (ret != -EAGAIN)
        return ret;
    if (!priv->irq_registered) {            /* 轮询降级：有界忙等 */
        if (timer_get_ticks() >= deadline)
            return -ETIMEDOUT;
        udelay(1000);
        continue;
    }
    uint64_t now = timer_get_ticks();
    if (now >= deadline)
        return -ETIMEDOUT;
    if (now < pre_poll_until) {             /* hybrid 预轮询窗口 */
        udelay(20);
        continue;
    }
    uint64_t chunk = now + MS_TO_TICKS(50); /* 有界 park 块 */
    if (chunk > deadline)
        chunk = deadline;
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
    wait_queue_entry_t entry = {0};
    wait_queue_link(&priv->waiters, &entry, token, 0);
    ret = consume_completion_locked_or_lockfree(priv);  /* link 后重查 */
    if (ret != -EAGAIN) {
        wait_queue_unlink(&priv->waiters, &entry);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return ret;
    }
    (void)proc_park_commit(token);
    wait_queue_unlink(&priv->waiters, &entry);
    proc_park_finish(token);
}
```

park 块必须有界（50 ms 量级），把理论上丢失的唤醒降级为一次重查，而不是整个命令超时的停滞。boot 早期没有 `proc_current()` 的路径只能使用有界轮询，不得 park。等待期间需要串行化多命令时，实例锁应使用可睡眠的 `mutex_t`（IRQ top-half 永不获取它）；自旋锁必须在 park 之前释放。

4. **top-half 只唤醒**。handler 确认并清设备中断源（ISR 读取/写回、PxIS 写清、ICR 读清等），然后 collect waiters 并 flush，不触碰提交者拥有的 ring 状态——被唤醒的提交者自己重查完成：

```c
static int acme_irq(int irq, void *priv){
    acme_priv_t *p = priv;
    if (!p)
        return 0;
    uint32_t is = acme_read(p, ACME_REG_INT_STATUS);
    if (!is)
        return 0;                     /* 共享线上的他人中断 */
    acme_write(p, ACME_REG_INT_STATUS, is);
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_all(&p->waiters, 0, PROC_WAKE_EVENT,
                                 &wake_q, NULL);
    (void)proc_wake_q_flush(&wake_q);
    return 0;
}
```

若 handler 清状态会吃掉 waiter 必须观察的位（如 AHCI 的 TFES），先把原始状态记入实例字段（如 `last_is`）再清寄存器。`wait_queue_t` 内部自锁，IRQ 上下文 collect 不需要也不能持有实例 mutex。

**remove / probe 回滚的顺序**：先屏蔽设备中断（清设备使能位或设备复位），再 `free_irq()`，最后释放 DMA 和实例内存。`free_irq` 返回时保证在途 handler 已退出。

**轮询降级的纪律**：允许降级的驱动必须在 [实现状态](implementation-status.md) 记录适用平台与解除轮询的条件；每次轮询调用工作量必须有界；无 IRQ 也无 `.poll` 进展入口的设备不得让 probe 成功。

## 锁

实例共享状态使用 `spinlock_t`。获取方式通常为：

```c
uint64_t flags = spin_lock_irqsave(&priv->lock);/* 只做不会睡眠、不会分配的短操作 */spin_unlock_irqrestore(&priv->lock, flags);
```

全局顺序为 `driver registry/IRQ locks -> device-private locks`。driver core 不得持有 registry 锁调用生命周期回调。设备锁通常最内层；不得在其下调用 VFS、`kmalloc`、调度或长 busy wait。网络另有 `g_lwip_lock -> device lock`。现有例外与具体锁保护字段见 [锁顺序](lock-order.md)。

自旋锁只能保护请求分配、descriptor 发布和完成回收。命令等待不得用自旋锁包住完整硬件超时周期：要么在锁外按 [标准完成模型](#标准完成模型irq-hybrid) park，要么把实例串行化锁换成可睡眠的 `mutex_t` 并允许持 mutex park（IRQ top-half 永不获取该 mutex）。

## 等待与唤醒

避免“检查为空 -> IRQ 到来 -> 登记 waiter -> 永久睡眠”的丢唤醒。基本循环是：持锁检查条件；在同一锁下登记 waiter/队列；解锁；调度；醒来后重新检查。多个读者必须使用 wait queue，不能只放单个 `task_t *waiter`。硬件完成等待的完整范式（hybrid 预轮询、有界 park 块、link 后重查）见上文 [标准完成模型](#标准完成模型irq-hybrid)。

remove 必须先禁止新等待，再唤醒所有等待者并让其返回 `-ENODEV`。中断 handler 只唤醒，不在锁下执行使用者逻辑。

## 错误、超时和日志

包含 `core/errno.h`，返回最具体的负 errno。probe 中使用一条 `goto fail_*` 逆序清理链比多个裸 return 更可靠。日志前缀使用稳定驱动名，例如 `[AHCI]`、`[E1000]`；成功 probe 至少打印设备身份和关键容量/模式，失败打印阶段和错误，不打印每个数据包或每次寄存器轮询。

不可恢复协议错误应先停止/复位设备，再返回 `-EIO`。超时返回 `-ETIMEDOUT`。暂时队列满用 `-EAGAIN`，不要伪装成硬件错误。

## 用户指针

类驱动只处理内核 buffer。VFS/ABI 适配层负责 `copy_to_user/copy_from_user`。驱动 ioctl 不得直接解引用用户地址。所有结构长度、枚举值、位图范围、乘加溢出和映射边界必须在适配层验证。

违反这些契约通常会在 `make check-concurrency-foundation` 或 smoke 测试里暴露，详情见 [测试门禁](../testing/testing-gates.md)。

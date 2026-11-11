# 驱动运行时契约

原生 GPU 与其他异步设备的新增契约见 [原生驱动运行时](native-runtime.md)：下线分为 offline、quiesce、在途调用排空和 remove；页级 DMA mapping、原生 fence、可取消工作队列、IRQ 排空及有界固件读取已提供独立接口，硬件 GPU 驱动仍待实现。

使用 MMIO、IRQ、DMA、并发或等待的驱动必须遵守下列规则。公共 API 在 `kernel/drivers/core/driver_hwapi.h`；驱动包含该头，不直接包含架构私有寄存器头。

## MMIO

`resource_t` 中的 MMIO 是包含两端的物理地址范围。先用 `device_map_mmio(dev, index)` 或 `device_map_resource(dev, res)` 映射；PCI 使用 `pci_map_bar(dev, physical_bar_number)`。映射失败即不可访问。映射由设备核心托管，在 probe 失败或解除绑定时回收，驱动不得自行加 `PAGE_OFFSET` 或把资源地址强制转换为指针。

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
CPU fill buffer-> dma_sync_for_device(buffer)-> fill descriptor-> dma_sync_for_device(descriptor)-> wmb()-> publish index / doorbell

observe completion-> dma_sync_for_cpu(used/descriptor)-> validate id and length-> dma_sync_for_cpu(buffer)-> consume data
```

`mb/rmb/wmb` 只约束 CPU/设备可见顺序，不替代 cache maintenance。DMA sync 也不替代发布顺序。

## DMA API

```c
void *dma_alloc_coherent(struct device *dev, size_t size, uint64_t *dma_handle);
void dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle);
void *dma_alloc_coherent_aligned(struct device *dev, size_t size,
                                 size_t alignment, uint64_t *dma_handle);
void dma_free_coherent_aligned(void *vaddr, size_t size, uint64_t dma_handle);
void dma_sync_for_device(void *vaddr, size_t size);
void dma_sync_for_cpu(void *vaddr, size_t size);

int      dma_set_mask(struct device *dev, uint64_t mask);
uint64_t dma_get_mask(const struct device *dev);
int      dma_addr_ok(const struct device *dev, uint64_t addr);
int      dma_range_ok(const struct device *dev, uint64_t addr, size_t size);
```

`dma_alloc_coherent` 返回清零的 CPU 地址，并在 `dma_handle` 返回设备使用的物理/DMA 地址。释放时必须传回同一地址、大小和 handle；释放不查 mask，所以 free 系列不带 `dev`。`dma_alloc_coherent_aligned` 是页分配对齐版本（物理连续 + 指定对齐）。

`dma_alloc_coherent()` 可能返回两种来源的内存：kmalloc 快路径的块，或（块落在 mask 窗口外时）帧分配器的物理连续页块。因此 `dma_free_coherent()` 不能一律 `kfree()`——把帧分配器页块交给 `kfree()` 会命中 `panic("kfree: invalid pointer")`。释放端用 `kmalloc_owns()` 判定来源（slab 页头 magic / big-alloc 头，与 `kfree()` 内部的判断同序同条件）：kmalloc 的块走 `kfree()`，帧分配器页块走 `pfa_free()`（`dma_free_coherent_aligned()` 的路径）。不要用「页对齐」或 `dma_handle == 0` 代替这个判定——kmalloc 的大块分配同样页对齐。分配与释放仍然必须成对：`dma_alloc_coherent()` ↔ `dma_free_coherent()`，`dma_alloc_coherent_aligned()` ↔ `dma_free_coherent_aligned()`。

### DMA mask

`device_t.dma_mask` 记录设备 DMA 引擎能接收的地址位。字段值 0 表示「未声明」，`dma_get_mask()` 按 `DMA_MASK_64BIT`（`0xffffffffffffffff`）处理，因此没有调用 `dma_set_mask()` 的总线枚举器和板级设备保持完整 64 位窗口。`DMA_MASK_32BIT` 为 `0x00000000ffffffff`。

规则：

1. probe 中读到硬件能力后、第一次分配 DMA 之前调用 `dma_set_mask()`。mask 必须是低位连续 1，否则返回 `-EINVAL`，不会被静默取整。
2. 分配器按 mask 校验并在不满足时另选内存：`dma_alloc_coherent()` 先走 kmalloc 快路径，块落在窗口外时归还并改从帧分配器取物理连续页；`dma_alloc_coherent_aligned()` 向帧分配器重试固定次数（`DMA_MASK_ALLOC_ATTEMPTS`）。
3. 无法满足时返回 `NULL`。`NULL` 是 probe 失败的信号，没有 bounce 路径，也不会截断 handle。**不得**为了让它成功而改用一个设备解不了的地址。
4. `dma_alloc_*_coherent(NULL, ...)` 表示「没有可约束的设备」，走完整 64 位窗口。普通设备驱动应传入已枚举的 `device_t`，以应用该设备的 DMA mask。
5. 驱动自己分配的 ring/context（例如 kmalloc 出来的 xHCI controller 结构）不经过分配器，必须在把地址写进设备前用 `dma_addr_ok()` / `dma_range_ok()` 校验，否则返回失败。

当前限制：帧分配器没有「给我 4 GiB 以下的内存」这类分区接口，收窄 mask 只能靠有界重试。在 RAM 全部位于 32 位窗口之上的机器上，声明 32-bit mask 的设备会 probe 失败；这比让它在运行中 DMA 到解不了的地址更接近真实行为，但确实是一个失败而不是成功。旧 coherent 分配路径中的 mask 只是窗口检查，不是地址翻译。新增页级 DMA domain/mapping 已定义 provider 翻译接口和回滚契约，但没有为旧分配器自动接入 IOMMU；现有 RISC-V 用户态隔离路径也尚未统一为该 provider 后端。

`dma_sync_for_device()` / `dma_sync_for_cpu()` 仍然只是把请求委托给架构 cache hook，mask 不改变同步语义。

## 静态 ring 与 `va_to_pa()`

既有静态 ring/buffer 仅在平台确认恒等 DMA 时使用 `va_to_pa()` 生成设备地址，并必须在各架构线性映射范围及设备 DMA 窗口内。新原生驱动的页数组应通过 `driver_dma_map_pages()` 获取设备地址，不能假定物理地址就是 IOVA。不得把栈上对象交给异步 DMA；同步命令也只有在确认完成后才能离开栈帧。设备超时后可能仍持有 buffer，必须先复位/停止设备，再复用或释放。

> 不要把栈上变量或临时结构传给 DMA，即使命令会很快完成。设备超时后仍可能访问该内存，而栈帧早已释放。超时后也不要立即复用同一块 buffer，先停止或复位设备。

## IRQ API

```c
int request_irq(uint32_t irq, irq_handler_t handler,
                unsigned long flags, void *priv);
void free_irq(uint32_t irq, void *priv);
int synchronize_irq(uint32_t irq);
```

成功注册后由驱动拥有，remove 和 probe 回滚必须成对 `free_irq`。`priv` 应是设备实例，释放时必须传回同一值。重复注册已占用 IRQ 返回 `-EBUSY`；驱动只有在协议明确支持轮询时才可降级。

handler 返回 `int`，当前 dispatch 不使用返回值。handler 必须：确认设备中断源；清硬件状态；完成有界 ring 推进；记录/唤醒后立即返回。不得分配、访问 VFS、睡眠或长时间轮询。ack/eoi 由核心/板级 irqchip 处理。

当前限制：IRQ 表固定 256 项。`IRQF_SHARED` 已实现单链表 handler chain；首个注册和所有追加注册都必须带共享标志，否则重复注册返回 `-EBUSY`。VirtualBox ARM 的 PCI 驱动目前多数采用轮询，直到 ACPI MSI/INTx 路由完整。

`priv` 同时是 handler 参数和 IRQ 所有权 token。`free_irq(irq, priv)` 的 `priv` 必须与 `request_irq` 完全相同；不匹配不会移除他人的 handler。成功释放会先从 dispatch 表摘除 handler、屏蔽中断线，并等待已取得快照的在途 handler 返回，因此随后才能释放 `priv` 指向的内存。不得从该 IRQ 自己的 handler 中调用 `free_irq`。

`synchronize_irq()` 保留注册，等待已进入核心的完整分发（包含 EOI/重启用）返回。它不是对未来中断的屏障，不替代 `free_irq()` 或工作队列取消。必须先停止设备 IRQ 来源，不能在同一 IRQ handler 内或持有其需要的锁时调用。

## 后台工作与延迟执行

原生驱动可使用 `driver_workqueue_*` / `driver_work_*`，将可能睡眠的完成处理和恢复操作放入独立内核 worker。提交/重设 deadline 在 IRQ 中不分配内存；cancel_sync、flush、destroy 只在任务上下文且不持有回调需要的锁时执行。销毁超时保留对象，不能立即释放 context。接口语义和引用规则见 [原生驱动运行时](native-runtime.md#后台工作延迟任务与销毁)。

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

异步驱动在 quiesce 中禁止新提交，停止或复位硬件后结束并唤醒未完成请求；核心随后排空在途调用，再执行 remove。不得等到 remove 才唤醒阻塞的 class 调用。中断 handler 可以 signal 原生 fence，但不在 fence/设备自旋锁下运行等待者逻辑。等待超时不表示硬件已经停止，不能据此释放 DMA 缓冲。

## 错误、超时和日志

包含 `core/errno.h`，返回最具体的负 errno。probe 中使用一条 `goto fail_*` 逆序清理链比多个裸 return 更可靠。日志前缀使用稳定驱动名，例如 `[AHCI]`、`[E1000]`；成功 probe 至少打印设备身份和关键容量/模式，失败打印阶段和错误，不打印每个数据包或每次寄存器轮询。

不可恢复协议错误应先停止/复位设备，再返回 `-EIO`。超时返回 `-ETIMEDOUT`。暂时队列满用 `-EAGAIN`，不要伪装成硬件错误。

## 用户指针

类驱动的 read/write 只处理内核 buffer。VFS/ABI 适配层负责 `copy_to_user/copy_from_user`。audio 通用 ioctl 和 block 的标准查询已在 devfs 封送，但自定义 block/char ioctl 当前仍会收到原始 `arg`；驱动不得直接解引用，应先扩展通用适配层。所有结构长度、枚举值、位图范围、乘加溢出和映射边界必须在适配层验证。

违反这些契约通常会在 `make check-concurrency-foundation` 或 smoke 测试里暴露，详情见 [测试门禁](../../testing-gates.md)。

## 固件读取

使用 `drivers/core/firmware.h`，在可睡眠的任务上下文从显式 source 加载。VFS source 接收可信启动配置已经打开的目录并独立持有引用，后续读取不依赖调用线程的 cwd/chroot，也不占用其 fd。名称必须是规范相对路径；只支持实际普通文件，不跟随 symlink、不进入子挂载、不自动解压或搜索其他来源。目录及其写入者必须可信，内核读取不套用当前用户的 DAC 权限。

驱动应给每类固件设置合理的非零 `max_size`，同时受 64 MiB 硬上限限制；短读可继续，提前 EOF、长度增长、空文件或超限均失败并释放部分资源。只有成功返回的 image 才可交给格式解析器。image 是独立的不可变 CPU 字节副本，持有引用期间有效；它不提供 DMA 地址、签名认证或设备初始化。读取过程不排斥并发修改，应部署不可变固件。详细的 provider、VFS 生命周期和错误语义见 [原生驱动运行时](native-runtime.md#固件数据与显式来源)。

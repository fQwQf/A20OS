# 驱动运行时契约

原生 GPU 与其他异步设备的新增契约见 [原生驱动运行时](native-runtime.md)：失败 probe 可在初始化前显式选择 `device_probe_use_remove()`，由同一退出路径处理部分实例，清理失败保留绑定与原 probe_error；子设备阻塞时保持 PROBE_CLEANUP 并拒绝新资源/DMA 使用。需要提前保护 child 存储的 provider 可登记可失败 remove prepare，先于 consumer 解绑，失败保留 PREPARING 并跳过已完成动作。下线分为 offline、quiesce、在途调用排空和可失败 remove；错误保留绑定/devres，显式重试不重复 quiesce；页级 DMA mapping、原生 fence、可取消工作队列、IRQ 排空及有界固件读取已提供独立接口，硬件 GPU 驱动仍待实现。

使用 MMIO、IRQ、DMA、并发或等待的驱动必须遵守下列规则。公共 API 在 `kernel/drivers/core/driver_hwapi.h`；驱动包含该头，不直接包含架构私有寄存器头。

当前模块 ABI 10 增加 HCD 的可失败 endpoint 释放；HID/hub 独立退出在 class drain 后调用 `usb_interface_release_endpoints()`，成功才交还 URB、buffer 和回调代码所有权。失败保留绑定并重试，不得清空回调指针后直接释放。Configure Endpoint 失败的 ring 由 controller 保留到可证明停止。详见 [接口独立退出](native-runtime.md#接口独立退出与-endpoint-释放)。

## 总线拓扑与进度

总线任务可用 `driver_lifecycle_begin/end()` 协调私有拓扑和核心绑定，所属上下文允许嵌套且不压制 probe。`driver_lifecycle_try_begin()` 只接受最外层入口，忙或同上下文重入均返回 `-EAGAIN`；成功后才 end。持 active class 调用时不能阻塞等待正在排空自己的 teardown，USB HID 使用 `usb_device_poll()` 尝试进度。这些 API 不用于 IRQ、不固定存储或模块代码，成功进入后的硬件操作仍可能等待。完整规则见 [原生拓扑协调](native-runtime.md#总线拓扑的原生生命周期协调)。

## MMIO

`resource_t` 中的 MMIO 是包含两端的物理地址范围。先用 `device_map_mmio(dev, index)` 或 `device_map_resource(dev, res)` 映射；PCI 使用 `pci_map_bar(dev, physical_bar_number)`。映射失败即不可访问。映射由设备核心托管，在已安全回滚的 probe 失败或成功解除绑定时回收；remove 失败则继续保留，驱动不得自行加 `PAGE_OFFSET` 或把资源地址强制转换为指针。

这些映射 helper 自动申请 owner 独占区间；另一设备持有重叠区间时拒绝映射。`device_map_resource_exclusive(dev, res, &address)` 返回详细错误（如 `-EBUSY`、`-ENOMEM`、`-EOVERFLOW`），失败清空 address；原有指针接口返回 NULL。受管映射保留设备引用，architecture unmap 完成后才归还区间。驱动应在 quiesce/remove 中停止访问，不能将地址区间的释放当作 CPU 访问撤销或 DMA 停止。

port I/O 可先调用 `device_claim_resource()` 申请 `dev->res` 中的 `RES_IOPORT`，范围限 0..ffffh。显式 `driver_resource_request()` 句柄需要独立释放，且会跨 unbind 保留占用；其 device 引用只保护对象存储，不保证绑定仍在线。受管申请及 devres 链操作限已经串行化的 probe/lifecycle 上下文。raw `arch_device_map_mmio()`、`drv_map_mmio()` 和直接 port I/O 访问不自动参与占用登记，不能据此宣称所有硬件访问都已隔离。

`driver_resource_request_checked()` 在相同生命周期锁中核对当前已发布资源并建立申请；未绑定设备的声明也会阻止冲突申请，`RES_MEM` 与 MMIO 按同一物理地址空间处理。该句柄还阻止后续冲突设备注册。所有资源声明更新都必须与生命周期事务协调；查询本身不能保护未串行化的直接写入。

hosted 启动用 `driver_memory_protect_boot()` 原子导入受管 RAM 和 ACPI 表所在页。独立 `driver_memory_reserve()` 对象也可保护其他可信物理存储；MMIO 申请、映射与发布均受约束，`RES_MEM` 存储描述可以共存。这不是物理页分配、DMA 隔离或 raw 映射撤销。完整契约见 [物理存储保留对象](native-runtime.md#物理存储保留对象)。

PCI 资源更新在 discovery 事务中运行。新 MMIO 地址使用 `driver_resource_check_range()`，已有 BAR 使用 `driver_resource_check_claims()`；前者包含声明和占用，后者包含 active claims 与受保护存储，避免把重复/包含关系的固件声明误当作活动使用者。已有非零 BAR 在 reachability hook 前先检查冲突，避免平台提前建立落入受保护存储的 CPU 别名。BAR 重建路径在 sizing 期间关闭地址解码和 BME；所有 BAR 规划成功后才写新地址、发布资源并恢复/设置 command。规划失败返回时，原 BAR、command 和资源元数据保持不变，单调分配游标可能推进。调用者必须在进入 API 前 quiesce：清除 BME 不等于排空在途 DMA，也没有可失败 PCI config 写入的通用回滚。

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

`dma_alloc_coherent` 返回清零的 CPU 地址，并在 `dma_handle` 返回设备使用的物理/DMA 地址。释放时必须传回同一地址、大小和 handle；释放不查 mask，所以 free 系列不带 `dev`。`dma_alloc_coherent_aligned` 是页分配版本，对齐须为非零二次幂且不超过 PAGE_SIZE；更强对齐应使用独立 DMA buffer 对象。

`dma_alloc_coherent()` 可能返回两种来源的内存：kmalloc 快路径的块，或（块落在 mask 窗口外时）帧分配器的物理连续页块。因此 `dma_free_coherent()` 不能一律 `kfree()`——把帧分配器页块交给 `kfree()` 会命中 `panic("kfree: invalid pointer")`。释放端用 `kmalloc_owns()` 判定来源（slab 页头 magic / big-alloc 头，与 `kfree()` 内部的判断同序同条件）：kmalloc 的块走 `kfree()`，帧分配器页块走 `pfa_free()`（`dma_free_coherent_aligned()` 的路径）。不要用「页对齐」或 `dma_handle == 0` 代替这个判定——kmalloc 的大块分配同样页对齐。分配与释放仍然必须成对：`dma_alloc_coherent()` ↔ `dma_free_coherent()`，`dma_alloc_coherent_aligned()` ↔ `dma_free_coherent_aligned()`。

### 原生 buffer 对象

`driver_dma_buffer_create(dev, size, alignment, &buffer)` 保存实际页阶数和设备引用，支持请求大小独立于强对齐，按捕获的 mask 分配和校验。用 `driver_dma_buffer_get/put` 独立交还引用，`data/address/size` 观察 CPU alias、CPU PA 和请求长度；地址不是自动生成的 IOVA。创建不等待生命周期锁，最终 put 在任务上下文执行，不能把 put 当作硬件停止或映射失效。unbind 不自动释放对象。`driver_dma_map_buffer()` 可显式将其页覆盖范围接入目标 domain，mapping 持有整块对象而非高阶尾页引用；最后一页 padding 也须满足 policy，按页查询 IOVA。完整错误码、限制和 xHCI 首个使用者见 [DMA buffer 所有权](native-runtime.md#独立-dma-buffer-所有权)。

### DMA mask

`device_t.dma_mask` 记录设备 DMA 引擎能接收的地址位。字段值 0 表示「未声明」，`dma_get_mask()` 按 `DMA_MASK_64BIT`（`0xffffffffffffffff`）处理，因此没有调用 `dma_set_mask()` 的总线枚举器和板级设备保持完整 64 位窗口。`DMA_MASK_32BIT` 为 `0x00000000ffffffff`。

规则：

1. probe 中读到硬件能力后、第一次分配 DMA 之前调用 `dma_set_mask()`。mask 必须是低位连续 1，否则返回 `-EINVAL`，不会被静默取整。
2. 分配器按 mask 校验并在不满足时另选内存：`dma_alloc_coherent()` 先走 kmalloc 快路径，块落在窗口外时归还并改从帧分配器取物理连续页；`dma_alloc_coherent_aligned()` 在 mask 受限时通过 `pfa_alloc_max(order, mask)` 选取物理连续块，完整 64 位窗口则使用普通 PFA。
3. 无法满足时返回 `NULL`。`NULL` 是 probe 失败的信号，没有 bounce 路径，也不会截断 handle。**不得**为了让它成功而改用一个设备解不了的地址。
4. `dma_alloc_*_coherent(NULL, ...)` 表示「没有可约束的设备」，走完整 64 位窗口。普通设备驱动应传入已枚举的 `device_t`，以应用该设备的 DMA mask。
5. 驱动自己分配的 ring/context（例如 kmalloc 出来的 xHCI controller 结构）不经过分配器，必须在把地址写进设备前用 `dma_addr_ok()` / `dma_range_ok()` 校验，否则返回失败。

受限物理分配已通过 `pfa_alloc_max()` 搜索窗口内的 buddy 块；窗口内没有可用 RAM 时明确失败。旧 coherent 分配路径中的 mask 仍只是物理窗口检查，不做地址翻译。原生页数组和 buffer 可显式接入 domain/provider，RISC-V 已有 managed 后端；旧分配器不会自动选择 IOMMU 或在 provider 失败时降级为物理 DMA。

`dma_sync_for_device()` / `dma_sync_for_cpu()` 仍然只是把请求委托给架构 cache hook，mask 不改变同步语义。

## 静态 ring 与 `va_to_pa()`

既有静态 ring/buffer 仅在平台确认恒等 DMA 时使用 `va_to_pa()` 生成设备地址，并必须在各架构线性映射范围及设备 DMA 窗口内。新原生驱动的页数组应通过 `driver_dma_map_pages()`，高阶 buffer 通过 `driver_dma_map_buffer()`、已物化的原生 VMO 页通过 `driver_dma_map_vmo()` 获取设备地址，不能假定物理地址就是 IOVA。不得把栈上对象交给异步 DMA；同步命令也只有在确认完成后才能离开栈帧。设备超时后可能仍持有 buffer，必须先复位/停止设备，再复用或释放。

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

## VMO 页快照导入

不要把 `vmo_peek_page()` 的借用 PFN 留到稍后再 pin；并发 resize 可以先释放它。MM 的 `vmo_pin_pages()` 原子验证/保留 canonical RAM，不物化缺页；驱动的 `driver_dma_map_vmo(domain, vmo, first, count, direction, &mapping)` 管理临时快照并交给页级 mapper。缺页 `-EAGAIN`，物理 VMO `-EOPNOTSUPP`，单次最多 4096 页；最后逻辑页 padding 也在 policy/DMA 范围内。provider 不持 VMO lock，映射和失败清理句柄保留独立帧引用，因此可在返回后释放源 VMO；它们不随 resize 改用新页，不接管 cgroup charge、权限或 BO 驻留。完整协议见 [VMO 快照](native-runtime.md#从原生-vmo-保留页并映射)。

## 可失败的 DMA 失效

可能超时的 IOMMU provider 使用 `driver_dma_checked_ops` 和 `driver_dma_domain_create_checked()`。必须检查 `driver_dma_try_unmap_pages()` 的状态：失败后 mapping 指针、页引用和 domain 均保留，禁止继续提交或查询地址，应在硬件停止期间重试。映射失败的回滚也可能失败，此时 `driver_dma_map_pages()`、`driver_dma_map_buffer()` 或 `driver_dma_map_vmo()` 会在负返回码之外交出非空清理句柄，调用者仍负责保留和重试。不能只按负返回码直接退出 probe 并释放 provider 资源；完整的生命周期规则见 [原生驱动运行时](native-runtime.md#原生-dma-domain-与页映射)。

## 固件读取

使用 `drivers/core/firmware.h`，在可睡眠的任务上下文从显式 source 加载。VFS source 接收可信启动配置已经打开的目录并独立持有引用，后续读取不依赖调用线程的 cwd/chroot，也不占用其 fd。名称必须是规范相对路径；只支持实际普通文件，不跟随 symlink、不进入子挂载、不自动解压或搜索其他来源。目录及其写入者必须可信，内核读取不套用当前用户的 DAC 权限。

驱动应给每类固件设置合理的非零 `max_size`，同时受 64 MiB 硬上限限制；短读可继续，提前 EOF、长度增长、空文件或超限均失败并释放部分资源。只有成功返回的 image 才可交给格式解析器。image 是独立的不可变 CPU 字节副本，持有引用期间有效；它不提供 DMA 地址、签名认证或设备初始化。读取过程不排斥并发修改，应部署不可变固件。详细的 provider、VFS 生命周期和错误语义见 [原生驱动运行时](native-runtime.md#固件数据与显式来源)。

## 硬件 DMA domain 关闭

需要交接失败页及重试硬件 detach 的 provider 使用 `driver_dma_managed_ops`。map 的 `needs_unmap` 必须明确指出当前页是否仍需清理，即使返回错误也不能丢失已安装翻译的地址。驱动停止硬件后调用 `driver_dma_domain_shutdown()` 关闭提交，排空/解除所有 mapping 和清理句柄，再重试 shutdown，成功后才 put owner。仅 put 不会强制拆除硬件 domain，省略 shutdown 会保留内部引用。回调在每 domain mutex 下串行执行，不能递归操作同一 domain。详细错误语义见 [原生驱动运行时](native-runtime.md#原生-dma-domain-与页映射)。

RISC-V 原生 PCI 驱动可显式使用 `riscv_iommu_dma_domain_create()`。创建前和 shutdown 前均需停止 DMA、关闭 bus mastering，并与绑定/用户所有权变更串行化。创建错误若仍输出 domain，必须保留这个清理句柄，重试 shutdown 后再 put；不能回退恒等 DMA。当前 requester 范围、页数限制与自检范围见 [原生驱动运行时](native-runtime.md#原生-dma-domain-与页映射)。

原生 RISC-V 驱动使用 `riscv_iommu_dma_fault_poll()` 取得并处理共享 FQ 故障。`blocked`、`isolated` 和 `isolation_error` 必须分开检查：软件停发与隔离命令完成都不证明设备请求已停止，不能直接回收 DMA 页。`queue_error` 非零表示 fault 记录可能丢失，应将已知 count 当作下界并进入恢复流程。接口是任务上下文轮询，不自动完成 fence，不自动恢复硬件。

需要后台监测时使用 `riscv_iommu_dma_watch_start()`。回调固定创建时的 owner token，快照有效期仅到回调返回；可 signal 驱动持有的 fence，但不自动停止硬件。quiesce 中先 `watch_stop()` 排空，再销毁 domain/context。停止超时保留句柄和回调资源，必须重试；回调内自停止返回 `-EDEADLK`。watch 不保持绑定有效，也不自动固定回调所在模块。

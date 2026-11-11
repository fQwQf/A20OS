# 原生驱动运行时：GPU 开发的第一阶段基础

本文区分已经接入源码的接口和后续工作。已提供设备 quiesce、页级 DMA mapping、原生异步 fence、可取消的工作队列、IRQ 排空和有界固件读取接口；不表示 AMD/NVIDIA 硬件加速已经可用。接口适用于 hosted 内核，MCU profile 不编入这些 hosted 运行时实现。

## 分层与边界

| 层次 | 职责 |
|---|---|
| 设备核心 | device/driver/bus/class、绑定、引用、依赖、资源回收、下线顺序 |
| 通用运行时 | DMA mapping/domain、IRQ 排空、工作队列、等待与原生 fence、固件读取 |
| GPU 子系统 | 后续实现 BO、GPUVM、上下文、任务队列、显示设备、同步 UAPI |
| 硬件驱动 | IP/固件初始化、命令协议、硬件完成中断、停止与复位 |
| 可选 Linux 源码兼容层 | 将上游源码接口映射到原生运行时，不成为原生驱动的依赖 |

内置驱动与 `.a20drv` 使用同一 `driver_t`。本阶段增加 `driver_t.quiesce`，驱动 ABI 更新为 **9**，旧模块必须重建。新增 DMA/fence/workqueue/firmware 函数已加入模块导出表。此处没有承诺跨内核版本稳定的二进制 ABI。

## 下线与资源生命周期

成功绑定的设备按以下顺序解除绑定：

1. 先解除 consumer/child，再处理 provider/parent。
2. 状态变为 `REMOVING`，`class_device_mark_offline()` 拒绝新类调用，并执行一次 offline observer。
3. 调用可选 `driver_t.quiesce(dev)`。此时既有 class 调用可能尚未返回，`drv_priv`、IRQ、DMA 和受管资源仍然有效。
4. `class_device_unpublish()` 撤销 registry publication，等待在途 class 调用结束。
5. 调用 `remove()`，逆序释放受管资源及依赖，清空绑定。

`quiesce` 必须先禁止新的硬件提交，排空或停止/复位硬件，再让未完成请求成功结束或以错误结束。驱动负责同步中断和取消其后台工作；`remove` 才释放请求队列、DMA mapping 和实例存储。驱动私有入口也必须检查自己的 stopping 状态，class offline 不会自动保护绕过 class adapter 的调用。

不能依赖 `remove()` 唤醒所有在途请求：核心在进入 `remove` 前已经等待这些调用。没有异步操作的现有驱动可以保持 `quiesce=NULL`。

成功 probe 后 class 发布失败也会执行 `quiesce -> remove -> devres`。失败的 probe 仍自行撤销已经启动的硬件，再返回错误；核心只回收它已登记的资源，不能判断哪些硬件阶段成功。

当前生命周期回调仍持有全局 lifecycle mutex，禁止等待需要该锁的工作或递归注销自己。quiesce 没有通用超时后强制释放内存的语义：如果不能证实 DMA 已停止，驱动不能继续释放硬件仍可能访问的存储。按设备并发的生命周期协调与通用恢复策略仍需后续开发。

## 原生 DMA domain 与页映射

头文件：`kernel/include/drivers/core/dma_mapping.h`；实现：`kernel/drivers/core/dma_mapping.c`。

- `driver_dma_domain_create(dev, ops, context, &domain)`：建立显式地址转换 domain，复制 provider ops，保留 device 引用和 DMA mask 快照。成功接管 context 的一份所有权；失败不调用 provider release。
- `driver_dma_domain_create_checked()`：接收独立的 `driver_dma_checked_ops`，允许解除映射返回错误，保留原有 provider 结构与模块 ABI。
- `driver_dma_domain_create_direct()`：仅在总线/平台已确认采用恒等 DMA 时显式使用。它不提供隔离，不能作为 IOMMU 失败时的降级，也不探测 AMD-Vi。
- `driver_dma_map_pages()`：映射调用者已持有的 order-0 RAM 页数组。页面可以物理不连续；每页获得独立的设备 DMA 地址。
- `driver_dma_page_address()`：读取设备地址，不根据 PFN 或 CPU 地址推算。地址 0 可以合法，错误由返回值区分。
- `driver_dma_mapping_sync_for_device/cpu()`：在 CPU 与设备停止访问的边界执行缓存同步。映射时自动执行初次 device 同步，成功映射解除时执行必要的 CPU 同步。
- `driver_dma_try_unmap_pages(&mapping)`：硬件停止访问后，逆序解除翻译；全部成功才同步 CPU、释放页面和 domain 引用并置空指针。失败返回负 errno 并保留 mapping，必须重试。
- `driver_dma_unmap_pages(&mapping)`：原有 infallible provider 的兼容入口；内部也保留失败对象，但不返回状态，checked provider 应使用 try 接口。

映射独立保留每一页的 frame 引用。调用者可在映射成功后释放原始页面引用，设备仍使用的 RAM 不会因此回收。页映射失败会尝试回滚之前的翻译及页面引用；checked 回滚如果失败，`driver_dma_map_pages()` 返回原始映射错误，同时通过非空 `*out` 交出仅供清理的句柄。调用者必须保留并反复 try_unmap，不能只判断返回码就丢弃输出。回滚没有完成初次 device 同步的 mapping 时不执行 CPU invalidate，避免丢失尚未交给设备的 CPU 数据。

provider 的 `map_page` 每次映射一整页，失败不得留下该页翻译或未确认的失效。旧 `unmap_page` 返回前必须完成设备地址翻译失效；checked `unmap_page` 只有返回 0 才确认失效，失败时必须保留 IOVA，且允许重试已经清除 PTE 的操作。设备地址需要页对齐且整页落在 domain 的 mask 内，违反条件时解除该页及之前映射，返回错误，绝不回退到物理地址。

解除映射一旦开始，mapping 即进入清理状态，地址查询和 cache sync 返回 `-ESHUTDOWN`，不能恢复提交；count 仍表示保留引用的页面数量。失败时全部 frame 引用保留，包括已经解除翻译的页，CPU 同步也推迟至全部成功；重试从失败页继续，不重复解除之前已成功的页。这里假定 provider 返回成功后不会再次失败地撤销该成功结果。正数错误被规范为 `-EIO`。

probe/解除绑定的错误路径同样要保留清理句柄、provider context、硬件资源和模块代码；不能返回失败后让 devres 释放失效命令仍需访问的资源。`release` 仍是不可失败的软件 context 销毁，硬件 domain detach 必须在 provider 的更高层完成协调。本接口没有自动隔离区或后台恢复 worker，也没有把 RISC-V 用户态 IOMMU 直接包装为原生 provider：其 map 失败时虽已保留页面并阻止新映射，但尚不能保证完成失效，不满足原生 map callback 的无残留契约。AMD-Vi 后端也尚未提供。

provider context、ops 所在模块及硬件资源的生命周期由 provider/驱动协调，直到最后一个 domain 引用释放。domain 引用只保持 device 对象和 context 的软件生命周期，不保持驱动绑定或硬件在线。创建/映射/提交必须与设备撤销串行化，解除绑定前停止 DMA 并销毁 mapping。

RISC-V IOMMU 命令队列的 tail 在写入 CQT 发布命令之前推进，表示槽位已经交给硬件；等待超时不会撤销所有权或让下一次提交覆盖该槽位。后续提交仍依靠 CQH 的队列满检查，CQCSR 有错误时不再发布；恢复 CQ 错误或超时后的完整设备重置仍由后续工作完成。解除映射入口在构造位图和缩小索引之前检查 64 位 IOVA 范围和页数，防止越界、移位未定义行为或高位截断造成槽位别名。这些队列修正本身不代表硬件恢复或原生 provider 接入已完成。

用户态 DMA heap 的连续块在交给 VMO 前通过 `pfa_split_allocated(head, order)` 转成每页独立的 order-0 所有权。这个操作只允许独占的、元数据一致的分配，不增加可用页计数；失败不修改元数据。请求大小不足整个块时，剩余尾页立即 frame_put。VMO 的页分配回滚、缩小和销毁也改为释放自身引用，不能用直接 free 覆盖 IOMMU 或其他持有者的 pin。转换后不得再按原始高阶块整体释放。

RISC-V 用户态 domain 映射还独立保留每个 RAM 页的 frame 引用。map 只接受调用者持有引用的已分配 order-0 RAM，先验证整个物理区间、pin 页面并保留 IOVA，再发布 PTE。失效命令失败时清除 PTE、保留 IOVA/页引用并将 domain 标为 blocked，拒绝后续 map；即使上层因返回错误而销毁 VMO，这些页也不会提前回收。unmap 失败仍保留引用，只有成功失效或安全 block/release 后才能释放；`mapped_pages` 统计包含隔离保留的页面。软件 blocked 不表示硬件 DMA 已停止。

设备 context 安装失败同样先尝试安全 block，无法确认时保留三层页表及 owner。原 owner 再次 claim 会先重试释放失败 claim 的页表，成功后才建立新 claim；已经成功 claim 后进入 blocked 状态的 domain 必须显式 release。release 调用者先停止 DMA/bus mastering；失败仍保留全部资源。当前没有后台回收器，硬件持续故障时这些有界资源可能一直保留，不能以回收内存为由绕过确认。

限制：仅支持完整的、已分配的 order-0 RAM 页。不是任意物理地址映射接口，不接收 GOP/VRAM、未物化 VMO 页或高阶分配的尾页。没有部分页、segment 合并、bounce、通用 IOMMU 硬件后端或缓存一致性新后端。缓存维护复用已有架构 hook，架构声明未就绪时拒绝创建/映射。旧 `dma_alloc_coherent()` 及用户态驱动路径保持既有行为。

## 原生异步 fence

头文件：`kernel/include/drivers/core/fence.h`；实现：`kernel/drivers/core/fence.c`。

`driver_fence_create()` 创建 pending 对象。`status()` 返回 0 表示 pending、1 表示成功、负 errno 表示终止错误。`signal(fence, 0)` 完成成功；负 errno 表示失败；竞争 signal 中只有第一次生效，后续返回 `-EALREADY`。signal 不分配内存，可以在 IRQ 中调用，唤醒不发生在 fence 自旋锁下。

`driver_fence_wait(fence, deadline, interruptible)` 使用既有 Park/Wake 和 wait queue，deadline 是 `clock_get_ticks()` 同单位的绝对截止时间，0 表示无限等待。成功返回 0；失败返回 producer 的错误；等待期限到返回 `-ETIMEDOUT`；中断返回 `-EINTR`；无法建立等待返回 `-EAGAIN`。最后一次检查已可见的终止结果优先于超时。

超时或等待者退出不完成 fence、不取消设备任务，也不允许释放 DMA 页面。请求队列和完成中断的 producer 必须持有自己的 fence 引用，直到完成处理返回。等待者持有独立引用，wait 过程再额外保留一份。IRQ producer 不执行最后一次 put；由任务上下文回收对象。

典型请求生命周期：

1. 准备 RAM、建立 DMA mapping、创建 fence，队列保留 mapping 和 producer fence 引用。
2. 提交硬件；等待者可等待或超时返回，队列继续持有资源。
3. IRQ 确认硬件完成后 signal；异常路径须先确认硬件停止再 signal 错误。
4. 在任务上下文同步/解除映射，释放队列引用和请求存储。

这是原生内核同步基础，不是 Linux `dma_fence` 的结构兼容实现；没有接入 DRM syncobj、timeline、sync_file 或用户态等待 ABI。现有 DRM 能力位不会因此自动改变。

## 后台工作、延迟任务与销毁

头文件：`kernel/include/drivers/core/workqueue.h`；实现：`kernel/drivers/core/workqueue.c`。

`driver_workqueue_create()` 在调度器启动后创建队列，每队列一个独立 detached 内核 worker，同队列回调串行执行。worker 不继承用户文件或信号状态。队列启动登记不在锁内分配任务，失败的任务创建不会留下可被 worker 领取的队列。

`driver_work_create(queue, callback, context)` 创建固定属于该队列的工作项。创建者和并发 producer 持有各自引用；pending/执行中的工作由队列独立持有引用。即使创建者先 put，工作也不会提前释放。回调和 context 的有效期仍由驱动保证，引用计数不会固定驱动模块代码。

| 操作 | 语义 |
|---|---|
| `driver_work_schedule(work, deadline)` | IRQ-safe、不分配内存；新排队返回 1，已有 pending 返回 0 并保持原 deadline |
| `driver_work_reschedule(work, deadline)` | IRQ-safe；重新定位 pending 项并替换 deadline，新排队返回 1、替换返回 0；提前 deadline 会唤醒 worker |
| `driver_work_cancel_sync(work, deadline)` | 移除 pending 项，取消期间拒绝提交，等待运行中的回调返回 |
| `driver_workqueue_flush(queue, deadline)` | 等待全部 pending（包含延迟项）及运行中的工作；不是禁止后续提交的屏障 |
| `driver_workqueue_destroy(&queue, deadline)` | 永久关闭提交、取消 pending、等待 worker 退出；成功消费 owner 引用并置空指针 |

所有 deadline 都是与 `clock_get_ticks()` 相同单位的绝对值，0 表示立即提交或无限等待。回调可给自身排一次后续执行；对运行中的自身 cancel、flush 自己的队列、destroy 自己的队列返回 `-EDEADLK`。取消期间提交返回 `-EBUSY`，销毁开始后提交返回 `-ESHUTDOWN`。

销毁超时返回 `-ETIMEDOUT` 并保留 queue 指针、回调和 context，允许稍后重试。存活的 work 引用继续保留 offline queue 的软件对象，所以销毁后通过 work 提交会得到错误，不会引用已释放的 queue。销毁和重试由同一个 owner 串行执行；创建工作项、flush 等使用 queue 指针的操作不得与 owner 的最终销毁竞争。

cancel 成功后其他 producer 仍可重新提交，释放 context 前必须先停止这些 producer。quiesce 应先禁止新请求，排空或停止/复位硬件，并解除回调依赖的完成等待，同时保留回调仍访问的存储；随后停止 IRQ 来源并 `free_irq()`，销毁/排空工作队列，最后让 remove 释放实例存储。不能先关掉回调所依赖的完成中断，再无限等待该回调退出。回调不得等待同队列上的后续工作，也不能在持有其需要的锁时同步取消它。跨队列依赖环由驱动避免；这里没有完整 Linux workqueue ABI、共享 worker 池或通用死锁检测器。

## IRQ 完整分发排空

`synchronize_irq(irq)` 在任务上下文等待已进入核心的分发结束，包含 handler、共享 handler 遍历以及控制器 EOI/重启用收尾。它不删除注册，也不排空工作队列，不能阻止未来或尚未进入核心的中断。调用者应先停止设备中断源，不能从同一 IRQ handler 中调用或持有 handler 需要的锁。越界 IRQ 返回 `-EINVAL`。

`free_irq()` 先撤销指定 owner，再使用同一排空原语，最后保持没有剩余注册的线路屏蔽，避免旧分发的 level rearm 让空线路重新开启。共享 handler 晋升为 primary 时保留其完整注册 flags。IRQ 仍在注册时，单独 synchronize 不授权释放 handler 的私有存储；最终回收应依靠 free_irq 或外部保证不再分发。

此接口当前是有界 handler 契约下的忙等排空，没有 IRQ 域重构、自动线程化 handler 或通用设备 IRQ 隔离。

## 固件数据与显式来源

头文件：`kernel/include/drivers/core/firmware.h`；实现：`kernel/drivers/core/firmware.c`、`firmware_vfs.c`。这些接口只在可睡眠的任务上下文调用，不能在 IRQ 或自旋锁中加载固件。

- `driver_firmware_source_create(ops, context, &source)` 复制 provider ops；成功接管 context 的一份所有权，失败由调用者处理。每次成功 open 都恰好 close 一次，包括尺寸、内存分配和读取失败。provider 支持独立 stream 并发；代码和 context 的有效期持续到最后一次 source put。
- `driver_firmware_load(source, name, max_size, &image)` 从一个显式来源加载完整固件，失败置空输出并回收部分数据。调用者必须持有 source 引用。没有隐式搜索路径、后备来源或用户态 helper。
- 名称是最多 255 字节的相对路径，允许子目录；拒绝绝对路径、空组件、`.`、`..`、重复或结尾斜线。文件长度必须非零，同时不超过调用者的 `max_size` 和 64 MiB 硬上限。
- 每次读取最多 64 KiB，支持正数短读；提前 EOF、返回超出请求长度、声明长度后仍有数据都视为 `-EIO`，负 errno 原样传回。空文件返回 `-ENODATA`，过大返回 `-EFBIG`。不会因中断错误无限重试，也没有通用读取超时或取消功能。
- `driver_firmware_from_memory(data, size, max_size, &image)` 为内置固件建立独立副本，不保留原始指针。image 通过 get/put 共享不可变字节，与 source 和原始文件的生命周期独立。

VFS 来源由可信启动配置打开固件目录，再调用 `driver_firmware_source_create_vfs(directory, &source)` 创建。函数保留已打开目录的 vfile 引用及其既有 mount pin；调用者可立即关闭 fd。后续加载直接从该目录 vnode 查找实际子项，不重新解析当前线程的 cwd、chroot 或 mount namespace，也不把临时固件文件安装进调用者的 fdtable。

该后端不遍历挂载表、不进入子挂载，lookup 返回其他 mount 的节点时拒绝；中间和最终 symlink 均拒绝，特殊文件在 open 前通过 type 和 inode mode 双重检查。固件包应部署为该目录同一文件系统内的普通、未压缩文件，别名需要预先展开。内核读取有意不按当前任务的 DAC 权限重新授权；目录、文件及其写入者必须可信，这不是面向不可信可变目录的路径沙箱。

一次 VFS open/read/close 全程保留 fileop admission，阻止关机并发关闭后端；已经关闭 admission 或 backend 的来源返回 `-ESHUTDOWN`。普通错误路径释放临时 vnode、vfile、mount pin 和 admission。source 引用不意味着已关闭的文件系统可以恢复服务。

image 数据是普通 CPU RAM，只有持有 image 引用时可访问；需要 DMA 的驱动应另行准备可映射页面并完成同步，不能把 image 指针当作设备地址。完整复制并不提供对并发写入的原子快照、固件签名认证、格式验证或解压，部署时应保证文件不可变。设备固件协议、上传、启动超时和恢复仍归硬件驱动负责。接口也不实现 Linux `request_firmware()` 的全部语义。

## 验证入口与范围

```sh
python3 -m unittest tools.tests.test_driver_quiesce tools.tests.test_driver_dma_mapping tools.tests.test_driver_fence tools.tests.test_driver_workqueue tools.tests.test_driver_irq_sync tools.tests.test_driver_firmware tools.tests.test_riscv_iommu_queue -v
make ARCH=x86_64 BOARD=qemu-virt-x86_64 kernel-only -j4
make smoke-driver-lifecycle-x86_64 -j4
tools/a20 test smoke-driver-runtime-x86_64
tools/a20 test smoke-driver-runtime-smp2-x86_64
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only -j4
```

主机测试编译生产 C 并使用 ASan/UBSan。quiesce 测试运行真实设备核心和 class registry，覆盖在途调用、发布失败、重新绑定和无 class 设备。DMA 测试替代硬件翻译/页分配器，覆盖非恒等地址、96 页映射、页面存活、掩码、逐页失败回滚、分配失败和架构未就绪。fence 测试以 pthread 替代调度器，注入 prepare/commit 边界，并覆盖 80 等待者与竞争 signal。

QEMU 生命周期门禁还保留一个在途 class 调用，由 quiesce 结束它，验证生产内核能继续完成 unregister。新增运行时门禁通过显式 `driver.runtime_test=1` 在 init_kthread 中测试实际内核 worker 与 Park/Wake，验证延迟/重设、取消、自重排、销毁超时保留对象和重试。测试成功自动关机，普通启动不执行。它们没有执行实体 GPU DMA，也没有证明 AMD/NVIDIA 硬件初始化成功。主机 fence 测试中的调度和 IRQ 是替代后端，不应声称为真实 IRQ/SMP 调度压力验证。

## 下一阶段

1. 固件目录的系统级部署与启动交接、可选线程化 IRQ，以及工作队列资源/故障策略。
2. 通用 IOMMU domain 接口及 x86 AMD-Vi 后端，逐步统一内核和用户态驱动的地址契约。
3. GPU 每设备实例、BO 驻留/固定、GPUVM、上下文与请求队列，明确与原生 fence 的引用关系。
4. Phoenix 固件/IP 初始化、SDMA 操作及像素回读，再接上 Mesa 的 AMD UAPI 和显示呈现。

以上是待实现工作，不能以头文件预留或本阶段测试代替实现证据。

本轮验证：上述三项主机测试和 x86_64 生命周期 QEMU 门禁通过；x86_64 默认部署、AArch64 embedded 内核构建通过。AArch64 generic 构建在用户态依赖阶段因 `external/sbase/id.c` 缺失失败，未作为完整 generic 构建通过。文档导航检查在加入本页索引后仅剩原工作区已有的四个未索引文档，原生接口测试还在独立导出的提交树上通过，避免依赖工作区其他未提交修改。验证代码提交为 `a25b51a85`（包含前置生命周期提交 `aedf811ef`）。

本轮新增主机测试使用实际工作队列/IRQ 核心代码，pthread 仅替代任务和硬件后端：覆盖并发启动、两个并发取消者、deadline 提前、超时销毁重试、EOI 阻塞期间同步等待及共享 handler 的存储保留。IRQ 测试的阻塞 handler 仅用于制造测试窗口，不授权真实 ISR 睡眠。

本轮验证代码提交：`1d88ac8ab`（工作队列、显式启动自检及单核/双核实例）、`5909bf80b`（IRQ 排空及共享 flags）。两项新增主机测试与前置 quiesce/DMA/fence 回归在独立导出的提交树中通过；QEMU 单核、双核均输出 `DRIVER_RUNTIME: PASS delayed=1 retime=1 cancel=1 requeue=1 destroy_timeout=1 park_wake=1`；AArch64 embedded 构建通过。普通启动不含该参数时不进入自检。这里的双核门禁是功能验证，不声明所有回调必然跨 CPU 执行或已完成 SMP 压力测试。

固件读取实现提交：`bf402c953`；真实 VFS 自检提交：`68c3e9aee`。新增固件主机测试与前置 7 项回归在独立导出的待提交树上通过（共 8 项）；覆盖字节副本、短读、提前 EOF、长度增长、错误/分配失败回滚、目录引用、特殊文件拒绝与关机 admission 释放。QEMU 单核、双核均输出包含 `firmware=1` 的完整 PASS 标记，验证关闭目录 fd 后加载、拒绝符号链接、超限失败，以及释放 source/删除文件后 image 仍保留字节。AArch64 embedded 构建通过。固件测试使用人工字节，没有加载厂商 GPU 固件或执行硬件上传。

DMA 可失败失效提交：`67fccfdf1`。主机测试执行生产 mapping 代码，注入解除映射超时、部分成功后重试、映射失败回滚超时和返回地址非法后的清理超时，检查全部 pin/domain 保留、成功页不重复解除、清理状态拒绝地址/sync 及 CPU 同步时机。该测试与前置 7 项回归在独立导出的待提交树上通过，x86_64 与 AArch64 embedded 构建通过。这里的失效由测试 provider 模拟，不表示硬件 IOMMU 超时恢复已验证。

RISC-V IOMMU 队列与解除映射边界提交：`e985ff07f`。新增主机测试使用生产 `riscv_iommu.c` 的命令函数和解除映射函数，以假 MMIO 模拟超时后的延迟消费、满队列、回绕与 CQ 错误，同时验证超大页数、高位 IOVA 截断和最后一个合法页槽位。独立导出的待提交树上共 9 项主机回归通过，启用 ASan/UBSan。标准 `tools/a20 test smoke-iommu-discovery` 在 `ufsd-rv` 用户态链接处因既有 `klog_write_ex` 未定义而失败；不能声称此门禁通过。该构建生成的当前 RISC-V 内核直接用 QEMU `virt`、`riscv-iommu-pci` 和 `driver.runtime_test=1` 启动，观察到 `translation domain verified`、未映射 IOVA 故障及包含 `firmware=1` 的运行时 PASS 标记并关机。启动中其他模块也有同一日志符号的加载失败，因此该验证只覆盖 IOMMU 初始化/翻译和驱动运行时，不覆盖完整用户态或所有模块。实际硬件队列超时恢复尚未验证。

页所有权转换提交：`a6f0d385b`；IOMMU 数据/页表保留提交：`51d0c7f48`。PFA 生产函数提取测试验证独占块转换、共享/错误 order 拒绝、引用保留和最终 buddy 合并；17 项 VMO Rust 主机测试通过。独立导出的提交/待提交树中 9 项驱动主机回归通过，IOMMU 测试注入 context 发布后的命令错误、map/unmap/release 超时，模拟原始数据引用被销毁，检查数据与页表保留及恢复重试。最终 RISC-V 内核构建与直接 QEMU 启动通过，观察到翻译验证、未映射地址故障和运行时 PASS。该启动沿用显式 `driver.runtime_test=1`；没有执行用户态 DMA 分配 syscall，也未注入实际硬件超时，不能代替端到端用户态 DMA 或实体硬件恢复验证。标准用户态门禁仍受前述日志符号问题影响。

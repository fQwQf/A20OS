# 原生驱动运行时：GPU 开发的第一阶段基础

本文区分已经接入源码的接口和后续工作。已提供设备 quiesce、页级 DMA mapping、原生异步 fence、可取消的工作队列和 IRQ 排空接口；不表示 AMD/NVIDIA 硬件加速已经可用。接口适用于 hosted 内核，MCU profile 不编入 DMA mapping/fence 实现。

## 分层与边界

| 层次 | 职责 |
|---|---|
| 设备核心 | device/driver/bus/class、绑定、引用、依赖、资源回收、下线顺序 |
| 通用运行时 | DMA mapping/domain、IRQ 排空、工作队列、等待与原生 fence；后续扩展固件加载 |
| GPU 子系统 | 后续实现 BO、GPUVM、上下文、任务队列、显示设备、同步 UAPI |
| 硬件驱动 | IP/固件初始化、命令协议、硬件完成中断、停止与复位 |
| 可选 Linux 源码兼容层 | 将上游源码接口映射到原生运行时，不成为原生驱动的依赖 |

内置驱动与 `.a20drv` 使用同一 `driver_t`。本阶段增加 `driver_t.quiesce`，驱动 ABI 更新为 **9**，旧模块必须重建。新增 DMA/fence 函数已加入模块导出表。此处没有承诺跨内核版本稳定的二进制 ABI。

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
- `driver_dma_domain_create_direct()`：仅在总线/平台已确认采用恒等 DMA 时显式使用。它不提供隔离，不能作为 IOMMU 失败时的降级，也不探测 AMD-Vi。
- `driver_dma_map_pages()`：映射调用者已持有的 order-0 RAM 页数组。页面可以物理不连续；每页获得独立的设备 DMA 地址。
- `driver_dma_page_address()`：读取设备地址，不根据 PFN 或 CPU 地址推算。地址 0 可以合法，错误由返回值区分。
- `driver_dma_mapping_sync_for_device/cpu()`：在 CPU 与设备停止访问的边界执行缓存同步。映射时自动执行初次 device 同步，成功映射解除时执行必要的 CPU 同步。
- `driver_dma_unmap_pages(&mapping)`：硬件停止访问后，逆序解除翻译，再释放页面和 domain 引用，并置空 mapping 指针。

映射独立保留每一页的 frame 引用。调用者可在映射成功后释放原始页面引用，设备仍使用的 RAM 不会因此回收。任何页映射失败都会回滚之前的翻译及页面引用；回滚没有完成初次 device 同步的 mapping 时不执行 CPU invalidate，避免丢失尚未交给设备的 CPU 数据。

provider 的 `map_page` 每次映射一整页，失败不得留下该页翻译；`unmap_page` 返回前必须完成设备地址翻译失效。设备地址需要页对齐且整页落在 domain 的 mask 内，违反条件时解除该页及之前映射，返回错误，绝不回退到物理地址。

provider context、ops 所在模块及硬件资源的生命周期由 provider/驱动协调，直到最后一个 domain 引用释放。domain 引用只保持 device 对象和 context 的软件生命周期，不保持驱动绑定或硬件在线。创建/映射/提交必须与设备撤销串行化，解除绑定前停止 DMA 并销毁 mapping。

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

## 验证入口与范围

```sh
python3 -m unittest tools.tests.test_driver_quiesce tools.tests.test_driver_dma_mapping tools.tests.test_driver_fence tools.tests.test_driver_workqueue tools.tests.test_driver_irq_sync -v
make ARCH=x86_64 BOARD=qemu-virt-x86_64 kernel-only -j4
make smoke-driver-lifecycle-x86_64 -j4
tools/a20 test smoke-driver-runtime-x86_64
tools/a20 test smoke-driver-runtime-smp2-x86_64
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only -j4
```

主机测试编译生产 C 并使用 ASan/UBSan。quiesce 测试运行真实设备核心和 class registry，覆盖在途调用、发布失败、重新绑定和无 class 设备。DMA 测试替代硬件翻译/页分配器，覆盖非恒等地址、96 页映射、页面存活、掩码、逐页失败回滚、分配失败和架构未就绪。fence 测试以 pthread 替代调度器，注入 prepare/commit 边界，并覆盖 80 等待者与竞争 signal。

QEMU 生命周期门禁还保留一个在途 class 调用，由 quiesce 结束它，验证生产内核能继续完成 unregister。新增运行时门禁通过显式 `driver.runtime_test=1` 在 init_kthread 中测试实际内核 worker 与 Park/Wake，验证延迟/重设、取消、自重排、销毁超时保留对象和重试。测试成功自动关机，普通启动不执行。它们没有执行实体 GPU DMA，也没有证明 AMD/NVIDIA 硬件初始化成功。主机 fence 测试中的调度和 IRQ 是替代后端，不应声称为真实 IRQ/SMP 调度压力验证。

## 下一阶段

1. 固件读取服务、可选线程化 IRQ，以及工作队列资源/故障策略。
2. 通用 IOMMU domain 接口及 x86 AMD-Vi 后端，逐步统一内核和用户态驱动的地址契约。
3. GPU 每设备实例、BO 驻留/固定、GPUVM、上下文与请求队列，明确与原生 fence 的引用关系。
4. Phoenix 固件/IP 初始化、SDMA 操作及像素回读，再接上 Mesa 的 AMD UAPI 和显示呈现。

以上是待实现工作，不能以头文件预留或本阶段测试代替实现证据。

本轮验证：上述三项主机测试和 x86_64 生命周期 QEMU 门禁通过；x86_64 默认部署、AArch64 embedded 内核构建通过。AArch64 generic 构建在用户态依赖阶段因 `external/sbase/id.c` 缺失失败，未作为完整 generic 构建通过。文档导航检查在加入本页索引后仅剩原工作区已有的四个未索引文档，原生接口测试还在独立导出的提交树上通过，避免依赖工作区其他未提交修改。验证代码提交为 `a25b51a85`（包含前置生命周期提交 `aedf811ef`）。

本轮新增主机测试使用实际工作队列/IRQ 核心代码，pthread 仅替代任务和硬件后端：覆盖并发启动、两个并发取消者、deadline 提前、超时销毁重试、EOI 阻塞期间同步等待及共享 handler 的存储保留。IRQ 测试的阻塞 handler 仅用于制造测试窗口，不授权真实 ISR 睡眠。

本轮验证代码提交：`1d88ac8ab`（工作队列、显式启动自检及单核/双核实例）、`5909bf80b`（IRQ 排空及共享 flags）。两项新增主机测试与前置 quiesce/DMA/fence 回归在独立导出的提交树中通过；QEMU 单核、双核均输出 `DRIVER_RUNTIME: PASS delayed=1 retime=1 cancel=1 requeue=1 destroy_timeout=1 park_wake=1`；AArch64 embedded 构建通过。普通启动不含该参数时不进入自检。这里的双核门禁是功能验证，不声明所有回调必然跨 CPU 执行或已完成 SMP 压力测试。

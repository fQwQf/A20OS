# 原生驱动运行时：GPU 开发的第一阶段基础

本文区分已经接入源码的接口和后续工作。已提供设备 quiesce、资源占用与物理存储保留、页级 DMA mapping、原生异步 fence、可取消的工作队列、IRQ 排空和有界固件读取接口；不表示 AMD/NVIDIA 硬件加速已经可用。DMA/fence/workqueue/firmware 和本页运行时自检面向 hosted 内核；资源占用接口随完整 driver core 编入，相应 MCU 配置不包含 hosted 异步运行时。

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

## 资源占用与受管映射

`kernel/drivers/core/driver_core.h` / `driver_core.c` 提供 owner 绑定的地址区间申请，并通过模块导出表供 `.a20drv` 使用，没有修改 `device_t` / `driver_t` 布局。

- `driver_resource_request(owner, resource, &claim)`：复制 inclusive type/start/end，保留一份 device 引用，返回显式句柄。同一 namespace 中不同 owner 的区间重叠返回 `-EBUSY`；同一 owner 可持有重叠的独立句柄。MMIO 还必须避开物理存储保留区，该限制没有同 owner 豁免。支持 `RES_MMIO` 和 16-bit `RES_IOPORT`，二者使用独立 namespace；其他类型或反向区间返回 `-EINVAL`。申请失败清空输出，分配失败返回 `-ENOMEM`，无法保留 owner 引用返回 `-ENODEV`。
- `driver_resource_request_checked(owner, resource, &claim)`：owner 必须已注册。在生命周期 mutex 内核对其他已发布设备的地址声明及 active claims，再建立占用，避免检查与设备发布之间的空隙。MMIO 同时核对 `RES_MEM` 物理区间，port I/O 保持独立；未绑定设备也参与检查，畸形清单返回 `-EINVAL`，未注册/正在移除的 owner 返回 `-ENODEV`。同 owner 的声明不构成冲突，parent/window 则没有隐式豁免。句柄存续期间，冲突的新设备注册返回 `-EBUSY`；owner 注销不会自动撤销保留句柄。
- `driver_resource_release(claim)`：移除占用并释放 owner 引用；NULL 无操作，非空句柄必须准确释放一次。显式句柄不会在 unbind 时自动释放，适合在硬件清理失败时继续保留 aperture。申请时调用者须持有 owner 的有效引用，成功后由 claim 保留设备存储；调用者继续持有有效 claim 句柄，并自行协调 stopping 状态。device pin 不等于有效绑定或模块代码保留。
- `device_claim_resource(dev, resource)`：只申请占用并登记 devres 回收，resource 必须是 `dev->res` 的元素，适用于 MMIO 或 port I/O。登记动作分配失败会立即撤销申请。
- `device_map_resource_exclusive(dev, resource, &mapping)`：只接收所属设备的 MMIO 资源。先申请占用再调用 architecture mapper，返回精确错误并在失败时清空输出。长度超出 `size_t` 返回 `-EOVERFLOW`；mapper 返回 NULL 记为 `-ENOMEM`。映射或 devres 登记失败立即回滚；成功映射的占用持续到 architecture unmap 返回之后。

现有 `device_map_resource()`、`device_map_mmio()` 及 `pci_map_bar()` 已接入相同申请路径；它们仍以 NULL 表示失败，不会在冲突时绕开登记重新映射。显式 claim 和同 owner 的受管映射拥有各自的引用，提前释放其中一份不会解除其余占用。正常 unbind 在 quiesce、class drain、remove 之后逆序回收受管映射；probe 失败也会回收。受管申请及 devres 链更新须处于串行化的 probe/lifecycle 上下文，不能与同设备的 devres 回收并发修改链表。显式句柄的全局冲突检查/释放使用 registry spinlock；分配、owner finalizer 和 MMIO 回调在该锁之外执行。

基础 claim 列表仍是参与者之间的占用登记；普通申请/受管映射不自动将未绑定设备的声明变成 busy，checked 申请才额外核对当前已发布资源。启动已导入 PFA 受管 RAM 与 ACPI 表所在页作为独立存储保留区；未覆盖未纳入 PFA 的 RAM、全部 DT/其他固件保留区，也未实现 PCI host window 树或 page cache 属性仲裁。raw architecture 映射、旧 `drv_map_mmio()` 和直接 port I/O 仍不自动登记。x86 architecture unmap 的共享永久映射契约仍然有效，claim 释放不撤销旧指针。AMD-Vi 的只读报告本身不持有 claim；独立的被动 PCI 驱动已通过 checked 接口保留完整 aperture，见下文。其他未导入区间的核对及 AMD-Vi DMA 隔离仍待完成。

`driver_resource_check_range(owner, resource, &busy_end)` 是观察接口，核对当前已发布声明、active claims 和物理存储保留区；owner 为 NULL 时不忽略任何设备。`driver_resource_check_claims()` 核对 active claims 与物理存储保留区，供处理已有固件 BAR 等场景使用；两者均不忽略存储保留区。`RES_MEM` 声明按物理 MMIO namespace 核对；busy_end 仅在 `-EBUSY` 时给出最大冲突 inclusive end，其他返回清零，端点 0 仍可合法。查询不建立 lease。总线必须在 `driver_discovery_begin/end` 事务中完成查询、硬件变更与清单更新；checked 申请使用相同 lifecycle mutex。绕开事务修改 `dev->res` 或直接重编程硬件不受这项保证保护。

PCI 分配新 MMIO 地址时先避开已有 PCI BAR，再按 busy_end 跳过其他资源声明/占用，窗口耗尽或迭代上限返回错误。BAR 重建和用户态驱动设备查询均与 discovery 串行化。任何已有非零 BAR 都先核对 active claims 与物理存储，再调用可能建立 CPU 映射的 reachability hook；不把可能存在包含/别名关系的全部固件声明一律当作 active user。新地址也先查冲突、后调用 hook。实现先在栈上规划全部 BAR/资源并验证，再提交地址及元数据；规划失败时没有已移动的前序 BAR，旧 command 和资源表保持不变，单调分配游标可以消耗空间。重建路径的 sizing 期间关闭 decode/BME，不能代替调用者先停止硬件，亦不声称解决不可报告的 PCI 写失败、热拔出或在途 DMA 排空。

显式 `driver.runtime_test=1` 会在生产内核执行 metadata-only 的资源自检，输出 `DRIVER_RESOURCES: PASS claims=1 conflicts=1 owner_refs=1 devres=1 inventory=1 memory=1`。该自检不访问测试区间的物理地址，验证重复句柄、冲突、引用、受管申请回收，以及未绑定声明/后续发布与 checked 申请的互斥、存储保留与 MMIO/发布冲突、`RES_MEM` 描述允许共存和实际受管 RAM 申请拒绝。并发申请、阻塞 unmap 和失败注入另由主机 fixture 验证。资源和运行时 PASS 标记在测试末尾同一条日志中输出，避免 best-effort SMP 控制台只丢失早期资源标记。

## 物理存储保留对象

头文件：`kernel/include/drivers/core/memory.h`；保留对象与 claim 共用 `driver_core.c` 的 registry 锁，启动导入位于 `kernel/drivers/core/memory_boot.c`。这是原生 CPU 物理存储清单，独立于 device/binding；RAM 无须伪装成设备，驱动结构布局和 ABI 9 保持不变。reserve/release 已进入模块导出表，启动导入函数只供内核启动路径使用。

`driver_memory_reserve(ranges, count, &reservation)` 在任务上下文复制非空数组，每项为 inclusive `start/end`。地址 0 和 `UINT64_MAX` 均有效，反向区间返回 `-EINVAL`，分配尺寸溢出返回 `-EOVERFLOW`，分配失败返回 `-ENOMEM`。输入只需在函数返回前保持有效且不可变，输出不保留借用指针。副本在锁外排序并合并相邻/重叠区间，锁内使用二分查询；不建立 Linux resource tree 或推断父窗口共享。

发布前，在 lifecycle mutex 内核对当前所有 `RES_MMIO` 声明，再在 registry 锁内核对 active MMIO claims；冲突返回 `-EBUSY`。原批次任何一项失败都不发布部分保护。`RES_MEM` 可以描述同一存储，port I/O 使用独立地址空间。多个存储保留对象可以重叠，独立释放其中一个不会撤销另一个的保护。畸形的已发布资源表返回 `-EINVAL`。

对象存续时，基本/checked claim、受管 MMIO 映射、PCI 的两类范围查询和新的 MMIO 设备发布都不能覆盖该存储；同 owner 的 claim 例外不适用。保留发布与基本 claim 共享 registry 锁，与设备发布共享 lifecycle 协调，所以并发竞争只能一方成功。已经持有 claim 的映射即使正在 unmap，也会阻止新存储保留；仍须先完成 unmap，再释放 claim。

调用者持有成功句柄，恰好调用一次 `driver_memory_release()`；NULL 无操作。释放在 registry 锁内撤销保护，在锁外回收副本。对象没有 device 引用、自动 devres 回收或物理页所有权；它不会为 PFA 预留/分配页面，也不冻结 raw 映射、物理硬件或 DMA。它只约束请求的物理区间，不能替代架构实际映射粒度、缓存属性和永久别名的审查。

hosted 启动在 PFA 和 `driver_core_init()` 完成后、发布设备前调用 `driver_memory_protect_boot()`。它复制不可变的 PFA RAM 几何，并通过 `acpi_reserve_tables()` 导入 RSDP、根表、各 SDT 和 FADT 引用的 DSDT/FACS；ACPI 表的边界向外取整到物理页。先计数与校验，再填入副本，最后一次性发布；两遍不一致或任意表读取/校验失败均不留下部分保护，启动路径会停止设备初始化。没有 ACPI context 的平台仍保护受管 RAM；没有有效 RAM 清单则拒绝初始化。成功后保留对象存续至内核结束，重复调用返回 `-EALREADY`。

`DRIVER_MEMORY: protected ram_ranges=... firmware_ranges=...` 报告导入前的区间/表回调数量，内部可以合并。该导入不等于完整固件内存图：PFA 范围之外的 RAM、其他启动数据、全部 DT reserved-memory 和运行时热增内存仍需各自的可信来源；不能把所有固件 reserved 类型都误当成不可用 MMIO，也不能从“未发现冲突”推导硬件启用许可。

## 原生 DMA domain 与页映射

头文件：`kernel/include/drivers/core/dma_mapping.h`；实现：`kernel/drivers/core/dma_mapping.c`。

- `driver_dma_domain_create(dev, ops, context, &domain)`：建立显式地址转换 domain，复制 provider ops，保留 device 引用和 DMA mask 快照。成功接管 context 的一份所有权；失败不调用 provider release。
- `driver_dma_domain_create_checked()`：接收独立的 `driver_dma_checked_ops`，允许解除映射返回错误，保留原有 provider 结构与模块 ABI。
- `driver_dma_domain_create_managed()`：接收 `driver_dma_managed_ops`，把未完成的当前页清理及可失败的硬件 detach 纳入生命周期。
- `driver_dma_domain_create_direct()`：仅在总线/平台已确认采用恒等 DMA 时显式使用。它不提供隔离，不能作为 IOMMU 失败时的降级，也不探测 AMD-Vi。
- `driver_dma_map_pages()`：映射调用者已持有的 order-0 RAM 页数组。页面可以物理不连续；每页获得独立的设备 DMA 地址。
- `driver_dma_page_address()`：读取设备地址，不根据 PFN 或 CPU 地址推算。地址 0 可以合法，错误由返回值区分。
- `driver_dma_mapping_sync_for_device/cpu()`：在 CPU 与设备停止访问的边界执行缓存同步。映射时自动执行初次 device 同步，成功映射解除时执行必要的 CPU 同步。
- `driver_dma_try_unmap_pages(&mapping)`：硬件停止访问后，逆序解除翻译；全部成功才同步 CPU、释放页面和 domain 引用并置空指针。失败返回负 errno 并保留 mapping，必须重试。
- `driver_dma_unmap_pages(&mapping)`：原有 infallible provider 的兼容入口；内部也保留失败对象，但不返回状态，checked provider 应使用 try 接口。

映射独立保留每一页的 frame 引用。调用者可在映射成功后释放原始页面引用，设备仍使用的 RAM 不会因此回收。页映射失败会尝试回滚之前的翻译及页面引用；checked 回滚如果失败，`driver_dma_map_pages()` 返回原始映射错误，同时通过非空 `*out` 交出仅供清理的句柄。调用者必须保留并反复 try_unmap，不能只判断返回码就丢弃输出。回滚没有完成初次 device 同步的 mapping 时不执行 CPU invalidate，避免丢失尚未交给设备的 CPU 数据。

provider 的 `map_page` 每次映射一整页，失败不得留下该页翻译或未确认的失效。旧 `unmap_page` 返回前必须完成设备地址翻译失效；checked `unmap_page` 只有返回 0 才确认失效，失败时必须保留 IOVA，且允许重试已经清除 PTE 的操作。设备地址需要页对齐且整页落在 domain 的 mask 内，违反条件时解除该页及之前映射，返回错误，绝不回退到物理地址。

解除映射一旦开始，mapping 即进入清理状态，地址查询和 cache sync 返回 `-ESHUTDOWN`，不能恢复提交；count 仍表示保留引用的页面数量。失败时全部 frame 引用保留，包括已经解除翻译的页，CPU 同步也推迟至全部成功；重试从失败页继续，不重复解除之前已成功的页。这里假定 provider 返回成功后不会再次失败地撤销该成功结果。正数错误被规范为 `-EIO`。

managed provider 的 `map_page` 显式返回 `needs_unmap`：只要当前页已安装翻译或保留 IOVA，就设置 true，并交出可供 unmap 的地址，包含返回负 errno 的情况。核心将当前页也纳入回滚；失效再次失败时，清理句柄保留当前页及前序页面引用。返回成功却没有设置 needs_unmap 是 provider 契约错误，转为 `-EIO`；失败且 false 仍要求当前页没有硬件残留。旧 infallible/checked provider 的接口及回滚语义保持原样。

managed domain 的 `driver_dma_domain_shutdown()` 在任务上下文永久关闭新 map。有 mapping 或清理句柄时返回 `-EBUSY`；驱动先停止 DMA、解除这些 mapping，再重试 shutdown。没有剩余 mapping 后调用可失败的 `deactivate`，成功才解除内部 provider 引用。失败保留完整 context 供重试；正数错误转为 `-EIO`。shutdown 成功可重复调用，不消费调用者引用，最后再 put。遗漏 shutdown 而只 put owner 会保留内部引用，产生安全但需要恢复的资源泄漏，不能据此卸载 provider 模块或释放其硬件资源。

同一个 managed domain 的 map、unmap 和 shutdown 回调由每 domain mutex 串行化，回调可睡眠，但不得递归调用同一 domain 的这些接口，也不能在持有回调需要的锁时调用。shutdown 关闭提交后，已有 mapping 的清理仍可进行；没有强制释放或后台自动 detach。不同 domain 之间没有这把锁，旧 provider 保持独立映射并发契约。其他类的 domain 调用 shutdown 返回 `-EOPNOTSUPP`。

probe/解除绑定的错误路径同样要保留清理句柄、provider context、硬件资源和模块代码；不能返回失败后让 devres 释放失效命令仍需访问的资源。旧 provider 的 `release` 仍是不可失败的软件 context 销毁；managed provider 将可失败 detach 放入 `deactivate`，`release` 只在确认完成后销毁软件 context。本接口没有自动隔离区或后台恢复 worker，已提供显式的 RISC-V PCI managed provider，将失败页清理与硬件 detach 交给通用生命周期，而非包装成旧 map callback。AMD-Vi 后端也尚未提供。

provider context、ops 所在模块及硬件资源的生命周期由 provider/驱动协调，直到最后一个 domain 引用释放。domain 引用只保持 device 对象和 context 的软件生命周期，不保持驱动绑定或硬件在线。创建/映射/提交必须与设备撤销串行化，解除绑定前停止 DMA 并销毁 mapping。

RISC-V IOMMU 命令队列的 tail 在写入 CQT 发布命令之前推进，表示槽位已经交给硬件；等待超时不会撤销所有权或让下一次提交覆盖该槽位。后续提交仍依靠 CQH 的队列满检查，CQCSR 有错误时不再发布；恢复 CQ 错误或超时后的完整设备重置仍由后续工作完成。解除映射入口在构造位图和缩小索引之前检查 64 位 IOVA 范围和页数，防止越界、移位未定义行为或高位截断造成槽位别名。这些队列修正本身不代表硬件恢复或原生 provider 接入已完成。

用户态 DMA heap 的连续块在交给 VMO 前通过 `pfa_split_allocated(head, order)` 转成每页独立的 order-0 所有权。这个操作只允许独占的、元数据一致的分配，不增加可用页计数；失败不修改元数据。请求大小不足整个块时，剩余尾页立即 frame_put。VMO 的页分配回滚、缩小和销毁也改为释放自身引用，不能用直接 free 覆盖 IOMMU 或其他持有者的 pin。转换后不得再按原始高阶块整体释放。

RISC-V 用户态 domain 映射还独立保留每个 RAM 页的 frame 引用。map 只接受调用者持有引用的已分配 order-0 RAM，先验证整个物理区间、pin 页面并保留 IOVA，再发布 PTE。失效命令失败时清除 PTE、保留 IOVA/页引用并将 domain 标为 blocked，拒绝后续 map；即使上层因返回错误而销毁 VMO，这些页也不会提前回收。unmap 失败仍保留引用，只有成功失效或安全 block/release 后才能释放；`mapped_pages` 统计包含隔离保留的页面。软件 blocked 不表示硬件 DMA 已停止。

设备 context 安装失败同样先尝试安全 block，无法确认时保留三层页表及 owner。原 owner 再次 claim 会先重试释放失败 claim 的页表，成功后才建立新 claim；已经成功 claim 后进入 blocked 状态的 domain 必须显式 release。release 调用者先停止 DMA/bus mastering；失败仍保留全部资源。当前没有后台回收器，硬件持续故障时这些有界资源可能一直保留，不能以回收内存为由绕过确认。

`riscv_iommu_dma_domain_create(device, &domain)` 接收已发布的 PCI device，从实际 BDF 计算 requester ID，创建 managed domain。构造和 shutdown 时要求 bus mastering 已关闭；调用者还必须停止 DMA，并与驱动绑定/用户所有权变更串行化。创建不启用 bus mastering，驱动确认初始化成功后自行启用。user_owned 设备、已有 requester claim 和不受当前 DDT 覆盖的 requester 均拒绝，不回退物理地址。原生 owner 使用单调负 token，与正 PID 的用户态 owner 隔离；token 用尽返回错误，不循环重用。device 对象由 managed domain 保留。

创建失败通常置空输出，但 context 安装后失效失败且无法安全清理时，输出非空的已关闭 domain：调用者须保留，重试 shutdown 后再 put。map 通过 needs_unmap 交接当前失败页地址；unmap 和 deactivate 复用实际 IOMMU 失效路径，失败返回 `-EIO` 并保留引用。构造/关闭时 BME 未清除返回 `-EBUSY`。后端仍限 8 个 domain、每 domain 64 页和一层 DDT；没有 AMD-Vi、多 segment、ATS、页大小协商或原生故障 IRQ 派发；已提供下述 per-device 轮询和后台 watch。TO_DEVICE 页使用只读 PTE；FROM_DEVICE/BIDIRECTIONAL 使用读写 PTE（SV39 不允许只写叶 PTE）。cache sync 仍由通用 mapping 层执行。

`riscv_iommu_dma_fault_poll(device, &snapshot)` 是任务上下文的轮询接口。它验证该 device 对象确实持有当前 native requester，拒绝同 BDF 的其他软件对象，返回累计 `count`、最后 `cause/address`、`blocked`、`isolated`、`isolation_error` 和 `queue_error`。轮询会排空共享 FQ 并处理所有 owner，不只处理调用者。故障记录归属和 BME 清除/context 失效在同一 IOMMU domain 锁下串行完成，避免在旧事件处理前 release/reclaim requester。隔离命令失败时，后续轮询即使没有新记录也会重试；已成功的动作不重复执行。

`blocked` 表示软件拒绝后续 map；`isolated` 只表示 BME-disable 与 context-invalidation 命令曾完成，不证明 DMA engine/in-flight 请求已经停止，也不允许自动释放页面。调用者仍需停止/复位设备、处理未完成请求和 fence，再 unmap/shutdown。查询成功返回 0 也可能带 `isolation_error=-EIO`；没有本对象的 live native domain 或 IOMMU 已离线时返回 `-ENODEV` 并清空输出。故障 latch 不通过读取清除，恢复需销毁并重建 domain。

FQCSR 的 memory-fault/overflow 位会累计到 `queue_error`：记录可能缺失，所有活跃 domain 均进入 blocked 并尝试隔离，新 claim 拒绝，`count` 仅代表已知记录的下界。接口不清除硬件队列错误或虚构缺失地址，队列恢复/重初始化仍待开发。基础查询是同步轮询；可使用下述 watch 建立后台轮询。没有 IRQ 派发或自动 signal fence，驱动仍需检查所有字段并定义请求完成策略。

`riscv_iommu_dma_watch_start(device, period, callback, context, &watch)` 在调度器启动后建立独立 worker，保留 device 引用并固定当前 native owner token。period 是两次轮询之间的非零 native ticks，包含轮询/回调执行耗时后再计时，不是硬实时 IRQ 延迟保证。健康且未变化的快照不通知；count、地址、cause、queue/isolation 错误或状态变化才调用 callback。terminal poll 错误只通知一次并停止重排；旧 owner 被替换后返回 `-ENODEV`，不会跟随或隔离新 owner。回调拿到的快照只在调用期间有效，需要保留时复制。

callback 在该 worker 上且不持有 IOMMU/queue 锁，可 signal 原生 fence，但框架不替驱动选择请求错误、停止引擎或 reset。启动自检的 fence 只表示通知路径成功，不表示 DMA 请求成功；真实驱动须先确认引擎停止，再按任务状态结束请求。callback 可能在 start 返回前运行，context 必须预先初始化，模块/代码/context 必须保留到 stop 成功。不得在回调中等待当前绑定锁或直接发起需要该锁的绑定操作；设备生命周期回调仍可能持有该锁。

`riscv_iommu_dma_watch_stop(&watch, deadline)` 永久关闭重排并等待在途回调返回，成功释放对象、device 引用并置空指针。deadline 是绝对 ticks，0 表示无截止；超时或中断保留整个对象和 context，应在同一 owner 上串行重试。回调内自停止返回 `-EDEADLK` 且不释放。start 的部分失败也可能交出非空清理句柄，须 stop 后再丢弃。监测器不固定硬件 domain 或驱动绑定，应在 quiesce 中先 stop，再 shutdown domain/释放设备上下文。目前采用每 watch 一个 worker，没有 IRQ 触发或共享线程池。

显式 `driver.runtime_test=1 driver.iommu_native_test=1` 自检需要额外 QEMU EDU fixture，普通启动不运行。测试从已枚举、未绑定的 EDU 找到真实 device，建立原生 domain、映射两个 RAM 页，通过 IOMMU TR_REQ 验证设备地址翻译和只读权限，再检查 unmap 与 detach 后翻译故障。该路径使用 [QEMU 的寄存器定义](https://raw.githubusercontent.com/qemu/qemu/master/hw/riscv/riscv-iommu-bits.h)中 TR_REQ 的 DID/GO/NW 字段；高半部 DID 在低半部 GO 前写入，并有 busy 等待上限。启用 legacy PPN 编码兼容时，TR_REQ 响应校验只能比较旧编码保留的地址字段。进一步的 EDU 往返测试将两个物理不连续 RAM 页映射成相邻 IOVA，以 128 字节传输跨越页边界：先读入 EDU 内部缓冲，再清零 RAM 传输区并写回，逐字节验证模式以及两页非传输区域和额外 guard 页。测试传给 EDU 的是通用 mapping 返回的 IOVA，原始 RAM owner 引用在 DMA 前已经释放，页面由 mapping/backend 持有。两次传输之间依次执行 for-CPU 与 for-device 同步，确认 DMA_RUN 清除后才读回、关闭 bus mastering 和 unmap。等待同时有时钟期限和迭代上限；超时 panic 保留活跃资源。之后在持有独立 guard 引用的原 RAM 页上保留哨兵，unmap 后让 EDU 再尝试写原 IOVA；检查 FQ 新记录，由后台 watch 完成隔离并通过回调 signal fence；主线程等待 fence、stop 排空回调后，再检查 store page fault、BME 清除、隔离状态和新 map 拒绝，并验证哨兵未变。保留 guard 引用使失效回归也不会写坏已重新分配的内核 RAM。该数据路径补充了旧 TR_REQ 编码的校验缺口，但仍不代表实体硬件恢复或端到端用户态 DMA 已验证。

限制：仅支持完整的、已分配的 order-0 RAM 页。不是任意物理地址映射接口，不接收 GOP/VRAM、未物化 VMO 页或高阶分配的尾页。没有部分页、segment 合并、bounce、通用 IOMMU 硬件后端或缓存一致性新后端。缓存维护复用已有架构 hook，架构声明未就绪时拒绝创建/映射。旧 `dma_alloc_coherent()` 及用户态驱动路径保持既有行为。

## AMD-Vi 的 IVRS 发现与路由候选

`firmware/ivrs.h` 与 `kernel/firmware/acpi/ivrs.c` 提供分配无关的 IVRS 读取接口。格式依据 [AMD IOMMU Specification 3.10 第 5 章（AMD 原文镜像）](https://kib.kiev.ua/x86docs/AMD/IOMMU/48882-3.10.pdf)，并对照 [ACPICA 表定义](https://raw.githubusercontent.com/acpica/acpica/master/source/include/actbl2.h)；实现使用字节读取，没有导入 Linux/ACPICA 驱动代码。

`acpi_ivrs_begin()` 在暴露首个条目前校验整张不可变表，包括签名、长度、校验和、revision 1/2、block/device 边界、已知范围配对和内存区间溢出。支持 10h、11h、40h IVHD，以及 20h–22h IVMD；HID 设备条目的可变 UID 长度单独处理，不按固定长度跳过。未知但有界的 block/fixed entry 保留原始类型，无法确定长度的 variable entry 返回 `ACPI_IVRS_UNSUPPORTED`。校验失败清空 iterator，不把前面合法的条目交给调用者。

iterator 和 typed hardware/memory/device accessor 返回借用的表视图，使用结束后才可释放 ACPI mapping；底层读取器保留 alias、extended、HID 的原始字节和 IVMD 顺序。不同 IVHD 格式可以描述同一单元，记录数不能作为唯一硬件单元数。

`kernel/firmware/acpi/ivrs_policy.c` 在全表校验后提供两个只读查询：

- `acpi_ivrs_route(table, segment, requester, out)`：返回硬件描述、实际 DMA requester、DTE settings、extended settings、alias/non-PCI 标记及相关 IVMD 数量。同一 `(segment, IOMMU DeviceID, capability offset)` 采用 A20OS 的 `40h > 11h > 10h` 选择顺序；这是一项显式软件策略，不表示规范要求所有平台使用相同顺序。同一格式重复、base/info 矛盾、不同单元的 16-KiB MMIO 窗口重叠或查询命中多个单元返回 `ACPI_IVRS_AMBIGUOUS`。ALL 是单元内的默认属性，具体 select/range 覆盖默认；ALL 与命中的 alias 并存则报歧义。ALL 不能自动解释成任意设备均可启用；仍须核对实际 PCI 拓扑、对应单元与当次有效的硬件约束，不能使用无效 Range 字段推断覆盖。
- `acpi_ivrs_route_memory(table, segment, requester, index, out)`：按固件顺序返回属于该单元、适用于原 requester 或 alias DMA requester 的 IVMD。所有替代 IVHD 下的约束都会保留，未命中的设备区间跳过；不去重、不合并权限、不建立 unity mapping，也不执行 exclusion。

查询支持普通/extended select 与 inclusive range，以及单层 alias select/range。alias 目标须有描述且归属同一单元；缺失目标或 alias 链/循环返回 `ACPI_IVRS_UNSUPPORTED`，跨单元归属报歧义。查询匹配到 special/HID requester 时返回 unsupported；其他 requester 可以得到带 `non_pci_entries` 标记的候选，后续驱动仍须完成这类设备的绑定和策略。未选中的替代格式同样检查未知条目、保留字段和 range 邻接要求，不因选中了较新格式而静默跳过未知策略。

没有匹配返回 `ACPI_NOT_FOUND`，格式错误返回 `ACPI_INVALID`。查询失败清空输出；超过 64 个单元或 1024 个 block 返回 unsupported。输出借用不可变表映射，调用者不得释放映射后继续使用 `hardware.entries`。这些是路由候选接口：没有建立 alias 隔离组、合并共享 DTE 属性或落实 IVMD 权限，不授予启用 IOMMU/DMA 的条件。

x86 启动时调用 `amd_iommu_firmware_report()`，先完成全表校验，再记录 IVHD 的类型、segment/requester、寄存器地址及 capability offset，并释放表映射。该报告仍使用原始记录，不自动执行上述路由查询，也不读写 IOMMU 寄存器。`acpi_ivrs_unit_count()` / `acpi_ivrs_unit_get()` 则按单元首次出现的顺序枚举去重后的候选描述，复用路由查询的全表结构与条目策略检查。输出视图借用表映射；出错清空 count/view。单元枚举不代替对具体 requester 的作用域和 alias 查询。

### PCI 与 MMIO 只读探测

`drivers/core/amd_iommu.h` 与 `kernel/drivers/core/amd_iommu_probe.c` 提供 `amd_iommu_probe_unit()`。调用者提供已校验的 IVHD 和只读 transport，返回不含借用指针的 `amd_iommu_snapshot_t`。transport 的 PCI dword 读取须支持指定 segment/requester；MMIO callback 只对已开启 decoder 的单元执行 uncached、对齐的 64-bit 读取，offset 限于 control（18h）和 EFR（30h）。callback 返回负 errno，正数失败规范化为 `-EIO`；调用者负责设备、固件和映射的生命周期及串行化。

探测检查 PCI vendor 是否有效、class `08:06:00`、type-0 header、完整 capability 链以及 IVHD 指定的精确偏移。链表越界、未对齐、循环、匹配项后的损坏链尾和指向目标 capability payload 的其他 header 均拒绝；不能只取第一个 ID 为 `0Fh` 的 capability。目标须为 Secure Device/type 3、interface revision 1，并有完整的 20-byte 或 CapExt 24-byte 空间。PCI base 须与 IVHD 一致，地址宽度须为当前支持的 PA 40/48/52、VA 32/40/48/64。Range register 只描述同一 bus 的 integrated device/root port 范围，不能直接用它判断下游 requester 是否覆盖。

[QEMU 10.0 的 capability 定义](https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/i386/amd_iommu.h) 未设置 CapRev，实际报告 revision 0。实现仅为 PCI `1022:1419` 且 subsystem `1af4:1100` 的组合接受这一差异，在 snapshot 和启动日志标记 `revision_quirk=1`；其他 revision 0 或未知 revision 仍拒绝。这不证明物理 revision-0 IOMMU 的接口可用。

`mmio_enabled` 是 PCI capability 的 MMIO decoder Enable 位，不是 IOMMU translation enable。decoder 未开启时不映射、不访问 MMIO，`registers_read=false`，此时 control/efr 的零值不表示硬件实际为零。已开启时才读取 control，且仅在 EFRSup 为 1 时读 EFR。PCSup 决定 16-KiB 或 512-KiB aperture 和相应对齐；11h/40h IVHD 的 PCSup 与读到的 EFR 矛盾则拒绝。10h IVHD 没有 EFR image，若 decoder 关闭且支持 EFR，完整 aperture 尚未知，`register_size=0`，不猜测为 16 KiB。其他情况下按固件描述或读取结果返回 aperture 大小。

没有对应函数/capability 返回 `-ENODEV`；不支持的 class/header/interface/address width 返回 `-EOPNOTSUPP`；畸形链或 aperture 返回 `-EINVAL`；base 或 PCSup 矛盾返回 `-EIO`；transport 错误原样传播。错误清空整个 snapshot。探测成功不等于资源已独占或设备已接管，也不保证后续时刻的硬件状态；硬件初始化时必须重新验证。

PCI host 枚举后，x86 调用 `amd_iommu_pci_report()`，对去重候选做上述探测。当前架构 PCI HAL 没有 segment 参数，启动适配器明确拒绝非零 segment。适配器在 discovery 事务内先检查 active claims/存储保留，再按需映射首个 16-KiB 寄存器窗口并配对 unmap（x86 映射仍遵循架构既有的共享、永久映射契约）。日志报告原始 control/EFR、地址宽度和 aperture；`verified` 只统计只读探测成功的单元。该诊断不建立持久占用。下述被动驱动负责与已发布声明/claims 核对完整 aperture；下文已提供 PCI 桥路径与有效 Range 的补充查询；将这些证据与 IVRS requester/alias 归属、控制器生命周期及 IVMD 权限组合成 DMA domain provider 仍待实现。

两类报告都不建立 device table、队列或页表，不改变 DMA 路径。`driver_enabled=0` 表示这条发现路径没有启用 AMD-Vi 翻译；它不表示固件此前未开启硬件，也不表示下述软件驱动没有绑定。

`tools/a20 test smoke-amdvi-discovery-x86_64` 使用 QEMU q35 与 amd-iommu，要求发现 10h IVHD 和两条原始硬件记录，按 11h 格式探测一个去重单元成功，并完成已有运行时自检。该门禁验证固件/PCI 发现与启动集成；MMIO 已开启分支另由主机 fixture 验证，不表示 AMD-Vi DMA 隔离或实体机器 GPU 已可用。

### PCI 桥路径与 AMD Range 补充查询

`pci_device_scope_get(client, scope_bus, first_devfn, last_devfn, &anchor)` 在任务上下文检查已发布 PCI 拓扑，返回 client 本身或符合范围的上游 bridge 引用，由调用者 `device_put()`。`first/last` 包含 device 和 function 的完整 8-bit devfn，不能把它当作 slot 编号。输入 client 须有有效引用；输出失败清空。查询不重新枚举、不写配置/BAR、不建立 `device_t.parent` 或隐式依赖，也不创建新的 group 编号。

`device_is_registered(dev)` 在 registry 锁下观察核心成员关系，不增加引用，与绑定/state 独立。PCI 查找与作用域查询均核对核心注册状态；即使私有 PCI 记录尚未更新，已注销但被引用保留的 client/bridge 也不能继续作为已发布节点使用。rescan 可重新发布该 function，外部引用仍保留旧元数据。

实现于 `kernel/drivers/bus/pci_bus.c`。它在 discovery 事务中读取当前已发布 function 的身份与 type-1 bridge 的 primary/secondary/subordinate，按唯一 secondary 寻找上游路径；每一级窗口须包含原 client bus。所有能够解码该 client bus 的 bridge 都必须属于该路径，不能取范围最小或最宽的任意 bridge 来消解重叠。缺失中间桥不会只凭祖先 subordinate 范围猜测连接。

没有 client/匹配 scope 返回 `-ENODEV`；重叠 sibling 或重复父桥返回 `-EBUSY`；反向范围、primary 不符及畸形窗口返回 `-EINVAL`；路径循环返回 `-ELOOP`。当前全局视图中任何已发布 function 的 live PCI ID 与元数据不一致都会拒绝查询，避免悄悄省略消失/替换的桥；应先通过正常 rescan 调和设备集。不可读 header 返回 `-EIO`，不支持的 bridge class 返回 `-EOPNOTSUPP`。未配置的零 secondary/subordinate 不作为路由。

`kernel/drivers/core/amd_iommu_scope.c` 的 `amd_iommu_scope_get(snapshot, client, &anchor)` 消费已验证的只读 snapshot，仅提供 Range 寄存器的补充证据。按 [AMD IOMMU 3.10 的 Capability Offset 0Ch](https://kib.kiev.ua/x86docs/AMD/IOMMU/48882-3.10.pdf)，有效范围描述同一 bus 上的集成设备与 root port；下游 client 必须通过上述桥路径落到范围内的 PCIe root port。辅助查询检查完整 capability 链、唯一 PCIe capability、version 1/2、Root Port 类型及所需结构边界，链尾损坏、循环、重复或 payload 内的其他 header 均拒绝。PCIe 字段布局另对照 [QEMU 固定版本的 PCI 定义](https://github.com/qemu/qemu/blob/v10.0.0/include/standard-headers/linux/pci_regs.h)，未导入其实现代码。

`RngValid=0` 时规范要求使用 I/O 固件拓扑；该辅助函数返回 `-EOPNOTSUPP`，不将未定义字段当作覆盖，也不表示 IOMMU 本身不受支持。真正的 provider 还需结合 IVRS 路由。非零 segment 仍拒绝；没有覆盖返回 `-ENODEV`，不是 root port/未知版本返回 `-EOPNOTSUPP`，畸形 capability 返回 `-EINVAL`。硬件无效范围的拒绝分支已加入被动 owner 的显式自检。

两项函数均只返回带 device 引用的观察结果，已导出给模块。引用保留元数据，不冻结物理拓扑、配置寄存器、驱动绑定或控制器 snapshot。需要一致的 lookup/config/binding 观察时，调用者须用外层 discovery 事务覆盖操作，并在硬件启用前重新验证。结果不证明 ACS、peer-to-peer、共享 requester/alias 隔离，也不授予 DMA 或修改 DTE 的权限。

`tools/a20 test smoke-pci-scope-x86_64` 提供 root port 下 bus 1 的 EDU，显式参数 `driver.runtime_test=1 driver.pci_scope_test=1` 执行身份与桥路径查询，通过后在资源 PASS 行追加 `pci=1`。实例同时运行被动 AMD-Vi owner 自检；普通启动不执行这项测试。查询不会给 EDU 分配 BAR 或发起 DMA；启动枚举本身仍遵循既有 PCI 资源流程。

### 被动 PCI 绑定与 aperture 所有权

`kernel/drivers/core/amd_iommu_owner.c` 在 x86_64 + ACPI hosted 配置中注册内建 `amdvi-owner`。它作为隔离基础设施随 driver core 部署，使用普通 PCI `driver_t` 生命周期，未新增旁路设备注册表、Linux 内部类型或用户 ABI。PCI ID 表先匹配 AMD vendor；probe 仅为 IVRS 中指向该 BDF 的候选继续初始化，并用既有只读探测校验 class/header/capability。正常绑定禁止用户态已占有的设备；没有把它标为可绕过所有权的 `read_only_probe`。

每个 PCI function 拥有独立上下文，保留 ACPI mapper/context 的副本和 IVRS 映射；各 capability 的借用视图只在该表存续时使用。probe 成功前不发布 `drv_priv`。首先按固件已知的 16/512-KiB 大小申请 checked claim，再按需映射首个 16 KiB 并只读 control/EFR。旧 10h 描述若通过已开启 MMIO 才确定 512-KiB 大小，则在持有最小窗口占用期间申请完整窗口，成功后才释放旧 claim；decoder 关闭且完整大小未知返回 `-EOPNOTSUPP`，不猜测。完整窗口还要核对同一 function 的其他 capability，不能依赖允许同 owner 重叠的通用 claim。

失败与 remove 均按相反顺序先 unmap、再释放各 claim，最后释放 IVRS 映射和上下文；部分成功时同样回收。claim 保留 device 引用，绑定本身由核心串行化。当前没有硬件写入、DMA、IRQ 或后台生产者，因此无需主动停止阶段；未来增加翻译/队列前必须重新验证硬件状态，并补上停止失败时保留资源的协议，不能沿用当前无条件释放路径。

`AMDVI_OWNER: bound ... writes=0 translation=0` 只证明被动软件绑定成功。已有受管 RAM 与 ACPI 表页保护，但 PFA 以外的 RAM/其他固件预留仍未完整导入；raw MMIO/PIO 不受 claim 拦截，物理热拔插也不被 lifecycle mutex 冻结。该绑定不授予启用翻译、重置控制器或改变固件既有 DMA 路由的权限。

PCI 另提供 `pci_device_find_get(segment, requester, &device)`：在 discovery 事务内查找已发布的 function，并返回需由调用者 `device_put()` 的引用。失败清空输出；非零 segment 返回 `-EOPNOTSUPP`，不存在返回 `-ENODEV`。设备未绑定时仍可查询；旧引用跨设备替换保留原元数据，不被新身份覆盖。引用不固定绑定或物理设备，若 lookup 和后续 config/绑定观察必须协调，调用者须用外层 `driver_discovery_begin/end` 覆盖完整操作。

显式自检须同时指定 `driver.runtime_test=1 driver.amdvi_owner_test=1`。它在启动客户端前检查竞争 owner 被拒绝，注销整个被动驱动后确认占用可重新申请，再注册并仅恢复此前绑定的控制器、确认占用恢复；不会重试其他未绑定设备。测试无硬件写入，成功输出 `AMDVI_OWNER: PASS claim=1 rebind=1 writes=0` 后随运行时自检关机；普通启动不会执行注销/重绑。实例入口为 `tools/a20 test smoke-amdvi-owner-x86_64`。

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
python3 -m unittest tools.tests.test_driver_memory tools.tests.test_driver_resources tools.tests.test_resource_pci_integration tools.tests.test_pci_resources tools.tests.test_amd_iommu_probe tools.tests.test_acpi_ivrs tools.tests.test_amd_iommu_owner tools.tests.test_pci_scope -v
python3 -m unittest tools.tests.test_driver_quiesce tools.tests.test_driver_dma_mapping tools.tests.test_driver_fence tools.tests.test_driver_workqueue tools.tests.test_driver_irq_sync tools.tests.test_driver_firmware tools.tests.test_riscv_iommu_queue tools.tests.test_riscv_iommu_watch -v
make ARCH=x86_64 BOARD=qemu-virt-x86_64 kernel-only -j4
make smoke-driver-lifecycle-x86_64 -j4
tools/a20 test smoke-driver-runtime-x86_64
tools/a20 test smoke-driver-runtime-smp2-x86_64
tools/a20 test smoke-amdvi-owner-x86_64
tools/a20 test smoke-pci-scope-x86_64
tools/a20 test smoke-driver-iommu-native-riscv64
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only -j4
```

主机测试编译生产 C 并使用 ASan/UBSan。quiesce 测试运行真实设备核心和 class registry，覆盖在途调用、发布失败、重新绑定和无 class 设备。DMA 测试替代硬件翻译/页分配器，覆盖非恒等地址、96 页映射、页面存活、掩码、逐页失败回滚、分配失败和架构未就绪。fence 测试以 pthread 替代调度器，注入 prepare/commit 边界，并覆盖 80 等待者与竞争 signal。

QEMU 生命周期门禁还保留一个在途 class 调用，由 quiesce 结束它，验证生产内核能继续完成 unregister。新增运行时门禁通过显式 `driver.runtime_test=1` 在 init_kthread 中测试实际内核 worker 与 Park/Wake，验证延迟/重设、取消、自重排、销毁超时保留对象和重试。测试成功自动关机，普通启动不执行。它们没有执行实体 GPU DMA，也没有证明 AMD/NVIDIA 硬件初始化成功。主机 fence 测试中的调度和 IRQ 是替代后端，不应声称为真实 IRQ/SMP 调度压力验证。

## 下一阶段

1. 固件目录的系统级部署与启动交接、可选线程化 IRQ，以及工作队列资源/故障策略。
2. 在 AMD-Vi 被动绑定与完整 aperture claim 基础上补齐剩余 RAM/固件保留来源，将 PCI 桥路径/Range 证据与 IVRS requester/alias、控制器绑定及 IVMD 约束组合验证，再实现翻译、失效完成、故障处理及失败保留/恢复协议；扩展 IOMMU 地址空间能力与共享 worker 策略。被动绑定和只读快照均不能直接转为硬件启用入口。
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

managed DMA domain 提交：`5b2cc3f31`。10 项驱动主机回归在独立导出的待提交树上通过，包含 pthread map/shutdown 竞争验证、当前失败页的清理句柄保留、无残留映射失败、busy 关闭提交、detach 超时/正数错误规范化及幂等重试。provider 的内部引用在 owner put 后仍保留 context，成功 shutdown 后才允许最终释放。x86_64 与 AArch64 embedded 构建通过。该轮没有增加 QEMU 硬件适配或执行真实 IOMMU detach；这是通用生命周期机制，RISC-V requester 所有权、managed map/unmap/deactivate 适配及端到端硬件验证仍待实现。

原生 provider 提交：`12cb41f67`；启动自检提交：`a7a9ce7ec`。独立导出的 provider 待提交树中 10 项主机回归通过，原生集成测试同时编译实际 `riscv_iommu.c` 与 `dma_mapping.c`，覆盖正 PID/原生 token 互斥、重复 native claim、BME 拒绝、unsupported requester、失败页双层 pin 保留及创建失败清理句柄。RISC-V、x86_64、AArch64 embedded 构建通过，`make check-manifests` 包含新实例并通过。以当前 RISC-V kernel.bin 直接启动 QEMU，附加 `riscv-iommu-pci`、`edu` 和两个自检参数，得到 `DRIVER_IOMMU_NATIVE: PASS map=1 translation=1 readonly=1 unmap=1 detach=1` 及完整运行时 PASS。没有重跑依赖完整用户态构建的 `tools/a20 test` 门禁，不能将直接内核启动记录成完整门禁通过。

EDU 实际 DMA 往返提交：`419e268dc`。独立导出的待提交树中 10 项主机回归通过，RISC-V 内核构建、`make check-manifests` 和文档引用检查通过。直接 QEMU 启动当前内核并附加 IOMMU、EDU 及两个自检参数，获得完整 `DRIVER_IOMMU_NATIVE: PASS map=1 translation=1 readonly=1 unmap=1 detach=1 dma=1 cross_page=1` 和运行时 PASS，随后正常关机。这里验证了原生 PCI → managed domain → 两页 IOVA → EDU 内部缓冲 → RAM 的数据路径，实际 RAM 页不物理连续，原始 owner 引用在传输前已释放。未执行用户态 syscall 路径，未运行完整用户态门禁，也未测试硬件超时、IRQ 完成或故障恢复；这些仍需后续验证。

原生 fault 轮询提交：`724eb73ff`；实际 DMA fault 自检提交：`7f99c8f9e`。独立导出的接口待提交树中 10 项主机回归通过。主机测试覆盖两 native owner 的共享队列归属、同 BDF 不同对象拒绝、BME 清除失败、CQ 超时后无新记录重试、FQ overflow/memory error 对所有 owner 的隔离、记录下界与 claim 拒绝。RISC-V/x86_64 构建、实例清单及文档引用检查通过。直接 QEMU 运行当前 RISC-V 内核得到包含 `fault=1` 的完整原生 IOMMU PASS：EDU 在 unmap 后发起真实 DMA 写、FQ tail 前进，poll 返回 store page fault、blocked/isolated，后续 map 拒绝，保留引用的原 RAM 哨兵不变。硬件故障成功隔离不等于故障恢复；测试未注入真实 BME/CQ/FQ 硬件失效，也未覆盖 IRQ/worker 自动通知、自动 fence 完成或完整用户态门禁。

后台 fault watch 提交：`22462dbd0`；worker → fence 自检提交：`b37442442`。独立导出的监测器待提交树中 11 项主机回归通过，新增测试使用真实生产 workqueue 和 pthread worker，验证健康静默、隔离状态变化通知、旧 token 终止、新 owner 不受旧 watcher 影响、自停止拒绝、停止超时保留及重试、三阶段分配失败回收和 device 引用。后端测试另验证 expected-owner 不匹配时连共享 FQ 错误也不会触碰新 owner。RISC-V/x86_64 构建、清单及文档引用检查通过；直接 QEMU 运行当前内核得到包含 `fault=1 watch=1` 的完整原生 IOMMU PASS，真实 EDU fault 由 worker 隔离，回调 signal 测试 fence，主线程 stop 排空后再检查/释放上下文。该测试 fence 表示通知完成，不代表 GPU 请求成功；没有验证真实 IRQ、共享 worker 池、完整用户态门禁或硬件恢复。

IVRS parser 提交：`a18f21214`；x86 发现与门禁提交：`49baceb1c`。独立导出的 parser 待提交树中 13 项主机回归通过，覆盖新的非对齐/畸形 IVRS fixture 和既有驱动基础设施。x86_64 构建、清单和引用检查通过；正式 `tools/a20 test smoke-amdvi-discovery-x86_64` 通过，发现同一 requester/base 的 10h、11h 两条记录，日志明确 `driver_enabled=0`。门禁初始预期误写为一条记录，已按 QEMU 实际的双格式描述修正；解析器保留原始记录，没有重复启动硬件单元。该验证没有启用 AMD-Vi，没有执行 AMD DMA 隔离或裸机 GPU 测试。

IVRS 路由候选提交：`3d23c9e86`。独立导出的待提交树中 14 项驱动主机回归通过；新增生产 policy 测试启用 ASan/UBSan，覆盖三种 IVHD 的全部排列、未选中格式重复、范围边界、alias 缺失/链/循环/跨单元、原始与 alias requester 的 IVMD、其他单元约束排除、MMIO 完全/部分重叠与相邻窗口、未知策略和数量上限。x86_64 构建及引用检查通过。正式 AMD-Vi 发现门禁通过；首次运行因可用磁盘略低于默认 8192 MiB 预检门槛而等待，终止后使用 `A20_MIN_DISK_MB=7168 A20_WAIT_TIMEOUT=120 tools/a20 test smoke-amdvi-discovery-x86_64` 完成。QEMU 门禁验证当前内核的发现和运行时回归，启动报告没有调用新增路由查询；候选策略由主机 fixture 验证。本轮没有执行 AMD-Vi 硬件启用、IVMD 权限落实或 GPU 加速。

IVRS 单元枚举提交：`a96fffc29`；PCI/MMIO 只读探测提交：`6ad339603`。独立导出的待提交树中 15 项驱动主机回归通过；新增测试启用 ASan/UBSan，覆盖精确 capability 偏移、完整链尾/循环、payload 边界、QEMU revision-0 身份限制、decoder 关闭时零 MMIO 访问、启用时 control/EFR 读取、地址宽度、aperture 对齐与 transport 失败清空输出。x86_64 bringup/dev 与 AArch64 embedded 内核构建通过，110 个实例清单及文档引用检查通过。

本轮正式 `tools/a20 test smoke-amdvi-discovery-x86_64` 在 `ufsd-x86_64` 链接处因既有日志改动的 `klog_write_ex` 未定义而失败，不能记为完整门禁通过。最新 dev 内核改用 `timeout 60s qemu-system-x86_64 -machine q35 -m 1G -smp 1 -nic none -device amd-iommu -kernel .kernel-build/x86_64-qemu-virt-x86_64-linux-6d99d92e3642d9cf/kernel.bin -nographic -monitor none -append 'driver.runtime_test=1'` 直接启动，正常关机，匹配实例全部五条预期标记。日志为两条 IVHD、一个去重 11h 单元、`verified=1 failed=0 driver_enabled=0`，并有完整的 `DRIVER_RUNTIME: PASS ... firmware=1`。该 QEMU 10.0.13 单元实际报告 `revision_quirk=1 pa_bits=40 va_bits=48 mmio_enabled=0 registers_read=0`；这验证了实际 PCI capability 读取和关闭 decoder 的路径，没有读取实际 MMIO。已启用 MMIO、counter aperture 及失败注入分支由主机 fixture 验证；没有启用 AMD-Vi、执行 AMD DMA 隔离或 GPU 加速。

资源占用提交：`b643e0f0b`；内核自检提交：`8ec0e2843`；SMP 结果输出提交：`6c0a7588d`。独立导出的待提交树中 46 项主机回归通过，包含新的 pthread 竞争申请、阻塞 unmap、owner 最终引用、三阶段分配失败与 mapper 失败回滚，以及既有固件、class、PCI/USB、simplefb、板级音频、DMA/IOMMU 和异步运行时回归。资源测试使用生产 driver_core/class 代码，ASan/UBSan 开启；硬件和任务锁由主机后端替代。x86_64 dev 单核/双核与 AArch64 embedded 内核构建通过，实例清单及引用检查通过。

本轮直接用 q35、amd-iommu、`driver.runtime_test=1` 启动单核与双核内核，两次正常关机并匹配对应 runtime 实例全部标记。双核日志确认 `2/2 configured CPUs online`；最初早期资源 PASS 行因当前 best-effort 控制台竞争未显示，已将两项 PASS 合并到测试末尾同一条日志，复验两项均完整出现。资源自检仅操作元数据，未对测试区间执行 MMIO 或 port I/O；并发申请和 unmap 窗口由主机线程验证，不据此声明完成真实硬件 SMP 压力测试。没有重跑依赖完整用户态构建的 `tools/a20 test`，也没有启用 AMD-Vi 或 GPU 加速；已有的用户态日志符号链接问题未在本轮处理。

资源清单核对提交：`c3f2f9dfa`；PCI BAR 规划提交：`fd8ce372f`；内核清单自检提交：`ea32e702f`。代码阶段独立导出的待提交树中 75 项主机回归通过，包含 checked 申请与设备发布的线程竞争、未绑定声明和 `RES_MEM` 冲突、owner 注销后继续保留占用，以及生产 driver core/PCI 代码的三项集成测试：分配跳过已申请 aperture、冲突发布拒绝后可恢复、后序 BAR 冲突时前序 BAR/command/资源表不变。PCI 模型测试覆盖 host window、64-bit BAR、桥、port I/O、W1C status 和 VirtIO capability；配置硬件和任务锁仍由主机后端替代。

代码阶段的 x86_64 dev 单核/双核与 AArch64 embedded 内核构建通过，110 个实例清单检查通过。直接 QEMU q35 + amd-iommu + `driver.runtime_test=1` 的单核/双核启动均输出 `DRIVER_RESOURCES: PASS ... inventory=1` 和完整运行时 PASS；双核日志确认 `2/2 configured CPUs online`。这些结果验证资源元数据和运行时集成，不代表真实硬件并发压力测试、AMD-Vi 接管或 GPU 加速。完整用户态 `tools/a20 test` 门禁未在本轮重跑；本次文档收尾核对上述保存日志并执行文档引用检查，没有新增功能或重跑硬件测试。

2026-11-13 AMD-Vi 被动所有权提交：`160066987`；自检恢复范围修正：`73861c13b`。隔离待提交树中的 55 项相关主机回归通过；新增 11 项测试使用生产 PCI/core/ACPI/IVRS/探测代码与 ASan/UBSan，覆盖 BDF 引用跨替换保留、未绑定查询、decoder 关闭时零 MMIO、开启时 map/unmap 的 claim 存续、完整 512-KiB 窗口、旧格式扩展及冲突回滚、同 owner 多 capability 的双向重叠拒绝、三阶段分配失败、mapper 失败及 user-owned 拒绝；自检还断言无关失败设备不被再次 probe，恢复旧版全局 `probe_all` 的负控会触发该断言。硬件、任务锁和 class publication 由 fixture 替代；这不是实板或硬件并发压力测试。

`make ARCH=x86_64 ABI=linux BRINGUP=0 DRIVER_DEPLOYMENT=embedded NR_CPUS=2 kernel-only -j4` 与 AArch64 QEMU virt 的 `ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded` 内核构建通过，111 个实例清单检查通过。x86 双核内核直接以 QEMU q35、`-smp 2 -nic none -device amd-iommu` 和上述双自检参数启动，退出码 0，匹配 owner 实例全部预期标记；日志确认 `2/2 configured CPUs online`、首次绑定与重新绑定，以及 owner/resource/runtime 三项 PASS。QEMU 的 MMIO decoder 关闭，decoder 已启用的 MMIO 分支仅由主机 fixture 验证，没有启用 AMD-Vi 翻译或 GPU 加速。

本次 generic `kernel-only` 在工作区现有的 virtio-scsi 模块编译处因 `CONFIG_NR_CPUS` 未定义失败；未修订无关模块代码，未把 embedded 构建及直接 QEMU 验证记作完整 generic/用户态 `tools/a20 test` 门禁通过。验证日志保存在本地 `.kernel-build/amdvi-owner-review/`，可清理的构建日志不作为长期接口契约。

2026-11-13 物理存储保留提交：`9e1da2462`。隔离待提交树上 97 项主机回归通过；新增 8 项存储测试运行生产 driver core、class 与 ACPI 表读取代码，以 ASan/UBSan 和 pthread 验证副本/归并/边界、重叠保留独立释放、失败批次无部分保护、mapper 拒绝、保留与基本 claim/设备发布的竞争、受管 RAM 与 ACPI 页导入，以及失败后的重试。另扩展生产 PCI/core 集成与 AMD-Vi 用例：PCI 跳过存储、变动 BAR 在 reachability 前拒绝、只读 AMD-Vi 诊断和被动绑定在 map 前拒绝。移除 PCI/诊断前置检查的两个负控均触发新断言；硬件与任务同步仍是主机替代后端。

内核构建与 QEMU 使用保留既有改动的当前工作区，主机回归另以隔离待提交树复验。x86_64 `ABI=linux BRINGUP=0 DRIVER_DEPLOYMENT=embedded NR_CPUS=2` 与 AArch64 QEMU virt `ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded` 内核构建通过，111 个实例清单检查通过。直接 QEMU q35、双 CPU、`-nic none -device amd-iommu`、`driver.runtime_test=1 driver.amdvi_owner_test=1` 启动正常关机，匹配更新后 owner 实例全部标记；日志为 `ram_ranges=1 firmware_ranges=10`、`2/2 configured CPUs online`、owner 重绑 PASS、资源 `inventory=1 memory=1` 和完整运行时 PASS。固件记录数是本次 QEMU 的观察值，不作为其他机器的固定预期。

本轮未启用 AMD-Vi 翻译或 GPU DMA，也未重跑完整 generic/用户态门禁；上一阶段记录的模块构建问题未纳入这次修改。新增保护不拦截 raw MMIO/PIO，不替代 IOMMU、PFA 页所有权或实际映射缓存属性检查，未验证实体硬件热插拔和 DMA 故障恢复。

2026-11-14 PCI 作用域提交：`ce5912bcc`；核心注册状态修正：`388197124`。在更新后的共享基线与隔离待提交树中，106 项主机回归通过，新增 8 项用例以生产 PCI/core 和 AMD scope 代码配合 ASan/UBSan 覆盖：多级/直接路径、function 位边界、输出引用、身份替换、直接核心注销与 rescan 后的新身份、重叠 sibling/重复 secondary、缺失桥、非法 primary/window/class、循环、PCIe capability 损坏链尾/重复/越界/payload 交叠/错误类型或版本，以及无效 Range 拒绝。去掉重叠检查、忽略 root-port 校验的两个负控均触发断言；查询前后配置写入计数不变。硬件与同步由主机后端替代。

保留既有改动的工作区中，x86_64 dev 双核 embedded、x86_64 generic 以及 AArch64 bringup embedded 内核构建通过，112 个实例清单检查通过。分别按新增实例的 q35/root-port/EDU/amd-iommu 参数直接启动 x86 双核 embedded/generic 内核，退出码 0，匹配全部门禁标记；日志确认 EDU 位于 `01:00.0`、`2/2 configured CPUs online`、owner 重绑以及 `memory=1 pci=1`。QEMU 的 AMD Range 没有 RngValid，验证的是拒绝使用无效字段；AMD 有效 Range 与 root-port capability 的组合由主机模型验证，不能写成真实 AMD DMA 路由已验证。没有给 EDU 提交 DMA，也没有启用 AMD-Vi 翻译或 GPU 加速。

`tools/a20 test smoke-pci-scope-x86_64` 已尝试，但完整门禁在 Native `ufsd` 构建阶段被 fscompat 时钟适配缺失（`TICKS_PER_SEC` 展开依赖未定义的 `ARCH_TIMER_FREQ`）阻塞；局部时钟适配诊断还暴露 `fifo_open_vnode`/`klog_write_ex` 缺失，诊断改动已撤回。上述直接启动结果不记为完整 CLI/用户态门禁通过。本地验证日志位于 `.kernel-build/pci-scope-review/`；硬件热插拔、AMD 实机翻译与 DMA 故障恢复仍待验证。

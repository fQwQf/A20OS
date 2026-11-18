# 原生驱动运行时

本文阐述 A20OS 原生驱动运行时的核心机制与设计规范，涵盖原生请求队列、可失败执行停止（execution stop）、设备静默（quiesce）、受管资源绑定、物理存储保留、页级 DMA 映射、原生异步同步原语（fence）、工作队列与固件加载等关键基础设施。这些机制面向标准通用内核，同时核心设备模型具备良好的跨架构与微控制器可伸缩性。

## 服务发现与调用边界

通用架构以每绑定 endpoint 的协议集合连接资源、执行和各子系统；详见[完整架构与开发流程](core-model.md#架构与开发入口)。既有 class publication 与原生 endpoint 共用身份、active call 和退出路径，没有第二个全局注册表。发布时复制并排序 ID/version/ops_size/capabilities，调用在本地二分查询后准入，元数据与代码访问分离。组合协议本身不更改 device_t/driver_t 或既有 class vtable，当前模块 ABI 见下节。无传统 class 的服务和 MCU driver-core 配置同样使用这一模型。

protocol access 借用 caller 的 endpoint 引用，caller 必须持有到 end；准入/结束不分配、不执行最后 put，可在 IRQ 中使用，方法另行声明上下文。它保护当次绑定的方法与实例，不保留返回 backing、不停止 DMA、不授予用户权限。队列/请求、存储和完成仍按本页的各自所有权退出。

## 分层与边界

| 层次 | 职责 |
|---|---|
| 设备核心 | device/driver/bus/class、绑定、引用、依赖、资源回收、下线顺序 |
| 通用运行时 | 有界 request/queue、DMA mapping/domain、IRQ 排空、共享工作队列、原生 fence、固件读取 |
| GPU 子系统 | 已有带引用的 DISPLAY/DRM 与 native queue 接入；厂商 BO/GPUVM、硬件上下文、调度/恢复和同步 UAPI 按后端完善 |
| 硬件驱动 | IP/固件初始化、命令协议、硬件完成中断、停止与复位 |
| 可选 Linux 源码兼容层 | 将上游源码接口映射到原生运行时，不成为原生驱动的依赖 |

内置驱动与 `.a20drv` 使用同一 `driver_t`。ABI 9 增加了 `driver_t.quiesce`；当前 ABI 为 **13**，在 URB 末尾增加同步 bulk 的 `actual_length`；ABI 12 在 GPU vtable 末尾增加显式原生 `get_capabilities`；ABI 11 已增加 owning RAM `get_scanout`；ABI 10 已扩展 `usb_hcd_ops_t.release_endpoint`，所有旧模块必须重建。后文带日期的验证保留当时 ABI，不表示当前接受旧模块。新增 DMA/fence/workqueue/firmware 函数已加入模块导出表。此处没有承诺跨内核版本稳定的二进制 ABI。

## 下线与资源生命周期

成功绑定的设备按以下顺序解除绑定：

1. 若 probe 已显式注册 remove prepare，先 offline provider class 并执行准备动作；失败即返回，尚不解绑 consumer。
2. 准备成功（或未选择准备）后，先解除 consumer/child，再处理 provider/parent。
3. 对登记了 execution action 的设备先 offline，在 active-call drain 前执行所有未成功的 fallible stop；成功 checkpoint，失败返回并保留 PREPARING/PROBE_CLEANUP、代码、实例、IRQ 和存储。
4. 所有 stop 成功后状态变为 `REMOVING`，执行一次可选 `driver_t.quiesce(dev)`；旧的同步驱动可以不登记 execution action。
5. `class_device_unpublish()` 撤销 registry publication，等待在途调用结束。
6. 调用 `remove()`；只有返回 0，核心才逆序释放受管资源及依赖并清空绑定。

新异步驱动使用[原生请求与队列](request-queues.md)：每请求保留 native owner，IRQ complete 只记录硬件完成，任务 retire 成功后才发布 fence；队列自动登记 execution action。硬件 stop 失败直接返回，不会卡在尚未解决请求的调用排空中。各 stop 都会尝试，成功项在重试时跳过。provider context 仍须保留到调用排空和 remove 成功。

已有 void quiesce 仅用于兼容退出。它必须关闭软件生产者，但不能把未确认的硬件停止伪装成请求存储可回收。新驱动不把可失败的停止工作推迟到 call drain 之后；remove 也不能成为唯一的请求唤醒入口。驱动私有入口必须检查 stopping，class offline 不保护绕过 adapter 的调用。

probe 成功后 publication 失败同样走 execution stop → quiesce → drain → remove。`driver_queue_create` 与 `device_add_execution_action` 自动选择受管 failed-probe 清理；`device_probe_use_remove` 可用于其他自有执行模型。未选择者仍自行安全回滚硬件，核心不从普通错误码推断硬件已停。

### 可失败的解绑与显式重试

`driver_t.remove()` 已有 int 返回值，现在由核心执行：非零结果阻止资源回收，负 errno 原样返回，正数按 `-EIO` 处理。驱动必须在失败时保留有效的 `drv_priv`、硬件可达存储和恢复所需寄存器/代码；不能先释放后报错。`device_unregister()`、`platform_device_unregister()` 和 `device_hotplug(REMOVE)` 返回错误，保留设备注册表引用、绑定、devres、匹配身份及尚未释放的依赖。自己的 remove 已执行失败时，设备保持 `REMOVING`，class 已 offline/撤销 publication；不会重新开放旧句柄。调用者保留对象并在条件修复后重试同一接口，核心只重试 remove，不重复 quiesce/offline observer。正常 unregister 完成前 `device_is_registered()` 仍可为真，不能把成员关系当作在线许可。

未选择 remove prepare 时先处理 consumer/child，它们停止失败时 provider/parent 尚不 quiesce 或释放。选择 prepare 的 provider 可以先冻结硬件，但必须保留 consumer 清理仍需的接口和存储；不能把这个显式协议推广到所有 provider。部分 consumer 可能已解除绑定并进入 deferred；失败不是事务回滚，不自动重建原图。移除意图与递归栈由 lifecycle mutex 下的私有栈帧保护：正在退出的驱动不能接收新匹配绑定，递归退出同一对象返回 `-EBUSY`；正在移除的 provider 拒绝新 dependency（`-EPROBE_DEFER`），其 parent 链拒绝新 child 发布（`-EBUSY`），即使它仍处于等待 consumer 停止的阶段。未新增公开 group/上下文标识或布局字段。

`driver_unregister()` 只有在剩余绑定全部成功解绑后才移除驱动登记；失败保留登记与代码所有权，调用者不能释放模块/driver 对象。已完成解绑的设备保持已完成状态，调用者显式重试处理余下绑定。成功 probe 后 class 发布失败且 remove 也失败时，同样保留绑定/资源；`probe_error` 保存原 publication 错误，不允许下一候选驱动覆盖它。未选择受管失败清理的旧 probe 仍须自行安全回滚；核心无法从普通错误码推断未停止的硬件。

REMOVE 仅在成功后发出 bus notification，核心为通知另持一份 device 引用，防止 registry 最后一次 put 提前销毁元数据。PCI rescan 停止失败时保留旧 BDF 记录，不发布替代身份，也不将缺失 function 的槽标为空闲。已发布 USB 端口先 offline interfaces，再检查 `abort_slot` 的返回；缺少停止回调返回 `-EOPNOTSUPP`，失败保留 slot/address、interface/endpoint 和 HCD 登记。slot 停止成功但后续 remove 失败时，重试不重复 Disable Slot；port 的 pending 状态会在重新枚举前重试，即使物理连接已经恢复。`usb_core_unregister_hcd()` 返回错误，xHCI/hub remove 传播它并保留实例。USB 枚举的部分 slot 和接口图现在也交给同一端口清理路径，见下文；xHCI 的 start/probe 已接入下述退出准备，其余 HCD 和错误阶段仍须各自确认。

公开 `device_t`/`driver_t` 布局未因这项返回值变更而改变；新增模块导出 `device_unregister`，源码调用方须检查上述 int 返回值。关机/kexec 现在复用下节的可失败解绑管线；失败 probe 的硬件回滚、实际 stop/失效完成仍需要各 provider 的明确协议，核心不把超时转换成可释放证明。

当前生命周期回调仍持有全局 lifecycle mutex，禁止等待需要该锁的工作或递归注销自己。quiesce 没有通用超时后强制释放内存的语义：如果不能证实 DMA 已停止，驱动不能继续释放硬件仍可能访问的存储。按设备并发的生命周期协调与通用恢复策略仍需后续开发。

### 关机与 kexec 的终止管线

`driver_core_shutdown_all()` 返回 int，在其他 CPU 仍可处理完成事件时运行现有可失败解绑管线。它预先拒绝缺少 remove 的绑定，随后关闭新 driver/device/bus 注册、probe 与 dependency；这些入口返回 `-ESHUTDOWN`。停止失败保留原绑定、匹配、DMA 存储和依赖，调用方可重试清理，已完成部分不回滚。回调会退役子设备声明，因此核心每次处理后重新扫描注册表，不跨回调保留数组下标。

Linux/Native reboot/poweroff 必须检查停止结果，错误时不进入 firmware。kexec 先确认架构后端和设备协议存在，再关闭文件系统、停止设备，最后进行架构准备/其他 CPU 停机及内核交接；架构准备失败取消其资源，但不重新打开已关闭的文件系统或设备。这是终止过程，不提供恢复正常运行的事务回滚。

旧 `driver_t.shutdown` void 字段保留布局兼容，终止路径不再调用它；驱动必须在已有 prepare/stop/remove 协议内确认硬件停止并报告错误。公开 device/driver 布局和该变更未修改当时模块 ABI 12。此核心行为不自动证明旧驱动的 remove 已正确停止 DMA，也不替代其在 class drain 前取消/唤醒活跃请求的协议。

2026-11-16 的代码提交为 `3e02ee9aa`、`46aa30689`。仅对生产 C/Rust core/class 的终止失败/重试及生产 kexec/Linux/Native 入口控制流作针对性检查，均通过；记录见 `build/evidence/v1-closeout-20261116/driver-terminal-stop/report.json`。下方历史记录中关于尚未接入关机/kexec 的描述以本节当前实现为准；实体硬件停止仍按各 provider 的证据认定。

### VirtIO 块设备与网卡的停止确认

VirtIO blk/net 的 remove 现用已有 `virtio_transport_reset(..., 100)` 发起复位并等待 STATUS 读回零，再退休 IRQ/MSI-X 和驱动状态。网卡 ring 也只在确认成功后释放。ETIMEDOUT 保留原 `drv_priv`、IRQ 和全部硬件可达队列，调用方按核心失败保留协议重试同一绑定；不能把一次 STATUS 写操作当成停止证明。离线网卡 IRQ 继续确认 ISR，但不进入网络栈。

2026-11-16 提交 `d528c8958`，针对性检查 `tools.tests.test_virtio_stop_ack` 编译生产 reset/remove/IRQ 函数，以寄存器/时钟模型检查超时、延迟确认、迟到中断及 INTx/MSI-X 的退休顺序，结果在 `build/evidence/v1-closeout-20261116/virtio-stop-ack/report.json`。公开布局/ABI 12 不变。这一结果不扩展到实体设备、class drain 前的活跃请求停止或私有入口的全部寿命协议。网卡与块设备受管 failed-probe 见下节；块设备整次 I/O 引用、活跃停止和 DMA 退休见下节；旧元数据消费引用/槽位复用已按下节接入。

### VirtIO 网卡的失败 probe 与槽位保留

网卡在 transport 初始化前显式选择核心 failed-probe 清理，并以 device owner 保留私有槽位。已验证 transport 设置 `transport_live` 后，初始 reset 和失败清理都要求有界零状态确认；未验证 transport 的清理不写复位寄存器。任一队列设置失败后，已分配 ring 留给 remove，不能在 init 错误出口直接释放。remove 失败不交还槽位，成功清理后才允许新绑定复用。

私有按编号的 ready 查询与 send/recv 准入使用专用槽位锁；probe 的硬件/IRQ 初始化完成后才发布可用状态。send/recv 持有整次调用引用，stop 关闭新准入，引用归零前保留 ring 和完成路径，之后确认复位才退休。已提交 TX 的超时不归还 DMA 所有权。数字编号仍不是可跨解绑保留的设备身份。

2026-11-16 提交 `49e3b2772`，`test_virtio_net_probe_cleanup` 与既有 `test_virtio_stop_ack` 直接相关检查通过，结果在 `build/evidence/v1-closeout-20261116/virtio-net-probe/report.json`。使用生产私有布局/probe/init/remove/reset/IRQ 控制流及替代核心 adapter/transport/队列分配边界；未扩展为真实网卡、完整核心 probe 集成或任意 SMP 压力认证。模块 ABI 12 和同时支持的设备容量不变。

### VirtIO 块设备失败 probe 与旧缓存借用

块设备 probe 在 transport 前选择受管失败清理并登记实例 owner；队列初始化报错后不直接 free DMA。验证过的 transport 由 remove 确认 reset 后才清理，超时保留原 owner/IRQ/DMA；未验证 transport 不写 reset。未发布失败实例成功清理后可复用槽位，ready、批写和编号 I/O 入口检查发布状态，不将这些实例提供给调用者。

成功发布过的嵌入 `block_dev_t` 可能仍由旧缓存借用。这类元数据保留原身份，槽位不用于另一磁盘；不能以“已经 reset”推导所有软件借用已消失。DMA 的停止后释放按下节执行；元数据消费引用与槽位复用现按下节执行，没有新增公开结构字段，也未改模块 ABI 12。

2026-11-16 提交 `ff4ab1474`。生产布局/probe/init/remove/free-DMA/reset/ready 配替代设备初始化、allocator、transport 和核心 adapter 的失败场景，以及受影响 stop ACK/批写三项检查通过，记录在 `build/evidence/v1-closeout-20261116/virtio-blk-probe/report.json`。它证明所列错误出口的存储保留与槽位选择，不代表完整核心集成、实体硬件、活跃 DMA 请求取消或缓存引用回收认证。

### VirtIO 块设备的整次调用引用与提前 stop

块设备 probe 先登记 `device_add_execution_action`，其 stop 在 class drain 前执行，失败 probe 登记 OOM 仍走已选择的可失败 remove。read/write 的多段循环、flush 和批写通过实例锁持有整次 active call，最后一次返回前释放；生命周期回调不等候持有该锁的工作。

stop 关闭新准入。active call 尚在时返回 EBUSY，保留硬件、IRQ/poll 与 DMA，使已接受的请求可以完成并退出；不会在等待过程中提前把 valid 清零。新的 request 分配与下一批提交被拒绝，已提交的批写前缀继续 kick/等待后返回错误。引用归零后，显式重试 stop 才进行有界 reset 和 IRQ 退休，成功动作由既有核心 checkpoint 保留；remove 在 class drain 完成后释放 DMA。

已交出的 block_dev 元数据仍是同一个关闭身份，其 I/O 入口在访问 DMA 前拒绝调用，不借槽位复用指向另一磁盘。未发布槽位重建保留原子发布守卫，ready/poll/I/O 先检查该守卫，再访问实例锁/内容。旧元数据的最后一个消费引用与槽位再利用按下节的 release 协议执行。

2026-11-16 提交 `ef546cce9`。现有 stop ACK、失败 probe、批写三项相关主机检查通过；生产所列控制函数配替代 MMIO/request/核心 adapter 边界，检查忙时不复位、read/write/flush 全调用引用、批写前缀收回以及停止后 DMA 释放，记录在 `build/evidence/v1-closeout-20261116/virtio-blk-active-stop/report.json`。没有完整回归、guest、实体硬件或任意 SMP 认证，也不扩大为其他 provider 的停止和超时恢复证明。

### USB storage 的停止与引用

storage 在端点配置前选择受管失败清理并登记 execution stop。部分配置或停止失败仍保留同一个 owner/接口，禁止另一 probe 覆盖；stop 关闭准入，有整次读写调用时返回 EBUSY，不持生命周期锁等候 I/O。调用结束后重试交还接口端点，确认成功后才在 class drain 后退休绑定；重绑先 SET_INTERFACE。

`usb_storage_get_dev()` 成功取得一个消费引用；缓存和 I/O 全部结束后通过 `block->release(block)` 归还，bcache 仍借用设备。旧引用保留关闭身份，最后 release 后才允许槽位复用；ready 查询不取得引用，编号不提供跨解绑身份。公开结构及 ABI 12 不变。提交 `46cab0d4a` 的生产 core/USB/storage 定向检查通过。后续 `b0f5654db` 将 reset recovery 放在 BOT mutex 内：先交还 host 端点，依序 reset/clear HALT IN/clear HALT OUT，再重建端点；失败保留 pending，新 CBW 必须先恢复。运行期生命周期忙则延后，probe 可复用已有生命周期所有权；原命令错误不转成成功或自动重放。`db916acc2` 又接入实际长度、数据短包/STALL 后读取 CSW，以及首次 CSW STALL 清除后单次重试；短数据不能冒充完整块读写成功。URB 因新增实际长度字段升级到 ABI 13，软件阶段处理的定向检查通过。

### VirtIO 块设备消费引用与旧身份

`virtio_blk_get_dev(index)` 成功时交给调用方一个拥有的消费引用。调用方必须跨所有缓存和 I/O 使用保留它，销毁相关缓存、结束所有 I/O 后调用 `dev->release(dev)`，每个成功 getter 配一次 release。bcache_create/destroy 仍借用设备，不自动转移或归还该引用。最后一次 release 后不再允许使用原指针；接口没有提供跨最后释放的永久编号身份。

解绑关闭旧实例并释放已停止的 DMA，消费引用保留旧 block_dev 元数据，旧 read/flush 等调用返回 ENODEV。绑定退休且消费引用归零后才交还槽位，新绑定可以复用其容量。按编号的 I/O、capacity/ready 和 poll_all 在访问期间也保留临时引用，槽位选择/引用由专用 spinlock 协调，该锁不与实例锁嵌套；poll 在实例锁内再次检查 valid。callback 表在 probe 发布前安装一次。

保持原 VIRTIO_MAX_DEVS 容量；在线或仍被消费引用保留的实例都占用容量，普通已结束绑定可在最后 release 后反复复用，不再按历史重绑次数耗尽。公开结构及 ABI 12 不变。树内没有该旧 getter 的实际调用方，本次没有改变其他 block_dev provider 或 bcache 的拥有约定。

2026-11-16 提交 `70a2e27ea`，两项直接相关检查通过，记录在 `build/evidence/v1-closeout-20261116/virtio-blk-consumer/report.json`。生产所列引用/slot/getter/release/probe/停止/I/O wrapper 配替代硬件边界，检查多引用跨解绑、旧 I/O 拒绝、最后 release 后复用和超过容量的成功重绑；DMA 分配/释放相等。没有完整回归、guest、实体设备或任意 SMP 认证。此前关于旧元数据永久保留/无法复用的当前说明由本节替代。

### 原生 publication 的绑定停止凭据

持有真实核心 publication 引用的消费者可调用 `class_device_binding_stopped(cdev)`。它记录这个 publication 所属旧绑定的显式成功 remove 契约：在线、仅 offline、调用已排空或 remove 返回错误时均为 false；没有 remove 的绑定也永远为 false。只有核心接受 remove 成功并开始 devres 回收，才发布 true。设备重新绑定后旧凭据保持 true，新 publication 从 false 开始。NULL 返回 false，不能传入栈上仿造的 class 对象。

该状态存放在核心 allocation 的私有尾部，没有新增公开 class/device/driver 字段，模块 ABI 沿用当前版本 12。绑定持有一个 devres action 引用；unpublish 不会提前销毁凭据。发布过程发生分配、编号或名称冲突错误时，精确撤销该 action 并归还引用，失败不留下额外设备 pin 或 devres 项。消费者自行保留 publication 引用，查询不会固定或重新进入驱动代码。

true 表示驱动已经履行 remove 停止契约，不是对硬件寄存器的探测，也不证明全部 devres 已执行完。它不替代 DMA 失效、CPU/TLB 退休或存储引用。禁止从 `dev->drv`、默认设备变化、class 离线或设备注册状态推导相同结论；旧对象的清理身份不能用于重绑后的新设备。驱动必须在 remove 返回 0 前确认不再访问由消费者准备回收的存储。

`device_release_resources()` 是核心完整绑定退出入口，驱动不得在仍在线或停止失败的绑定上自行排空它。局部步骤回滚使用 `device_release_resource_action(dev, release, data)`：在串行 probe/lifecycle 上下文中，分离最新的 callback/data 精确匹配项并调用一次，剩余动作顺序保持；不分配内存，不隐式停止硬件。无匹配返回 `-ENOENT`，无效 device/callback 返回 `-EINVAL`。它不适用于 class/IRQ 并发修改 devres。该新接口与停止查询均已导出，不改变现有结构布局。

### 显式选择失败 probe 的清理所有权

新驱动可在自己的 `probe(dev)` 中、任何硬件初始化之前调用 `device_probe_use_remove(dev)`，检查返回 0 后再启动初始化。它不分配内存、可重复调用，仅为本次 probe 选择失败时的清理路径；成功 probe 保持原有发布流程。当前 probe 之外或嵌套 probe 操作祖先返回 `-EINVAL`，未注册设备返回 `-ENODEV`，缺少 remove 返回 `-EOPNOTSUPP`；失败不设置清理意图。不要忽略错误后继续激活硬件。

```c
static int gpu_probe(device_t *dev) {
    int ret = device_probe_use_remove(dev);
    if (ret) return ret;
    /* Install partial-init-safe private state before hardware activation. */
    return gpu_initialize(dev);
}
```

选定后，probe 非零返回会交给 consumer-first、已登记 execution stop、quiesce、class drain、可失败 remove 管线。回调须能处理部分初始化乃至 NULL `drv_priv`；probe 不再自行释放交给 remove 的存储。remove 返回 0 才允许 devres/依赖回收及清空绑定。若 provider 构造失败同时返回非 NULL DMA 清理句柄，应保存在实例中；remove 重试 shutdown，成功后才 put。不能因为初始化返回错误就丢弃清理句柄、页表或 requester 租约。

核心保存原 probe 错误于 `probe_error`（正数规范化为 `-EIO`）。清理成功时 probe 调用路径仍返回原错误，设备转为 UNINIT 或原错误为 `-EPROBE_DEFER` 时的 DEFERRED；清理失败则返回清理错误，保留原 `probe_error`、绑定、匹配身份、私有状态、devres 和依赖。device_register/driver_register 接受登记不表示 probe 成功，仍须观察绑定结果。保留的失败绑定不进入下一候选或自动 deferred reprobe，只能显式注销重试清理；这不是硬件恢复/重新初始化接口。

子设备/consumer 停止失败可能阻止父设备执行 quiesce。此时失败 probe 保持 `DEV_STATE_PROBE_CLEANUP`，其 class 先 offline，恢复后重试仍须执行首次 quiesce；进入自己的 remove 后才变为 REMOVING，后续仅重试 remove。`device_is_stopping()` 同时观察两种状态；checked resource、PCI 查找/范围观察、managed DMA 创建/新映射及 class 发布均拒绝它们。旧引用和旧资源保留供停止，不授予新使用权限。该 helper 只观察元数据，调用者仍须持有引用并按需使用 discovery 事务协调绑定/配置。

probe 意图保存在 lifecycle mutex 下的私有调用栈，不增加公开布局字段或 ABI 版本（当前为 12）。嵌套 probe 各自选择、返回后恢复上一帧；构造尚未返回时对自己或祖先递归注销返回 `-EBUSY`。新 API 已导出给模块。旧驱动不会自动接入；迁移前必须逐一审查部分初始化的 quiesce/remove、错误传播和硬件停止证明。

### 在 consumer 解绑前准备退出

控制器的 IRQ/ring 可能继续访问 child/consumer 存储，等到 provider remove 才停止会太晚。`device_add_remove_prepare(dev, callback, context)` 让驱动显式选择提前准备：只能从该设备当前 probe、且 `device_probe_use_remove()` 已成功后登记。记录在 probe 中分配，失败返回 `-ENOMEM`，不安装动作；错误参数或阶段返回 `-EINVAL`，未注册的合法当前实例返回 `-ENODEV`。公开 device/driver 布局不增加字段，模块导出此函数，该接口不改 device/driver 布局；当前全局模块 ABI 为 13。

```c
int ret = device_probe_use_remove(dev);
if (!ret) ret = device_add_remove_prepare(dev, controller_prepare, dev);
if (ret) return ret;
/* Initialize drv_priv, then activate hardware. Prepare handles NULL too. */
```

准备动作按登记的逆序执行，先于任何 consumer/child unbind；只返回 0 才记为完成。正数错误按 `-EIO` 处理，失败保留绑定、匹配身份、依赖、devres、上下文和未动的消费图；之前成功动作的硬件副作用不回滚。显式注销重试从未完成的动作继续，已成功的动作不会因之后的 consumer 或 remove 失败而重复。普通退出停在 `DEV_STATE_PREPARING`；失败 probe 保持 `PROBE_CLEANUP` 及原 `probe_error`。`device_is_stopping()` 包含新状态，阻止新 class 发布、checked claim、PCI lookup/scope 与 DMA 创建/映射，已有映射仍可关闭回收。准备完成后进入 consumer-first、execution stop、quiesce、class drain、remove、devres；未登记 execution action 的旧驱动保持原有顺序。

prepare 必须容忍部分初始化，冻结新提交并排空可能访问 consumer 存储的回调/硬件；可以撤销 IRQ，但不能释放 consumer/remove 仍需的存储。callback/context 有效至最终回收，context 的销毁另行安排；完成的准备是单向退出，不能恢复硬件后期待核心重复它。回调在 lifecycle mutex 下、不持 registry spinlock；不能等待需要该锁的 consumer，也不能递归移除自己。未结束的传输可返回 `-EBUSY` 让核心释放锁、保留图后显式重试。准备不是通用并发隔离或超时后的强制释放授权。

xHCI 在 probe 最前选择失败清理并登记准备动作。准备先封闭提交/轮询、撤销并排空 IRQ；同步传输仍持有 event ring 时返回 `-EBUSY`，不在 lifecycle mutex 下等待它。已取得固件控制权的实例还须确认 RUN/INTE 写回清零、RESET 完成、HCHalted、CNR 清零，以及 PCI BME 清除读回；全 1 读取或超时均保留控制器和所有尚未解绑的子节点。尚未取得控制权的早期失败未发布自己的 DMA 上下文，不执行控制器停机。成功证明在私有上下文中单向保留；之后已知 slot 用软件清理，不再向已停止的命令环提交 Disable Slot。remove 只在确认后释放控制器，start 失败也不再自行释放不确定的 ring 存储。

USB 的拓扑协调与锁外回调保留另按下述协议实现，不能仅凭 prepare hook 推导。HID/hub 的独立退出和 Configure Endpoint ring 的错误保留已按下述协议接入；其他 class 迁移仍需完善；xHCI 同步 bulk 已通过下述独立 DMA buffer 保留暂存数据，不再把调用者缓冲区交给硬件。关机/kexec 已按下述终止管线检查解绑错误，实际 provider 的停止证明仍须单独满足。

### 总线拓扑的原生生命周期协调

`driver_lifecycle_begin/end()` 复用核心现有的生命周期 mutex，供任务上下文把总线私有拓扑、注册和绑定操作放入同一个协调域。阻塞入口可在所属任务或启动上下文嵌套；它不增加 discovery 深度，不压制同步 probe。需要批量发现后再 probe 的总线仍使用 `driver_discovery_begin/end()`，不能用它代替普通进度协调。两组 API 均不固定设备、绑定或模块代码，调用者另持有效引用。

`driver_lifecycle_try_begin()` 只尝试新的最外层入口：成功返回 0，忙（包括本上下文已经持有）返回 `-EAGAIN`；只有成功后才配对 end。它避免进度轮询等候正在排空 class 调用的 teardown，也避免在部分修改的图中重入。成功取得协调权后的驱动操作仍可能进行有界硬件等待；这不是整个函数无等待的保证。三项接口均导出给模块，不用于 IRQ 或持有 spinlock 的路径，这三项函数不改公开布局；当前全局模块 ABI 为 13。

USB register/enumerate/scan/unregister 全程使用这个协调域，保护 HCD 注册表、端口状态和协议所有权。`usb_core_poll()` 忙时清除防重入标记并跳过，不推进轮询时间；class 持有协议存储期间通过 `usb_device_poll()` 尝试进度。后者忙返回 `-EAGAIN`，端口不再归属该设备、待移除或任一上游 HCD 正在退出返回 `-ENODEV`，均不进入 HCD。HID read/poll 在自身 spinlock 外使用此入口；不能阻塞等候 teardown 所持的生命周期锁。

同步回调可以注册或操作其他 HCD。相同 HCD 的递归 register/enumerate/unregister 返回 `-EBUSY`，嵌套 scan 跳过；活动下游操作（包括尚未发布的 start）也阻止注销上游。重复注册返回 `-EEXIST`，无效/未注册的 parent 链和正在退出的祖先拒绝新注册；已注册下游尚存时上游注销返回 `-EBUSY`。start 前预留注册表容量，防止不同 HCD 的重入注册越界。scan 每轮从当前注册表重新选择未访问地址，以最多 32 项的栈内集合处理回调造成的增删和压缩；同一地址被重新分配时可以延后到下一轮，绝不遍历借用的旧注册表行。

### xHCI 锁外回调与端口退役

xHCI 在 controller 锁下收集 completion，并在解锁前增加 controller、slot 和 endpoint 在途计数；回调结束重新取锁后递减。跨回调只保存标量 slot ID 与计数保留的私有 endpoint 记录，回调可释放自己的 URB，退役过程不会再次读取该 URB。controller prepare 发现同步传输或任何回调在途时返回 `-EBUSY`，保留图和存储，不能持生命周期锁等待这些回调。

端口 abort 在成功 Disable Slot 后，于同一锁内清 DCBAA、封闭 endpoint 并清 pending；对应 slot 回调未结束时返回 `-EBUSY`，保留其借用的 endpoint 记录和 ring，归零后才摘链释放。重试利用 DCBAA=0 的停止记录，不重复 Disable Slot；其他 slot 的回调不阻止该端口清理。EP0 control 和 interrupt submit 在锁下拒绝已停 slot，interrupt 拒绝发生在清 DMA buffer 和敲 doorbell 之前。

硬件可立即复用 Disable Slot 释放的编号，因此仅记住停止还不够。USB core 在同一物理控制器任一 root/hub 端口仍有 REMOVE_PENDING 时，暂停新的端口枚举，显式入口返回 `-EAGAIN`，scan 跳过且不扣枚举尝试次数；reset 回调后再检查停止和退役状态。直到回调、class 和协议所有者全部交还，才重新开放分配。已有设备的完成处理及其他物理控制器继续运行。该策略保持现有 slot token，不增加另一套公开 generation/所有者模型。

### 接口独立退出与 endpoint 释放

`usb_interface_release_endpoints(iface)` 从当前接口的 remove 回调调用，先由核心 quiesce/offline 并排空 class 调用。接口/协议存储由调用者保留；helper 在生命周期协调域内核对 stopping owner、plat_data 和端口归属，再逐个调用 `release_endpoint(hcd, dev, addr)`。在线或错误 owner 返回 `-EINVAL`，协议不再属于已登记 HCD 返回 `-ENODEV`，非空 endpoint 清单缺少回调返回 `-EOPNOTSUPP`；正数后端错误规范化为 `-EIO`。已证明整个 slot 停止时无需再次向硬件发 endpoint 命令。

HCD 回调返回 0 才证明该非 EP0 endpoint 不再 DMA/回调访问 URB 和 buffer，并释放其 controller 侧记录；之后同地址的再次释放无操作。失败保留仍存续的全部所有权，已释放的前序 endpoint 不回滚，重试只清理剩余项。此操作永久关闭当前 endpoint，不是单 URB 的取消/恢复 API；其他 endpoint 和 EP0 保持可用。HCD 字段最初在 ABI 10 扩展；当前 loader 和 drvctl 只接受 ABI 13，拒绝其他版本，不能通过强制读取短 vtable 兼容旧模块。

xHCI 在锁下设置单向 closing，阻止新提交和 completion 收集。该 endpoint 回调或同步传输仍在途时返回 `-EBUSY`，不持生命周期锁等待。Running 用 Stop Endpoint，Halted 用 Reset Endpoint，Error 用 Set TR Dequeue Pointer 转为 Stopped；已有 Stopped 不重发停止命令。只有输出 context 读回 Stopped 才登记停止，再用 Configure Endpoint 的 Drop Context 移除该 endpoint，保留 Slot/EP0 和其他 endpoint 的 Context Entries；Disabled 读回确认后才摘链释放。停止成功、Drop 失败会记住进度。fatal 软件标志不等于硬件停止，只有已确认 controller halt 或 slot 停止才允许软件回收。规则参照 [Intel xHCI 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf) 的 4.6.6、4.6.8–4.6.10。

HID/hub probe 在创建新的 host context 前发送标准 `SET_INTERFACE` 选择当前 altsetting，将设备侧 endpoint toggle/sequence 与新 context 对齐；该控制请求失败也进入可失败清理，不继续投递 URB；未实现对拒绝 SET_INTERFACE 的设备的安全恢复策略。设备侧规则参照 [USB 2.0 9.4.10](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/138/usb_5F00_20.pdf)。

Configure Endpoint 在硬件可能读 ring 之前，将 closing 记录交给 controller 链；失败不会释放可能仍被访问的 ring，重复配置同 slot/DCI 返回 `-EBUSY`。HID/hub probe 显式选择可失败 remove，保存部分实例；HID quiesce 封闭重提交意图，remove 只有 endpoint 释放成功才释放 URB/buffer。hub 先停下游 bus，再释放自己的 status endpoint，任一步失败保留上下文并显式重试。它们可在根控制器继续运行时独立注销并重新绑定。USB storage 的静态实例、独立退出和 BOT 复位仍需另外迁移；xHCI 同步 bulk 的调用者缓冲区已由下述暂存协议保护，不依赖这个 class 的静态数组继续存活。

### 已持有 PCI function 的查询

`pci_user_device_find/index()` 对已有内核绑定（包括只读 probe）或 `user_owned` function 读取已发布 BAR/IRQ 清单，不再 sizing BAR、重写资源、重新路由 IRQ 或清 BME。未知/停止设备仍被排除，BAR0 不可用仍返回错误。尚未被持有的 function 保留原有受 discovery 协调的 BAR 准备路径；这不是所有查询均无副作用的承诺。显式 BAR 重建和 BME 修改仍由拥有者先协调硬件停止，不能借元数据查询夺取运行中设备的控制权。

### 部分 USB 枚举的所有权交还

USB 使用自己的端口/slot 对象表达协议存储，不额外建立通用事务对象。`usb_core_enumerate_port()` 在 reset/slot 分配前预分配 `usb_device_t` 清理对象，确保硬件初始化后即使 OOM 也无需再分配才能保留失败状态。端口已有对象或 pending 清理时返回 `-EBUSY`，停止中的 HCD 返回 `-ENODEV`，不继续 reset 或覆盖旧对象。

HCD `alloc_slot(..., &slot)` 的错误码与所有权分别表达：核心预先清零输出；只要 `slot.hcd != 0`，包括非零返回，都将可清理的已知 slot 交给核心。`slot.address` 回报仍使用的软件地址或隐式分配的地址；未使用的软件预留立即归还。核心私有状态另外记录是否持有软件预留，停止后只归还该份预留；隐式地址即使与另一个控制器的软件地址同号，也不能释放别人的占用。xHCI 在 Enable Slot 得到有效 token 后保留它，Address Device 失败不再自行忽略 Disable Slot 的结果。hub 转发保留这一契约。没有 token 时，未知的控制器命令状态仍由 HCD 自己持有；零 token 的成功结果按 `-EIO` 处理，不能解释为已获得设备。

EP0 MPS、descriptor/configuration、解析/分配或部分 interface 发布失败均走 `usb_disconnect_port()`：先 offline 已发布 class，再检查 `abort_slot()`；只有成功才继续 interface REMOVE 和协议存储回收。停止失败/缺少回调时保留端口、slot、仍占用的软件地址、已分配的接口/endpoint 和 HCD 登记；负 errno 传播，正数规范化为 `-EIO`，缺少回调返回 `-EOPNOTSUPP`。清理成功返回原枚举错误；清理失败返回清理错误，同时诊断中保留两者。部分发布 helper 不再先解绑后停止硬件。

扫描总在新枚举前重试 pending 清理；HCD 注销也可重试。slot 已停止但 interface remove 失败时保留该阶段，之后不重复 abort。普通重新连接也必须等待旧清理完成。清理重试不增加枚举次数，也不清零失败预算；扫描对同一次连接最多尝试三次枚举，只有观察到物理断开才重新计数。`port_state` 的计数/阶段位是核心私有实现，HCD 不解释它。

这些 USB 清理变更保持其公开结构布局；当前模块 ABI 13 由 URB 布局扩展；GPU vtable 的能力回调引入于 ABI 12。它们保证已交还的已知 slot 不会因错误码而丢失所有权，不证明未知 Enable Slot 结果、HCD 自身 start/probe、致命控制器恢复或硬件停止超时已经解决；控制器必须在 `abort_slot` 返回 0 前确认不再访问相关存储。同步 control/descriptor 回调也必须在返回前解除对调用者临时缓冲区的访问，不能靠保留端口代替该回调契约。

## 原生 GPU 能力与默认选择

`gpu_dev_ops_t.get_capabilities(dev, &out)` 在模块 ABI 12 末尾追加，返回内核内部 `struct gpu_capabilities { uint64_t flags; }`，定义在 `drivers/gpu/capabilities.h`。它声明当前绑定已建立的实现角色，不推断寄存器潜在功能、驱动名或 virgl 回调表；不是用户 ABI，也不是 MM/DMA 访问授权。

| 标志 | 含义与约束 |
|---|---|
| `GPU_CAP_SCANOUT` | 有显示输出实现和 get_info；不表示连接状态，也不承诺 CPU 可映射 framebuffer |
| `GPU_CAP_RENDER` | 已实现的设备渲染后端；不要求 virgl 表，也不代表 Mesa/某个用户协议必然可用 |
| `GPU_CAP_RAM_SCANOUT` | 提供 owning RAM get_scanout，必须同时声明 SCANOUT 并提供该回调；获取时仍独立校验 VMO/范围/引用 |
| `GPU_CAP_BOOT_SCANOUT` | 保留固件建立的输出模式，必须有 SCANOUT；同等渲染能力时作为原生管理输出的回退，不表示仍调用固件 |

回调只复制缓存的已建立状态，不做硬件 I/O、分配、VFS、生命周期调用或等待。注册在串行绑定所有权中查询，可早于 class publication；重新选择在候选的 active class call 内查询。两处均在 `g_gpu_lock` 外执行。标志在同一绑定内保持稳定，改变角色需退出/重新绑定；quiesce 可以返回 `-ENODEV`。simplefb 与 lsdc-boot 声明 SCANOUT/BOOT_SCANOUT，VMSVGA 声明 SCANOUT；virtio-gpu 在有效且未 closing 时声明 SCANOUT/RAM_SCANOUT，只有 VIRGL 实际协商成功才声明 RENDER。旧 `get_features`/capset 继续服务具体 virgl 协议，不能代替原生角色模型。

消费者在已有 `gpu_device_access` 内调用导出的 `gpu_device_capabilities_get(&access, &out)`，不新增 lease、存储引用或生命周期锁。成功后可保留复制的标志，不能因此在 access 结束后继续使用 ops。有效 out 在所有失败路径清零：无有效 access 返回 `-ENODEV`，缺少回调返回 `-EOPNOTSUPP`，未知位、SCANOUT 缺少 get_info、RAM_SCANOUT 缺少 SCANOUT/get_scanout 或 BOOT_SCANOUT 缺少 SCANOUT 返回 `-EINVAL`；正数 provider 错误规范化为 `-EIO`，负 errno 保留。NULL out 返回 `-EINVAL`。空角色集合允许发布元数据，但不进入默认输出。

`gpu_device_register()` 要求这个显式回调；缺少/畸形/失败声明不改变默认选择或取得其额外 device pin，probe 必须检查失败并完成自己的清理。只有 SCANOUT 有资格进入默认输出，headless RENDER 仍可作为 DISPLAY class 元数据发布。核心先比较 RENDER，再在同级偏好非 BOOT_SCANOUT，等分保持已有选择；注册与重新选择使用同一规则，遍历只有找到最高等级才提前结束。这样原生 2D 初始化后不会因未实现 3D 而继续让旧 GOP 视图占据默认输出，也不会把仅提供 virgl 函数表的 2D 设备当成渲染后端。优先级由核心从角色计算，不允许驱动提供任意数字。

上述是默认策略和能力发现，不证明硬件健康、授权接管固件存储或撤销旧物理 VMA；不建立通用 job/BO/GPUVM、格式/队列限制协商或多 GPU 用户节点。card0/renderD128 仍是默认聚合入口，headless 内部发布不等于用户态已能选择它。各次操作仍须取得 class access，持久 backing 仍需独立 owning VMO/DMA 引用及停止契约。

当前所有 `.a20drv` 与用户驱动描述符必须按 ABI 13 重建。kernel descriptor guard、loader 入口与 `drvctl` 拒绝旧版本，不能通过强制读取短表兼容。RAM `get_scanout` 引入于 ABI 11，原生能力查询引入于 ABI 12；ABI 13 增加 URB 的实际 bulk 长度。

## GPU publication 与作用域调用

GPU 的默认选择是既有 DISPLAY class 上的策略，不另建发布注册表。`gpu_device_register/unregister()` 在原生 lifecycle 协调域内更新选择；普通 probe 仍同步执行。选择持有一个 device 元数据引用，替换/撤销后在选择 spinlock 外交还，不持有绑定、模块或硬件使用权。优先级来自上节显式原生角色：只选择 SCANOUT，先比较 RENDER，同级优先原生管理输出而保留固件输出作为回退。不再检查旧 virgl 函数表存在性，也不在选择锁下调用或解引用可撤销的驱动实现。

`gpu_device_default_get()` 在锁内临时保留所选 device，锁外取得其 class publication 引用，再交还临时 device 引用；调用者最终 `class_device_put()`。返回值可能已经 offline；probe 尚未发布 class、publication 已撤销或无选择时返回 NULL。该读取入口不等待 lifecycle mutex，避免与持该锁排空 active call 的退出路径互相等待。重新选择遍历在线 DISPLAY publication，不受旧的 16 项限制。共享 class rank 枚举先过滤 offline 再计序号，get_by_type/get_nth 不会因仍在 quiesce/drain 的离线项而提前返回 NULL；get_by_index 仍是独立的稳定 publication index 查询，离线 index 不会转指其他 publication。

`gpu_device_access_begin(publication, &access)` 要求调用者持有 publication 引用。它另取一份 class 引用，再经 `class_device_ops_begin(..., DEV_CLASS_DISPLAY, ...)` 准入；成功后 `access.device` 和 `access.ops` 属于同一个绑定，直到 `gpu_device_access_end()`。调用者可在 begin 成功后交还原引用。`gpu_device_default_begin(&access)` 合并默认选择与准入。有效输出参数在失败时清零；无在线 DISPLAY publication 返回 `-ENODEV`，NULL 输出返回 `-EINVAL`。旧 publication 不会跟随同一 device 的新绑定，也不会跟随默认 GPU promotion。

```c
struct gpu_device_access access;
int ret = gpu_device_default_begin(&access);
if (ret) return ret;
ret = access.ops->get_info
    ? access.ops->get_info(access.device, &width, &height, &bpp)
    : -EOPNOTSUPP;
gpu_device_access_end(&access);
return ret;
```

这些入口用于任务上下文。成功 begin 必须在所有返回路径配对 end；不能复制 live access 或覆盖尚未 end 的输出，也不能等待需要该 active call 排空的生命周期修改。end 先清零输出，再结束 active call、交还独立 class 引用，允许最后 put 在锁外回收 publication/device；NULL 或已 end 的对象可再次 end。active call 保护调用期间的绑定、实例和操作表，quiesce 仍可封闭硬件提交并使调用报错；它不保证硬件持续运行，不提供 DMA 停止证明，也不保留方法返回的 framebuffer 存储。

fbdev 的一次 ioctl、mmap 安装及后台一次刷新，以及 sysfs GPU 属性的一次内容生成，已捕获同一 publication/device/ops 并在结束时交还。sysfs 只保存复制后的内容，不在读取期间借用已结束的驱动调用；不同 open 的属性仍可观察到不同默认 GPU。fbdev 文件尚不固定到某设备，下一次操作可能选择新默认设备。mmap 安装返回后 active call 已结束；有 get_scanout 的 RAM 路径由普通 VMA 保留 VMO，旧 get_fb 物理路径仍借用 backing。

`gpu_device_get_default()` 保留为旧的借用快照，使用它必须由外层协调整个使用期间的生命周期，不能据此读取可失效的 ops。DRM 已按下述协议迁移到固定文件 publication、每 publication 状态及显式 scoped access；厂商 BO/context/request/fence 与旧物理映射退役仍需后续实现。上述消费者改造不等于 GPU 热拔插或持久资源生命周期已经完成。四项 access 函数已导出，device/driver 布局不变；RAM scanout 回调引入于 ABI 11；上述能力回调引入于 ABI 12；当前模块 ABI 为 13。

## 连续 RAM VMO 与 owning scanout

`vmo_create_contiguous(bytes, &memory)` 是内核 MM 的专用 RAM 工厂，不是新用户态对象类型或设备 DMA 分配接口。调用者在可分配的任务上下文、不持 MM/spinlock；零长度/NULL 输出返回 `-EINVAL`，超出 `PAGE_SIZE << MAX_ORDER` 返回 `-E2BIG`，分配失败返回 `-ENOMEM`，地址/CPU alias 范围异常返回 `-ERANGE`，PFA 转换失败返回 `-EIO`。错误清空有效输出且不遗留 owner。长度向页取整，物理地址 0 有效，全部暴露字节包括末页 padding 先清零。

工厂在发布前分配高阶块，经已有 `pfa_split_allocated()` 一次转换为各有独立引用的 canonical order-0 页；逻辑范围只保留所需前缀，未用的幂次尾部通过 frame_put 交还。转换前的失败按原 order 释放，转换后不再原阶释放。随后发布内部 `VMO_CONTIGUOUS` 类型，沿用 VMO 原有结构、Rust ref/pin/destroy 和 TLB 后 frame_put 路径。`vmo_contiguous_info(memory, &physical, &bytes)` 要求调用者持有 VMO 引用，返回稳定的 CPU 物理基址和页取整 extent；输出在失败时清零。不可变指 extent 与 canonical slot，不是像素内容只读：公开 resize 返回 `-EOPNOTSUPP`，直接 Rust resize 的锁边界也拒绝修改（错误返回沿用旧约定），缺失 slot 返回 `-EIO`，不能 demand fault 出新页填洞。

该工厂不捕获设备 mask、不建立 IOVA、不授予 DMA 权限，也不转移 cgroup charge。它适合可信内核 RAM backing；有 DMA 地址/权限要求时继续通过 domain 和 `driver_dma_map_vmo()` 检查。kind 4 已加入 canonical pin 白名单；这些页在转换后具有真实 order-0 引用，并非把高阶尾页伪装成可 pin 页。普通可变 VMO 的页快照仍不冻结 extent，连续 VMO 的 extent 固定来自其自身类型约束。

GPU vtable 的 `get_scanout(device, &snapshot)` 引入于 ABI 11；ABI **12** 另追加原生能力查询；当前模块 ABI 为 **13**，旧模块必须重建。`struct gpu_scanout { struct vmo *memory; uint64_t offset; size_t size; }` 持有一份 VMO 引用；offset 页对齐，size 是正的可见字节长度，向页取整的覆盖范围必须完全落在上述不可变 RAM extent 中。提供者须在有效 class 调用内、与模式替换协调地捕获同一 backing/offset/size；即使返回错误，只要 memory 非空，也向 helper 交出该份引用。没有回调返回 `-EOPNOTSUPP`；存在回调但失败不能降级成借用物理映射。当前只接受连续 RAM，不把 VRAM/MMIO 当作普通 VMO。

消费者在有效 `gpu_device_access` 内调用 `gpu_device_scanout_get()`，helper 验证类型、对齐、溢出和整页覆盖；错误清理部分输出，正数错误规范化为 `-EIO`。成功后 snapshot 独立于 class/设备/模块存活，可立即结束 access；最后在任务上下文 `gpu_scanout_put()`，该函数先清空描述符再交还 VMO 引用，NULL/已 put 描述符无操作。不能复制 live snapshot 或覆盖尚未 put 的输出。最终存储回收只进入内核 MM，不回调已经解绑的驱动。

virtio-gpu 的 RAM framebuffer 已由该工厂创建；producer、临时 snapshot 和 VMA 分别持有 VMO 引用。MMU fbdev mmap/旧 `FBIO_MAP_FB` 优先走 `get_scanout`，内核 adapter 以 `mm_mmap_vmo_cap()` 接入既有 Rust VMO 放置/权限事务。wrapper 检查初始权限不超 ceiling，负责锁外 TLB begin、MM lock、locked mapper、锁外 finish/deferred release；VMA 自持 VMO，未新增 backing 注册表、VMA 字段或模块释放回调。映射使用 Normal RAM 属性、共享写入并支持 fork/split；mprotect 不得超过创建时有效硬件权限。末页 padding 可见且已清零。旧 ioctl 仍用固定 NOREPLACE。

`get_fb` 继续是借用 CPU PA/可见长度的兼容查询；没有 owning 回调的 simplefb/VRAM 路径继续走下述物理视图，NOMMU 也保留旧直接地址行为。fbdev 的每次操作仍重新选择默认设备，尚未绑定每个 open 的 GPU 身份。RAM VMO 存活不证明 transport 已停止：virtio-gpu 的 reset 读回、失败初始化保留与内部 DMA/IRQ 退役已接入下述协议；DRM 已另按下述协议迁移文件、状态和调用；厂商 BO/VRAM/GPUVM/请求队列仍待实现。

## 固定 resident RAM VMO 与预算 owner

`vmo_create_resident(bytes, owner, &memory)` 是内核 MM 的 RAM 工厂，返回内部 `VMO_RESIDENT`（kind 5），不是 Linux BO 类型或新用户 ABI。它在发布前以 order-0 页完整物化并清零页取整范围，包括末页 padding；不需要高阶连续块，也不承诺物理连续性。extent 与 canonical slot 固定、数据可写。公开 vmo_create 拒绝直接创建 kind 5；resize（含同大小、增长和收缩）由公开 C 入口返回 `-EOPNOTSUPP`，直接 Rust resize 的锁边界同样拒绝修改，但沿用其旧错误返回约定。物化入口遇到 resident 空 slot 返回 `-EIO`，不再补页。vmo_contiguous_info 拒绝这种对象；owning scanout 目前仍只接受 kind 4，不把 scatter RAM 当成连续 framebuffer。

调用限可分配的任务上下文，不持 MM/VMO/spinlock 或 driver store mutex。owner 指向调用期间有效的内部 cgroup；工厂 pin owner 并预先为全部页取整容量 charge，VMO 最后 release 才 uncharge/unpin。该 charge 使用现有共享匿名 backing 记账，避免 legacy task RSS teardown 扣掉仍由 VMO/VMA 持有的物理内存；不是按每个映射重复计费，也不是 GPU/VRAM 专属预算。NULL 明确选择无 cgroup charge 的内核存储，charge 失败不能降级成 NULL。不因共享到另一文件/进程而转移 owner。页表/对象元数据仍遵循现有 heap 记账。

失败清空有效输出：零长度/NULL 输出返回 `-EINVAL`，页取整或 dense table 无法表示返回 `-E2BIG`，预算/元数据/页面分配失败返回 `-ENOMEM`，CPU alias 异常返回 `-ERANGE`。参数预检在 charge 前；预算拒绝在分配前。对象构造中途失败复用原有 Rust destructor，交还已获得的每个 canonical 页、全部预收 charge 与 owner pin；没有发布半成品、第二套页注册表或 driver 回调。VMO/VMA 引用、页 pin、DMA 地址和硬件停止继续是不同契约。

canonical snapshot 与 driver_dma_map_vmo 已接受 kind 5；`driver_dma_map_vmo` 的 mapping 仍只持独立帧引用，不保留源 VMO、预算或访问 lease；新增 `driver_dma_map_owned_vmo` 可显式保留固定 RAM VMO 的整个 owner。resident 的固定 extent 来自工厂。需要预算 owner 在请求期间存活时，可显式使用 owned 映射或由请求另持 VMO 引用；不能把完成 fence、page pin 或 metadata 离线解释为该引用或硬件停止。工厂不检查设备 mask、不建立 IOVA、不授予 DMA 权限；缓存同步、页地址查询、可失败 unmap/停止仍由既有 domain 协议负责。

DRM 的 GEM_CREATE、CREATE_DUMB 与 VIRTGPU_RESOURCE_CREATE 已使用该工厂。adapter 在 GPU mutex 前通过 cg_task_group_pin 捕获当前分配任务的 cgroup，操作结束后交还临时 pin；工厂为 VMO 保留自己的引用。GEM/handle 元数据和 RAM 构造位于 store mutex 外，仅最终 ID/handle/对象发布进入 store mutex；没有在硬件命令路径延迟物化 backing。创建失败传播原生 errno，输出失败或正常 close 沿用每文件 owner/失败退休；framebuffer、导出 pin 和普通 VMA 仍各自保留 VMO，因此 backing 的原始 charge 随 VMO 一起存活。该 RAM 底座不提供 BO 迁移/回收策略、VRAM、GPUVM、任务/请求调度或共享访问权。

## VirtIO reset 确认与 GPU 退出

`virtio_transport_reset(transport, timeout_ms)` 位于原生 bus 层并导出给模块，不改变 transport vtable 或当前 ABI 13。任务上下文、不持 spinlock，调用者持有 transport/MMIO/代码和设备所有权，并排除新提交、并发 reset 及重新初始化。参数要求完整 read32/write32、1–1000 ms，否则 `-EINVAL` 且不访问寄存器。函数写 status=0、执行 barrier 后反复读回，只有读到 0 才成功。规则依据 [OASIS VirtIO 1.3 的 2.4、4.1.4.3.2](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)：设备在报告 reset 完成后停止队列交互和通知，PCI 重新初始化须等 status 读回 0。

时间预算以频率分解计算 ceil，避免毫秒乘法溢出；elapsed 差值允许计数器回绕，起点 0 也是有效时钟。另有 100 万次读取上限，保护早期启动、频率未知或停滞时钟；因此 poll 预算可能先于时间预算耗尽，并非保证等待足额毫秒。任务存在时每 256 次尝试 yield；无任务时有界 poll。耗尽返回 `-ETIMEDOUT`，不隐式清理任何资源。它只确认合规 transport 的状态，不排空软件调用、不注销 IRQ、不解除 IOMMU 或回收存储，也不证明恶意/损坏硬件或物理拔除安全。

virtio-gpu 在任何 PCI/MMIO 初始化前选择 `device_probe_use_remove()`，并把已初始化 mutex/wait queue 的部分实例交给 dev->drv_priv。验证 transport 身份后，首次 reset 也必须读回确认，再协商特性/发布 controlq。失败仍可设置 FAILED，但由原生 offline/stop/quiesce/drain/remove 管线清理；FAILED 本身不授权释放 DMA。reset 不完成时返回错误，核心保留绑定、原 probe_error、MMIO/devres、IRQ、命令暂存和 framebuffer owner，后续显式重试同一个 remove。

四种 controlq 命令共用 native queue/request 执行路径；该 wire queue 仍仅一个 active chain，command mutex 保护 staging 和 descriptor。closing 返回 ENODEV，故障队列返回 EIO；启动 polling 与运行时 IRQ-assisted wait 复用同一 publication 后端。超时、错误 used id 或 index 增量不等于 1 都封闭 staging，保留未退休 request/fence 与实例 owner，不再扩容/复用潜在硬件可达存储。未发布的分配失败不把队列判为故障；已确认返回链的设备错误响应可以正常退休。

原生 execution stop 在 class drain 前关闭 admission/closing，检查 command_active 并 trylock；忙返回 EBUSY，既有 issuer 观察 closing 后退出，调用者再重试。持锁执行有界 reset，确认 status=0 后 free_irq 排空 handler，队列随后退休未完成请求。reset 失败保留整个离线绑定、IRQ、staging、backing 与 pending fence。所有请求安全退休后才进入 class drain 和 remove，remove 交还默认选择、动态 staging/framebuffer owner 及队列 caller 引用；最后解开实例 mutex 再 put allocation。snapshot/VMA 可继续保留旧 RAM。

### VirtIO GPU 的每设备实例

virtio-gpu 不再使用全局静态实例或 owner 槽。每次 probe 在选择原生失败清理后，先以 `driver_dma_buffer_create(dev, sizeof(virtio_gpu_inst_t), __alignof__(virtio_gpu_inst_t), &allocation)` 分配独立的零填充、物理连续且满足当前 mask 的实例。该 owner 保留设备元数据，覆盖内嵌 controlq、固定命令暂存、mutex 和等待队列；transport、EDID、feature、fence 序号、动态 staging 与 framebuffer VMO 引用也都在同一 `dev->drv_priv` 中。每个绑定仍只有一个同步 controlq，不是每请求或每进程队列。驱动无需另建实例注册表。

原生 instance allocation 分配失败保留 errno，不访问 transport、不发布 drv_priv，也不留下设备 pin；后续 queue 分配失败由已选择的 managed failed-probe 清理回收部分实例；同一 device 已有 drv_priv 返回 `-EBUSY`，其他 device 不受影响。probe 后续失败交给 execution stop/quiesce/remove，保留这一个部分实例。关闭、未知完成和 reset 失败只封闭该设备；不覆盖或重置另一设备的队列。退出先经 execution stop 确认 reset、排空 IRQ，再由 remove 注销默认选择并清 drv_priv、交还动态 staging/framebuffer producer 引用；最后释放 command mutex，才 put 包含 mutex 本身的实例 allocation。失败则完整保留 allocation、设备 pin、IRQ 和 backing，显式重试同一绑定。

这复用已有 DMA buffer 的分配与所有权，实例化不扩展 device/driver/GPU vtable；ABI 12 引入的能力回调见上文。allocation 地址是 CPU 物理地址，现有队列仍使用平台恒等 DMA 假设；没有由此建立 IOMMU domain、GPUVM、厂商任务/fence 或安全物理拔除协议。旧 scanout 的 VMO/VMA 和 DRM 的 publication/GEM 各自持有独立所有权，实例释放不撤销这些 CPU 存储引用。两个 VirtIO 设备能独立运行，不表示 card0/renderD128 默认聚合入口已变成多 GPU 节点枚举或跨 GPU PRIME attach。

## DRM 文件、命名空间与失败退休

`drm_create_vfile_for(publication, &file)` 是内核 adapter 的显式构造接口：在 DISPLAY class access 内捕获这个 publication，并要求其驱动提供显式 remove；缺少停止契约返回 `-EOPNOTSUPP`，离线返回 `-ENODEV`，失败清空输出。`drm_open_vfile(&file)` 只在 open 时读取一次默认 publication；旧指针包装 `drm_create_vfile()` 仍以 NULL 表示错误。这些入口不新增用户 ABI 或模块导出。

每个 DRM 文件固定一个 publication。GEM 的 backing 已由上述固定 resident RAM 工厂构造，预算 owner 捕获与物化都不在 store 锁内。同一 publication 的对象 ID、legacy name、CRTC、EDID、virgl ID 与 vblank 状态属于同一 namespace，不同 publication 分区；同一 device 重新绑定也创建新 namespace。每个 open file 独立持有 handle 集合与 framebuffer；数值 ID 在 namespace 内分配，但只有该文件拥有 handle 才能用于 GEM、mmap、PRIME 导出、virtgpu 操作和 ADDFB。猜中另一文件的 ID 返回 `-ENOENT`。dup/fork 共享同一 vfile，最后一个文件引用关闭才交还这些 owner。私有 namespace 是 class 的消费者状态，没有第二个设备发布入口；当前仍为 card0/renderD128 聚合入口，没有多 GPU 节点枚举。

DRM ioctl/mmap 与需要设备访问的 dma-buf sync，显式携带栈上 `drm_call`，从文件或 GEM 的原始 publication 取得同一 device/ops。先等每设备操作 mutex，再取得 active class call，避免持租约排队阻塞退出。操作另持 CPU namespace 使用计数，直到解锁、唤醒及最终收尾结束，最后一个 dma-buf close 不能让 worker 提前释放 namespace。read/poll 只访问本文件事件，不持硬件租约睡眠；离线 observer 唤醒等待者，read 在排空已有事件后返回 `-ENODEV`，poll 报 HUP/ERR，并在连接等待队列后重查事件和离线条件以防丢失唤醒。

PRIME dma-buf pin 固定原 GEM 和 CPU 存储；同 publication 导入为接收文件显式建立一个 handle owner，数值沿用对象 ID；同一文件重复导入幂等，不叠加隐形引用。不同 publication 返回 `-EXDEV`，没有隐式跨 GPU DMA attach。fd 引用解析和最终 put，以及导出文件的 fd 安装/失败 close，位于 GPU 操作作用域之外，不能在持 GPU A mutex 时触发 GPU B 的最后一次 close。持有 dma-buf 的 CPU VMO 映射可在设备离线后继续存在；sync/DRM 新操作不因此获得离线硬件访问权。

resource/context 的所选 ID 在发命令前成为清理身份，未知完成不能解释为对象不存在或 ready。成功 resource_unref/ctx_destroy 才确认该对象不再被设备访问；非零结果或缺少在线 ops 将原对象和 VMO 放入每设备失败列表，保留身份重试，正数错误规范化为 `-EIO`。在线下一次操作或原生 worker 做一次重试，不忙循环；offline 不授权释放。停止凭据成立后，worker 不再调用旧驱动代码，清空显示/host 身份并释放无 pin 的 GEM 和失败 context；dma-buf pin 与普通 VMA 的 VMO 引用继续保留 CPU 存储。无文件、操作、GEM 或失败清理对象时，才移除离线 namespace 并交还 publication 引用。

正常 close 先移除该文件的 framebuffer，再交还全部 handle，最后处理 host context；不再把所有在线 GEM 保留到设备停止。`GEM_CLOSE`/`DESTROY_DUMB` 只撤销调用文件的 owner，其他导入文件、framebuffer、dma-buf pin 与普通 VMA 各自保留存储。失败 host 清理仍进入原有保留列表。停止凭据成立后，worker 可退休硬件对象和未被 dma-buf pin 保留的 GEM；离线文件的 handle 条目只保存 ID，不保存已退休 GEM 指针，最后 close 安全清空条目。在线 namespace 保留共享显示/缓存元数据；全局 reaper 服务随内核运行。离线未确认停止或失败清理按 100 ms 延迟重试；已经停止但仍有文件/dma-buf 的 namespace 不持续轮询，最终 close 重新调度回收。该协议依赖驱动诚实履行 remove，不是硬件故障后的强制释放。

DRM 和 dma-buf mmap 复用 `mm_mmap_vmo_cap()`：只允许共享非空读写及可选 FIXED/NOREPLACE，拒绝 EXEC、private、未知 flags、溢出/越界和未对齐 offset，保留原生 errno 与创建权限 ceiling。DRM 假 offset 仍编码 handle，拒绝高位截断 alias，不是 GPUVM 或完整 mmap-offset 区间分配器。GEM_CREATE/CREATE_DUMB 输出复制失败撤销未交付 handle。PRIME/GEM_OPEN/GETFB 导入或重建 handle 时，分配失败不安装引用，输出复制失败仅撤销本次新增 owner，已有 owner 保持。原生 VMA 自持 VMO，既有用户映射不需要在最终回收时进入驱动模块。

framebuffer 归创建文件所有。RMFB/SETCRTC/PAGE_FLIP 拒绝另一文件的 framebuffer；owner 即使先 GEM_CLOSE，也可通过 framebuffer 自持引用继续呈现。GETFB/GETFB2 可为 framebuffer owner 重建 handle；其他文件只查询元数据，不获得新 backing 权限，除非已经显式导入同一 GEM。查询失败回滚新引用。未经完整仲裁的 `is_master` 标志不授予这种访问。

GEM_FLINK/GEM_OPEN 作为 DRM legacy 显式共享入口保留在 adapter：name 由内核分配、重复 FLINK 稳定，不能用输入 name 覆盖其他对象；最后一个 handle owner 退出时撤销 name，name 本身不保留 backing。它不是原生能力句柄，也不提供保密共享；新原生机制继续使用 VMO/类型化对象。当前尚缺完整 DRM master/auth、DMA-BUF 只读权限传播、命令流验证、预算及厂商请求生命周期，不能将本轮 handle 所有权描述为完整进程/GPU 隔离。

这些消费者更改不扩展公开布局，当前模块 ABI 为 13，不实现厂商 BO/VRAM、驻留/预算、GPUVM、通用 job scheduler、DRM 同步 UAPI、完整模块卸载或实体 GPU 加速。fbdev open 仍按操作选择默认设备，旧 get_fb 持久物理视图仍借用存储。

## 驱动 CPU 物理映射事务

`mm_mmap_physical(mm, addr, len, prot, flags, physical, memory_flags)` 位于原生 MM 层，供内核 class adapter 和用户驱动服务安装 CPU 物理视图。调用者在任务上下文持有 mm 引用，不持 MM/spinlock。它复用 `mmap_policy.rs` 的 fixed/hint 地址策略，不在 fbdev 或用户驱动各维护一套 VMA/PTE 事务；C 只提供字段、CPU 页表、锁与 TLB 桥接。

物理起点须页对齐，长度向页取整，整段不得越过 uint64 或目标 paddr_t 的可表示范围；物理地址 0 有效。只接受非空 READ/WRITE、MAP_SHARED 及可选 FIXED/NOREPLACE；未知/私有映射返回 `-EINVAL`，EXEC 返回 `-EACCES`，NOMMU 返回 `-EOPNOTSUPP`。memory_flags 只能包含 `PTE_DEVICE`/`PTE_MAT1`，由可信调用者按 CPU 别名选择；该函数不核准任意物理区间，也不检查设备资源所有权。用户驱动仍先检查白名单，fbdev 的旧 get_fb 路径仍先取得 class access 并核对 scanout 范围和 boot framebuffer 的 CPU 属性。不能用 DMA/IOVA 地址代替 physical。

固定地址须非零、页对齐且全范围合法；NOREPLACE 对任何 VMA 重叠返回 `-EEXIST`，FIXED 调用同一 MM 事务中的 `mm_munmap_locked()` 并传播失败。hint 检查整个目标区间，碰撞或越界时重新找 gap，不再只检查起始点。安装前还检查整段现存叶 PTE，包括物理地址 0 和大页，拒绝覆盖没有 VMA 描述的旧映射。MAP_FIXED 不是保存旧映射的回滚事务：旧映射移除后发生分配/安装失败，不恢复旧内容。

先在锁外分配未发布 VMA，再开启 `mm->tlb_lock` 失效事务并取得 `mm->lock`。全部 PTE 安装成功才发布一个 `VM_SHARED|VM_PFNMAP|VM_DONTFORK` 记录，记录物理 backing_offset 并更新 total_vm/RSS；以 `VM_NATIVE_CAP` 及实际 READ/WRITE 权限记录不可扩大的 mprotect ceiling。失败清理当前页的半建空路径，再逆序撤销已安装前缀；锁外完成本地/远端 TLB 失效和页表退休，随后交还未发布记录并执行 deferred release。保护变更保留原 memory type 与 dirty 历史，不能把 x86 设备 PCD/PWT 丢成普通 RAM 类型。

fbdev 的旧 get_fb mmap 和 `FBIO_MAP_FB` 分支委托此事务，后者使用固定 NOREPLACE；用户驱动 `udriver_map_mmio()` 保留原 0/-1 与失败不改 out_va 的接口，内部同样使用这条 MM 路径及 Device 类型。旧 NOMMU framebuffer 直接地址接口保持原有语义，不由此获得 MMU 权限或撤销能力。此物理 mapper 不改变 device/driver 布局；当前 GPU vtable 的 ABI 12 扩展见上文。用户地址空间操作由内核 adapter 承担，没有为设备模块新增用户映射入口。

这是**借用物理存储的视图**：不取得 PFA frame、VMO、device、resource 或模块引用，不做 IOMMU/DMA 隔离。调用者须使 backing 在全部 VMA 和旧 TLB 访问结束前有效，或在回收/复用前撤销所有视图。fbdev 的旧 get_fb 物理分支仍无跨解绑的 backing 引用和映射撤销协议，不能据此安全释放持久用户映射的 backing；现有驱动退出仍需独立推进。这条事务关闭安装/回滚问题，不关闭存储所有权缺口。

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

### PCI requester 集合占用

`pci_requester_reserve(owner, segment, ids, count, &reservation)` 在任务上下文原子占用显式 requester 集合。调用者持有已注册 owner 的有效引用；输入在返回前保持不变，核心复制、排序并去重，输入数量为 1..`PCI_REQUESTER_SET_MAX`（128）。requester 是包含 bus/device/function 的完整 16 位 ID，0 与 0xffff 均是合法键。segment 仅支持 0，非零返回 `-EOPNOTSUPP`；超出输入上限返回 `-E2BIG`，参数错误 `-EINVAL`，未注册或无法保留 owner 返回 `-ENODEV`，分配失败 `-ENOMEM`，任一键冲突 `-EBUSY`；失败清空输出且不占用集合中的其他键。

同一 owner 也不能重复占用重叠集合；没有 MMIO claim 的同 owner 共享例外。冲突检查与发布由 discovery/lifecycle mutex 串行化，成功保留 owner 引用。已占用的键独立于 PCI 枚举槽、驱动绑定和控制器上下文：owner 注销、同 BDF 换设备或控制器重绑均不自动撤销。`pci_requester_release(reservation)` 在任务上下文显式释放，NULL 无操作，非空句柄准确释放一次；硬件停止/失效不确定时必须继续保留句柄，不能因元数据失效就回收 namespace。

该接口只管理协作软件的地址键，不推断 alias、不要求键对应已发布 function，也不证明 PCI 路由、ACS/peer 隔离或设备停止。调用者须提供经固件/硬件策略验证的完整集合；raw 配置写入和未接入该接口的旧 provider 不被强制拦截。它不创建 group 编号、PID 身份或隐式 parent/dependency，也不触碰硬件。

## 物理存储保留对象

头文件：`kernel/include/drivers/core/memory.h`；保留对象与 claim 共用 `driver_core.c` 的 registry 锁，启动导入位于 `kernel/drivers/core/memory_boot.c`。这是原生 CPU 物理存储清单，独立于 device/binding；RAM 无须伪装成设备，驱动结构布局和 ABI 10 保持不变。reserve/release 已进入模块导出表，启动导入函数只供内核启动路径使用。

`driver_memory_reserve(ranges, count, &reservation)` 在任务上下文复制非空数组，每项为 inclusive `start/end`。地址 0 和 `UINT64_MAX` 均有效，反向区间返回 `-EINVAL`，分配尺寸溢出返回 `-EOVERFLOW`，分配失败返回 `-ENOMEM`。输入只需在函数返回前保持有效且不可变，输出不保留借用指针。副本在锁外排序并合并相邻/重叠区间，锁内使用二分查询；不建立 Linux resource tree 或推断父窗口共享。

发布前，在 lifecycle mutex 内核对当前所有 `RES_MMIO` 声明，再在 registry 锁内核对 active MMIO claims；冲突返回 `-EBUSY`。原批次任何一项失败都不发布部分保护。`RES_MEM` 可以描述同一存储，port I/O 使用独立地址空间。多个存储保留对象可以重叠，独立释放其中一个不会撤销另一个的保护。畸形的已发布资源表返回 `-EINVAL`。

对象存续时，基本/checked claim、受管 MMIO 映射、PCI 的两类范围查询和新的 MMIO 设备发布都不能覆盖该存储；同 owner 的 claim 例外不适用。保留发布与基本 claim 共享 registry 锁，与设备发布共享 lifecycle 协调，所以并发竞争只能一方成功。已经持有 claim 的映射即使正在 unmap，也会阻止新存储保留；仍须先完成 unmap，再释放 claim。

调用者持有成功句柄，恰好调用一次 `driver_memory_release()`；NULL 无操作。释放在 registry 锁内撤销保护，在锁外回收副本。对象没有 device 引用、自动 devres 回收或物理页所有权；它不会为 PFA 预留/分配页面，也不冻结 raw 映射、物理硬件或 DMA。它只约束请求的物理区间，不能替代架构实际映射粒度、缓存属性和永久别名的审查。

hosted 启动在 PFA 和 `driver_core_init()` 完成后、发布设备前调用 `driver_memory_protect_boot()`。它复制不可变的 PFA RAM 几何，并通过 `acpi_reserve_tables()` 导入 RSDP、根表、各 SDT 和 FADT 引用的 DSDT/FACS；ACPI 表的边界向外取整到物理页。先计数与校验，再填入副本，最后一次性发布；两遍不一致或任意表读取/校验失败均不留下部分保护，启动路径会停止设备初始化。没有 ACPI context 的平台仍保护受管 RAM；没有有效 RAM 清单则拒绝初始化。成功后保留对象存续至内核结束，重复调用返回 `-EALREADY`。

`DRIVER_MEMORY: protected ram_ranges=... firmware_ranges=...` 报告导入前的区间/表回调数量，内部可以合并。该导入不等于完整固件内存图：PFA 范围之外的 RAM、其他启动数据、全部 DT reserved-memory 和运行时热增内存仍需各自的可信来源；不能把所有固件 reserved 类型都误当成不可用 MMIO，也不能从“未发现冲突”推导硬件启用许可。

## 独立 DMA buffer 所有权

头文件：`kernel/include/drivers/core/dma_buffer.h`，由 `driver_hwapi.h` 引入。`driver_dma_buffer_create(device, size, alignment, &buffer)` 在任务上下文分配清零、物理连续的 RAM，保存实际 compact PFN 和分配阶数；size 和 DMA 地址对齐要求可以独立，33 字节、64 KiB 对齐也会按实际高阶块回收。对齐须为非零二次幂，size/alignment 上限为 `PAGE_SIZE << MAX_ORDER`（当前 8 MiB）。旧 raw aligned helper 仍限制对齐不超过 PAGE_SIZE，释放协议保持不变。

对象捕获设备 DMA mask，通过受限 PFA 选择块，并核对整个分配块的物理窗口和请求对齐。失败清空输出：参数/mask 错误 `-EINVAL`，超上限 `-E2BIG`，架构未就绪 `-EOPNOTSUPP`，owner 不可用 `-ENODEV`，元数据/页分配失败 `-ENOMEM`，不可用 allocator 地址 `-ERANGE`，分配期间 mask 改变 `-EAGAIN`。分配后 owner 停止或已注销也会撤销分配。注册/stopping 检查只是快照，不是绑定租约；调用者另持 device 引用并协调 mask、请求提交和退出。

create 返回一份引用，`driver_dma_buffer_get/put()` 显式交接所有权。对象保留 device 元数据引用，独立于 devres/绑定；unbind/unregister 不自动释放 buffer。最后 put 用记录的 PFN/order 回收，并交还设备引用。get 可用于 IRQ，最终 put 限任务上下文；都须已有有效引用，不能从悬空指针复活对象。put 不停 DMA、不排空请求、不撤销映射，驱动必须保留引用直到相应停止/失效证明完成，也须自己保留回调/module 代码。

`driver_dma_buffer_data/address/size()` 在持引用期间返回 CPU alias、CPU 物理地址及请求字节数；size 不暴露向上取整的分配尺寸。地址 0 也可能有效，不能用它判定对象不存在。物理地址只有在显式 identity-DMA 平台上可直接作为总线地址；该对象不创建 IOVA、隔离、MMIO claim、BO 或 GPUVM；需要下述显式 buffer 映射入口才能取得 domain 的设备地址。CPU/设备可见性仍须经过既有 DMA sync/barrier。

分配不取得 lifecycle mutex，可从 active class 调用使用，避免与持该锁排空 class 的退出互相等待。它只构造存储所有者，不批准硬件提交。这些函数均导出给模块，这些 buffer 函数不改 device/driver/HCD 布局；当前全局模块 ABI 为 13。

### 将 buffer 显式映射到 domain

`driver_dma_map_buffer(domain, buffer, direction, &mapping)` 把请求大小向上取整到完整页，复用 domain provider、权限预检、设备地址校验、同步和可失败回滚；每个 mapping 只保留一份整块 buffer 引用，不对高阶尾页调用 frame_get/put，也不拆分原始分配。33 字节、64 KiB 对齐的对象只映射一页，PAGE_SIZE+33 字节映射两页，不暴露其余用于强对齐的 backing。最后一页的清零 padding 也属于设备可访问范围，policy 必须允许这一整页；不可用只覆盖请求字节的检查绕过 padding 限制。

调用者在 map 调用期间持有 buffer 和 domain；成功后可交还原 buffer 引用，mapping 继续保留原分配及 allocation owner 元数据，直到全部失效确认成功。allocation owner 与 domain device 可以不同：存储引用不授予设备权限，准入、mask、policy 由目标 domain 决定。每页 IOVA 可能不连续，必须用 `driver_dma_page_address()` 逐页读取，不能从 buffer PA 或首个 IOVA 推算其余地址。buffer 与 order-0 数组共享 `mapping_count/sync_for_device/sync_for_cpu/try_unmap_pages`；count 是保留的页条目数，失败清理期间可能只有已发布的前缀，但 buffer 的整个高阶分配仍然保留。

map 返回非零也可能交出非空清理句柄，必须继续保留并重试；unmap 开始后禁用地址/同步，部分失效成功不会提前释放 buffer 或执行 CPU invalidate。全部成功后才执行必要的 CPU 同步、交还 buffer/domain 引用并置空句柄。managed domain 的 map_buffer 与原有 map_pages/unmap/shutdown 使用同一 mutex；最后 put 和 provider 操作均限任务上下文。不替驱动停止 DMA，也不协调同一 buffer 在其他 mapping 或 CPU 中的并发访问。

RISC-V native provider 已按此契约借用通用 mapping 持有的存储，私有 IOVA 的 pinned 条目为 PFN_NONE；不再二次 pin 高阶尾页。用户态正 PID API 仍保留独立 order-0 frame 引用以覆盖失败 quarantine；旧 map/unmap 拒绝负 native owner，禁止绕过通用 mapping 的计数和所有权。硬件 PTE、IOVA 和失效/关闭重试协议保持独立；这不提供 GPU 页表、BO 驻留或自动选择 provider。

### 物理 VMA 的可选资源引用

`mm_mmap_physical_owned(..., owner)` 借用调用方持有的 mm_mapping_owner；只在所有 fallible 操作完成后，为已发布 VMA 取得独立引用。普通物理入口仍为借用。VM_PHYSICAL_OWNER 与 VM_FILE 互斥并共用指针槽，布局不增长；复制/拆分持有，最终引用在 TLB 和 deferred VMA 退休后归还。owner 的 release/context 必须始终有效，最终 put 在任务上下文且不持 MM/spinlock；它只保留资源，不授予映射权限或证明 DMA 停止。

`84d122b95` 的两项定向检查和实际头文件语法编译通过。GPU 绑定 owner、prepare 忙检查和 fbdev 消费者现由 `e9933597f` 接入，见下节。

### 物理 framebuffer 的 binding owner

借用物理 get_fb provider 在 probe、硬件使用之前调用 `gpu_device_physical_mapping_init(dev)`，选择受管失败清理，登记 per-binding owner、组合协议和 prepare；remove 必须容忍部分初始化。provider 的普通 driver/class 布局保持不变。simplefb、lsdc-boot、vmsvga 已接入；VMSVGA 的静态槽位不能覆盖仍被保留的 owner。

`gpu_device_physical_mapping_get(access, &owner)` 取得一份资源引用，调用方借给 `mm_mmap_physical_owned` 并归还捕获引用，成功 VMA 另持独立引用。退出 prepare 关闭新准入；任何视图仍存续则 EBUSY，保留绑定/资源供显式重试，避免持长期 active class call 阻塞 drain。最后 VMA 经 TLB/deferred 退休后归还，之后重试退出才能释放；finalizer 是内核代码，不回调已卸载模块。此引用不授予 MMIO/DMA 权限或证明实体硬件停止。

fbdev mmap 与 FBIO_MAP_FB 使用同一契约。缺失 owner 协议的物理 provider 返回 EOPNOTSUPP，存在但关闭的 owner 拒绝新视图；owning RAM get_scanout 保持原路径。三个定向检查及实际头文件语法编译通过，公开布局/模块 ABI 13 不变；实体停止和发布配置验证另计。

### 显示 backing 的现有完成边界

外部 GEM backing 已由 VMO、GEM pin、dma-buf 和 VMA 引用独立保留。attach/unref 失败的对象进入 retained，offline 不授权释放；旧 publication 的成功停止凭据才使 reaper 在不调用旧驱动的条件下退休硬件引用，CPU owner 仍可继续持有。两项既有失败 attach/停止与 dma-buf 跨停止定向检查于 2026-11-16 通过。此软件范围已完成，实体 provider 停止证据另计。

旧 get_fb 物理映射也已通过以下 binding owner 接入 VMA 的独立资源持有；GEM、owning RAM 和物理窗口按各自存储契约退休。

### Hub 的提前关闭与失败保留

hub probe 在端口/端点初始化前登记 remove prepare，先于 consumer-first 解绑设置 stopping；核心 PREPARING 状态使扫描和枚举拒绝新使用。若下游退出失败，hub 不再刷新/重装状态 URB，同时保留 HCD、协议图、上游 URB 和 callback。prepare 只关闭准入；下游 slot 停止与上游 endpoint 交还确认后，remove 才释放相应存储，失败保留同一 owner 供重试。

当前 HCD ops 只有 xHCI 与 hub 转发层；`ef5d9294b` 的生产 hub/core/USB 集成与既有端点定向检查通过，不扩展为实体设备认证。新增 prepare 不改公开结构及 ABI 13。

### xHCI 同步 bulk 暂存

xHCI probe 在启动自己硬件前预分配一份 64 KiB、页对齐的 DMA buffer，每控制器一份，HID-only 控制器也承担这项固定占用。`xfer_busy` 串行保护整个 copy/submit/wait；OUT 先拷入，IN 先清零暂存区并同步给设备。Normal TRB 只引用该分配的物理范围，不引用 URB 或调用者存储，调用者内存无需满足设备地址窗口及 64 KiB 边界。copy 后重新检查 fatal、slot 停止和 endpoint closing，拒绝期间发生的退出。

bulk 不运行 complete 回调：0 表示完整长度完成；短包返回 `-EMSGSIZE`，URB actual_length 报告实际字节数，IN 只拷回该前缀，尾部保持原值；len 保持请求长度。STALL 为 `-EPIPE`；超过 64 KiB 返回 `-EMSGSIZE`，残余计数超过请求返回 `-EIO`，这些错误不交付数据。actual_length 属于模块 ABI 13，旧模块须重建。保持 64 KiB 上限；跨 64 KiB 物理边界时用最多两个 Normal TRB，CH/TD Size/Link chain 连成同一 TD，首段 cycle 延后发布。首段短包或错误也终止该 TD，按完成段 residue 累计实际长度，避免等待错误的尾段。提交 `532cedf75` 的定向检查覆盖环绕、早/晚短包、STALL 和失败停止时保留全份存储。调用方仍按请求上限分块。边界及 Transfer Event residue 依据 [Intel xHCI 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf) 的 6.4.1、6.4.2.1。

超时后即使 HALT 未被证明，硬件仍只能访问 controller 持有的暂存区；fatal gate 阻止复用。controller prepare 失败继续保留这份引用，remove 只有确认 controller 停止且 USB 图清理成功后才 put；外部额外引用仍能继续保留存储。临时 URB 和调用者缓冲区在任意返回后都可结束自己的生命周期。storage 的实例保留和 BOT 恢复见前文；软件 fatal 标志不作为 DMA 停止证明。

## 原生 DMA domain 与页映射

头文件：`kernel/include/drivers/core/dma_mapping.h`；实现：`kernel/drivers/core/dma_mapping.c`。

- `driver_dma_domain_create(dev, ops, context, &domain)`：建立显式地址转换 domain，复制 provider ops，保留 device 引用和 DMA mask 快照。成功接管 context 的一份所有权；失败不调用 provider release。
- `driver_dma_domain_create_checked()`：接收独立的 `driver_dma_checked_ops`，允许解除映射返回错误，保留原有 provider 结构与模块 ABI。
- `driver_dma_domain_create_managed()`：接收 `driver_dma_managed_ops`，把未完成的当前页清理及可失败的硬件 detach 纳入生命周期。
- `driver_dma_domain_create_managed_policy()`：在相同生命周期中独立保留不可变物理约束，整批预检权限，并核对 provider 返回的 Unity/IOVA 要求；详见下文。
- `driver_dma_domain_create_direct()`：仅在总线/平台已确认采用恒等 DMA 时显式使用。它不提供隔离，不能作为 IOMMU 失败时的降级，也不探测 AMD-Vi。
- `driver_dma_map_pages()`：映射调用者已持有的 order-0 RAM 页数组。页面可以物理不连续；每页获得独立的设备 DMA 地址。
- `driver_dma_map_buffer()`：显式保留并映射原生 buffer 的页覆盖范围；与页数组使用相同后续接口，整块分配不拆分，详见 [buffer 映射](#将-buffer-显式映射到-domain)。
- `driver_dma_map_vmo()`：按 first/count 页索引导入已物化的原生 VMO RAM，先取得稳定页引用，再交给页映射；不建立活的 VMO 大小/驻留租约，详见 [VMO 页快照](#从原生-vmo-保留页并映射)。
- `driver_dma_map_owned_vmo()`：显式保留固定 RAM VMO 的整个 backing/预算 owner，设备仅映射选定页视图；成功和失败清理句柄使用同一所有权，见 [VMO owner](#持有-vmo-的-dma-视图与锁外-owner-退休)。
- `driver_dma_page_address()`：读取设备地址，不根据 PFN 或 CPU 地址推算。地址 0 可以合法，错误由返回值区分。
- `driver_dma_mapping_sync_for_device/cpu()`：在 CPU 与设备停止访问的边界执行缓存同步。映射时自动执行初次 device 同步，成功映射解除时执行必要的 CPU 同步。
- `driver_dma_try_unmap_pages(&mapping)`：硬件停止访问后，逆序解除翻译；全部成功才同步 CPU、释放页面和 domain 引用并置空指针。失败返回负 errno 并保留 mapping，必须重试。
- `driver_dma_unmap_pages(&mapping)`：原有 infallible provider 的兼容入口；内部也保留失败对象，但不返回状态，checked provider 应使用 try 接口。

页数组映射独立保留每一页的 frame 引用；buffer 映射独立保留整个分配对象；owned VMO 映射另保留整个固定 VMO 与其 charge。调用者可在映射成功后释放原始页面引用，设备仍使用的 RAM 不会因此回收。页映射失败会尝试回滚之前的翻译及页面引用；checked 回滚如果失败，`driver_dma_map_pages()` 返回原始映射错误，同时通过非空 `*out` 交出仅供清理的句柄。调用者必须保留并反复 try_unmap，不能只判断返回码就丢弃输出。回滚没有完成初次 device 同步的 mapping 时不执行 CPU invalidate，避免丢失尚未交给设备的 CPU 数据。

provider 的 `map_page` 每次映射一整页，失败不得留下该页翻译或未确认的失效。旧 `unmap_page` 返回前必须完成设备地址翻译失效；checked `unmap_page` 只有返回 0 才确认失效，失败时必须保留 IOVA，且允许重试已经清除 PTE 的操作。设备地址需要页对齐且整页落在 domain 的 mask 内，违反条件时解除该页及之前映射，返回错误，绝不回退到物理地址。

解除映射一旦开始，mapping 即进入清理状态，地址查询和 cache sync 返回 `-ESHUTDOWN`，不能恢复提交；count 仍表示保留的页条目数量。失败时全部 frame 引用及 buffer/VMO owner 保留，包括已经解除翻译的页，CPU 同步也推迟至全部成功；重试从失败页继续，不重复解除之前已成功的页。这里假定 provider 返回成功后不会再次失败地撤销该成功结果。正数错误被规范为 `-EIO`。

managed provider 的 `map_page` 显式返回 `needs_unmap`：只要当前页已安装翻译或保留 IOVA，就设置 true，并交出可供 unmap 的地址，包含返回负 errno 的情况。核心将当前页也纳入回滚；失效再次失败时，清理句柄保留当前页及前序页面引用。返回成功却没有设置 needs_unmap 是 provider 契约错误，转为 `-EIO`；失败且 false 仍要求当前页没有硬件残留。旧 infallible/checked provider 的接口及回滚语义保持原样。

managed domain 的 `driver_dma_domain_shutdown()` 在任务上下文永久关闭新 map。有 mapping 或清理句柄时返回 `-EBUSY`；驱动先停止 DMA、解除这些 mapping，再重试 shutdown。没有剩余 mapping 后调用可失败的 `deactivate`，成功才解除内部 provider 引用。失败保留完整 context 供重试；正数错误转为 `-EIO`。shutdown 成功可重复调用，不消费调用者引用，最后再 put。遗漏 shutdown 而只 put owner 会保留内部引用，产生安全但需要恢复的资源泄漏，不能据此卸载 provider 模块或释放其硬件资源。

同一个 managed domain 的 map、unmap 和 shutdown 回调由每 domain mutex 串行化，回调可睡眠，但不得递归调用同一 domain 的这些接口，也不能在持有回调需要的锁时调用。shutdown 关闭提交后，已有 mapping 的清理仍可进行；没有强制释放或后台自动 detach。不同 domain 之间没有这把锁，旧 provider 保持独立映射并发契约。其他类的 domain 调用 shutdown 返回 `-EOPNOTSUPP`。

probe/解除绑定的错误路径同样要保留清理句柄、provider context、硬件资源和模块代码；不能返回失败后让 devres 释放失效命令仍需访问的资源。旧 provider 的 `release` 仍是不可失败的软件 context 销毁；managed provider 将可失败 detach 放入 `deactivate`，`release` 只在确认完成后销毁软件 context。本接口没有自动隔离区或后台恢复 worker，已提供显式的 RISC-V PCI managed provider，将失败页清理与硬件 detach 交给通用生命周期，而非包装成旧 map callback。AMD-Vi 后端也尚未提供。

provider context、ops 所在模块及硬件资源的生命周期由 provider/驱动协调，直到最后一个 domain 引用释放。domain 引用只保持 device 对象和 context 的软件生命周期，不保持驱动绑定或硬件在线。创建/映射/提交必须与设备撤销串行化，解除绑定前停止 DMA 并销毁 mapping。

RISC-V IOMMU 命令队列的 tail 在写入 CQT 发布命令之前推进，表示槽位已经交给硬件；等待超时不会撤销所有权或让下一次提交覆盖该槽位。后续提交仍依靠 CQH 的队列满检查，CQCSR 有错误时不再发布；恢复 CQ 错误或超时后的完整设备重置仍由后续工作完成。解除映射入口在构造位图和缩小索引之前检查 64 位 IOVA 范围和页数，防止越界、移位未定义行为或高位截断造成槽位别名。这些队列修正本身不代表硬件恢复或原生 provider 接入已完成。

用户态 DMA heap 的连续块在交给 VMO 前通过 `pfa_split_allocated(head, order)` 转成每页独立的 order-0 所有权。这个操作只允许独占的、元数据一致的分配，不增加可用页计数；失败不修改元数据。请求大小不足整个块时，剩余尾页立即 frame_put。VMO 的页分配回滚、缩小和销毁也改为释放自身引用，不能用直接 free 覆盖 IOMMU 或其他持有者的 pin。转换后不得再按原始高阶块整体释放。

RISC-V 用户态 domain 映射还独立保留每个 RAM 页的 frame 引用。map 只接受调用者持有引用的已分配 order-0 RAM，先验证整个物理区间、pin 页面并保留 IOVA，再发布 PTE。失效命令失败时清除 PTE、保留 IOVA/页引用并将 domain 标为 blocked，拒绝后续 map；即使上层因返回错误而销毁 VMO，这些页也不会提前回收。unmap 失败仍保留引用，只有成功失效或安全 block/release 后才能释放；`mapped_pages` 统计包含隔离保留的页面。软件 blocked 不表示硬件 DMA 已停止。

设备 context 安装失败同样先尝试安全 block，无法确认时保留三层页表及 owner。原 owner 再次 claim 会先重试释放失败 claim 的页表，成功后才建立新 claim；已经成功 claim 后进入 blocked 状态的 domain 必须显式 release。release 调用者先停止 DMA/bus mastering；失败仍保留全部资源。当前没有后台回收器，硬件持续故障时这些有界资源可能一直保留，不能以回收内存为由绕过确认。

`riscv_iommu_dma_domain_create(device, &domain)` 接收已发布的 PCI device，从实际 BDF 计算 requester ID，创建 managed domain。构造和 shutdown 时要求 bus mastering 已关闭；调用者还必须停止 DMA，并与驱动绑定/用户所有权变更串行化。创建不启用 bus mastering，驱动确认初始化成功后自行启用。user_owned 设备、已有 requester claim 和不受当前 DDT 覆盖的 requester 均拒绝，不回退物理地址。原生 owner 使用单调负 token，与正 PID 的用户态 owner 隔离；token 用尽返回错误，不循环重用。device 对象由 managed domain 保留。构造在 discovery 事务中核对当次发布身份；旧引用不会自动指向同 BDF 的新设备。

创建失败通常置空输出，但 context 安装后失效失败且无法安全清理时，输出非空的已关闭 domain：调用者须保留，重试 shutdown 后再 put。map 通过 needs_unmap 交接当前失败页地址；unmap 和 deactivate 复用实际 IOMMU 失效路径，失败返回 `-EIO` 并保留引用。构造/关闭时 BME 未清除返回 `-EBUSY`。后端仍限 8 个 domain、每 domain 64 页和一层 DDT；没有 AMD-Vi、多 segment、ATS、页大小协商或原生故障 IRQ 派发；已提供下述 per-device 轮询和后台 watch。TO_DEVICE 页使用只读 PTE；FROM_DEVICE/BIDIRECTIONAL 使用读写 PTE（SV39 不允许只写叶 PTE）。cache sync 仍由通用 mapping 层执行。

原生与正 PID 用户 domain 现在共同使用 `pci_requester_reserve()` 的登记：发布 DC/页表前占用当前 PCI function 的显式 requester，任一其他登记冲突都拒绝，原生接口报告 `-EBUSY`。它只占用一个已知 RID，不推断 alias、ACS 或物理隔离组。健康的用户 claim 只有原设备身份仍相同时才幂等，重枚举不刷新旧 claim；设备移除/替换不自动释放原占用。

用户 `riscv_iommu_domain_claim()`/`release()` 保持正 PID、0/-1 接口，在任务上下文与 discovery 串行化。新用户 claim 先取得共享占用，再关闭 BME 并验证读回，失败回收临时占用且不发布 DC；拒绝重叠占用不会修改 PCI 配置或 IOMMU context，健康幂等调用不改变 BME。`udriver` 的首次 claim 将硬件接管和随后 BME enable 放在同一外层事务中，release 将 BME disable/context detach 放在同一事务中。BME clear 不排空 engine/in-flight DMA，停止与同一 owner 的调用串行化仍由调用者负责。

页表安装/失效失败时，占用和设备引用随页表保留；用户原 owner 可在身份未替换时重试失败 claim 的安全清理，成功 release 才释放登记。原生 shutdown 在确认 detach 后将占用转交 managed context，直到最后一次 `driver_dma_domain_put()` 才释放；shutdown 成功而旧 domain 引用尚在时，新重叠 claim 仍被拒绝。这个额外保留期使硬件 detach 回调无需在 mapper mutex 下等待 discovery，最终释放在映射锁之外执行，避免与解绑路径反向等待。负 token 的原生 domain 只能通过 managed handle 关闭，不能用用户 release 绕过生命周期。配置写入和未参与登记的其他 provider 仍不被这套占用强制拦截。

`riscv_iommu_dma_fault_poll(device, &snapshot)` 是任务上下文的轮询接口。它验证该 device 对象确实持有当前 native requester，拒绝同 BDF 的其他软件对象，返回累计 `count`、最后 `cause/address`、`blocked`、`isolated`、`isolation_error` 和 `queue_error`。轮询会排空共享 FQ 并处理所有 owner，不只处理调用者。故障记录归属和 BME 清除/context 失效在同一 IOMMU domain 锁下串行完成，避免在旧事件处理前 release/reclaim requester。隔离命令失败时，后续轮询即使没有新记录也会重试；已成功的动作不重复执行。

`blocked` 表示软件拒绝后续 map；`isolated` 只表示 BME-disable 与 context-invalidation 命令曾完成，不证明 DMA engine/in-flight 请求已经停止，也不允许自动释放页面。调用者仍需停止/复位设备、处理未完成请求和 fence，再 unmap/shutdown。查询成功返回 0 也可能带 `isolation_error=-EIO`；没有本对象的 live native domain 或 IOMMU 已离线时返回 `-ENODEV` 并清空输出。故障 latch 不通过读取清除，恢复需销毁并重建 domain。

FQCSR 的 memory-fault/overflow 位会累计到 `queue_error`：记录可能缺失，所有活跃 domain 均进入 blocked 并尝试隔离，新 claim 拒绝，`count` 仅代表已知记录的下界。接口不清除硬件队列错误或虚构缺失地址，队列恢复/重初始化仍待开发。基础查询是同步轮询；可使用下述 watch 建立后台轮询。没有 IRQ 派发或自动 signal fence，驱动仍需检查所有字段并定义请求完成策略。

`riscv_iommu_dma_watch_start(device, period, callback, context, &watch)` 在调度器启动后建立独立 worker，保留 device 引用并固定当前 native owner token。period 是两次轮询之间的非零 native ticks，包含轮询/回调执行耗时后再计时，不是硬实时 IRQ 延迟保证。健康且未变化的快照不通知；count、地址、cause、queue/isolation 错误或状态变化才调用 callback。terminal poll 错误只通知一次并停止重排；旧 owner 被替换后返回 `-ENODEV`，不会跟随或隔离新 owner。回调拿到的快照只在调用期间有效，需要保留时复制。

callback 在该 worker 上且不持有 IOMMU/queue 锁，可 signal 原生 fence，但框架不替驱动选择请求错误、停止引擎或 reset。启动自检的 fence 只表示通知路径成功，不表示 DMA 请求成功；真实驱动须先确认引擎停止，再按任务状态结束请求。callback 可能在 start 返回前运行，context 必须预先初始化，模块/代码/context 必须保留到 stop 成功。不得在回调中等待当前绑定锁或直接发起需要该锁的绑定操作；设备生命周期回调仍可能持有该锁。

`riscv_iommu_dma_watch_stop(&watch, deadline)` 永久关闭重排并等待在途回调返回，成功释放对象、device 引用并置空指针。deadline 是绝对 ticks，0 表示无截止；超时或中断保留整个对象和 context，应在同一 owner 上串行重试。回调内自停止返回 `-EDEADLK` 且不释放。start 的部分失败也可能交出非空清理句柄，须 stop 后再丢弃。监测器不固定硬件 domain 或驱动绑定，应在 quiesce 中先 stop，再 shutdown domain/释放设备上下文。目前采用每 watch 一个 worker，没有 IRQ 触发或共享线程池。

显式 `driver.runtime_test=1 driver.iommu_native_test=1` 自检需要额外 QEMU EDU fixture，普通启动不运行。测试从已枚举、未绑定的 EDU 找到真实 device，建立原生 domain、映射两个 RAM 页，通过 IOMMU TR_REQ 验证设备地址翻译和只读权限，再检查 unmap 与 detach 后翻译故障。该路径使用 [QEMU 的寄存器定义](https://raw.githubusercontent.com/qemu/qemu/master/hw/riscv/riscv-iommu-bits.h)中 TR_REQ 的 DID/GO/NW 字段；高半部 DID 在低半部 GO 前写入，并有 busy 等待上限。启用 legacy PPN 编码兼容时，TR_REQ 响应校验只能比较旧编码保留的地址字段。EDU 先对 64 KiB 对齐、PAGE_SIZE+33 字节的高阶 buffer 做跨页往返：map 后先交还 allocator caller 引用，验证模式和未传输字节，再确认停止并 unmap，要求 `DRIVER_IOMMU_BUFFER: PASS high_order=1 iova=1 dma=1 retained=1`。实例 manifest 显式选择 embedded，使构建与启动使用同一驱动配置。随后还将原生 VMO 的页 1–2 映射到相邻 IOVA，先销毁原 VMO，再由 EDU 跨页读入/写回并核对模式与未传输区域，要求 `DRIVER_IOMMU_VMO: PASS snapshot=1 offset=1 dma=1 lifetime=1`；确认 DMA/BME 停止后才解除映射。后续往返测试将两个物理不连续 RAM 页映射成相邻 IOVA，以 128 字节传输跨越页边界：先读入 EDU 内部缓冲，再清零 RAM 传输区并写回，逐字节验证模式以及两页非传输区域和额外 guard 页。测试传给 EDU 的是通用 mapping 返回的 IOVA，原始 RAM owner 引用在 DMA 前已经释放，页面由 mapping/backend 持有。两次传输之间依次执行 for-CPU 与 for-device 同步，确认 DMA_RUN 清除后才读回、关闭 bus mastering 和 unmap。等待同时有时钟期限和迭代上限；超时 panic 保留活跃资源。之后在持有独立 guard 引用的原 RAM 页上保留哨兵，unmap 后让 EDU 再尝试写原 IOVA；检查 FQ 新记录，由后台 watch 完成隔离并通过回调 signal fence；主线程等待 fence、stop 排空回调后，再检查 store page fault、BME 清除、隔离状态和新 map 拒绝，并验证哨兵未变。保留 guard 引用使失效回归也不会写坏已重新分配的内核 RAM。该数据路径补充了旧 TR_REQ 编码的校验缺口，但仍不代表实体硬件恢复或端到端用户态 DMA 已验证。

同一 `smoke-driver-iommu-native-riscv64` 实例还验证通用占用与原生/用户 domain 的双向冲突、拒绝时 DC 不变、shutdown 后最后 put 前继续占用，以及用户正常幂等/release 后重试，新增 `DRIVER_IOMMU_REQUESTER: PASS native=1 user=1 conflict=1 release=1` 标记；普通启动不运行。失败保留与锁顺序另由主机后端验证。

限制：仅支持完整的、已分配的 order-0 RAM 页。不是任意物理地址映射接口，不接收 GOP/VRAM、未物化 VMO 页或高阶分配的尾页。没有部分页、segment 合并、bounce、通用 IOMMU 硬件后端或缓存一致性新后端。缓存维护复用已有架构 hook，架构声明未就绪时拒绝创建/映射。旧 `dma_alloc_coherent()` 不自动加入 managed 页映射；用户态映射继续使用既有接口，共享 requester 接管与失败保留按上述协议执行。

### 从原生 VMO 保留页并映射

`vmo_peek_page()` 返回借用 PFN，不能在解锁后用它推导稍后 frame_get 仍安全。新增 MM 接口 `vmo_pin_pages(vmo, first, count, pages)` 在调用者持 VMO 引用的任务上下文，使用同一 VMO lock 排除 resize/publication，先验证整个范围及 canonical PFN，再发布输出并通过 PFA 批量保留各帧。Rust `vmo_lifecycle.rs` 拥有这项事务，C 只提供锁/字段访问和 batch pin 原语。错误不改输出数组、不取得部分 pin。

支持 ANONYMOUS、PAGED、稀疏 SHARED_ANON 已物化 RAM，以及工厂发布的 CONTIGUOUS canonical RAM；缺页返回 `-EAGAIN`，不分配页、不调用 pager，不用缺页异常隐式完成导入。物理/未知 VMO 返回 `-EOPNOTSUPP`，参数/范围/无效 PFN 返回 `-EINVAL`。单次上限 `VMO_PIN_MAX_PAGES=4096`，超过返回 `-E2BIG`；上限由 C 头文件定义，Rust 经窄查询取得同一值，用于约束 IRQ-off 遍历。更大对象分范围导入，调用者须协调跨批次 resize，不保证多个批次是同一快照。成功写出的每页都有独立 owning reference，最后由 frame_put 交还；不能把高阶尾页、VRAM 或任意物理 VMO 当作这类 canonical 页。

驱动入口 `driver_dma_map_vmo(domain, vmo, first, count, direction, &mapping)` 定义在 `dma_mapping.h`，由 `dma_vmo.c` 实现并导出给模块。调用者在整个调用期间持有 VMO/domain 引用；first/count 是页索引/页数，末个逻辑页的 padding 也会被映射。adapter 在 VMO lock 外分配临时 PFN 数组，取得上述稳定页快照，随后调用原有 map_pages，最后在所有返回路径归还临时 pin/数组。policy 检查完整页且先于 provider 调用；它发生在 MM 快照之后，拒绝时临时 pin 也会撤销，不留下翻译。分配失败返回 `-ENOMEM`，其他 mapper 错误原样传播；错误清空输出，除既有可失败回滚交出的非空清理句柄。

mapping 独立持有所需 frame 引用，调用者在成功返回后可释放 VMO；对可变源也可缩小 VMO，而设备仍引用取得快照时的原页面。新的 CPU fault/重新增长可能选用不同页面，不能把这解释为自动跟踪源地址。若映射失败只发布了一个前缀，清理句柄只保留那些可能被硬件访问的帧，未发布后缀的临时 pin 正常交还。失效失败仍按既有协议保留 mapping，只有全部确认成功后才进行必要 CPU sync 和最后释放。

所有 provider/cache 调用均不持 VMO lock；锁顺序只在快照期间是 VMO → PFA，释放 VMO lock 后才取得 managed domain mutex。该接口只保证帧存活，不冻结 VMO extent，不转移 cgroup charge，也不授予源对象访问权或硬件 DMA 权限。GPU BO/request 层仍须管理访问权、驻留/预算、CPU/device 排他和提交/完成；VMO resize/销毁释放其原有预算不代表 DMA pin 已经消失。这里不增加用户态 UAPI、Linux GEM 结构依赖或 GPUVM。

### 持有 VMO 的 DMA 视图与锁外 owner 退休

`driver_dma_map_owned_vmo(domain, memory, first, count, direction, &mapping)` 是显式的存储所有权入口，仅接受工厂发布的 CONTIGUOUS（kind 4）和 RESIDENT（kind 5）固定 RAM；其他类型返回 `-EOPNOTSUPP`。调用者在整个任务上下文调用期间持有 VMO/domain 引用。它复用 map_vmo 的 canonical 页快照、4096 页上限、mask/policy 检查、cache sync 和逐页设备地址接口，不另建页注册表、用户 ABI 或 GPU 对象模型。

adapter 在任何 provider 调用前取得一份 VMO 引用，用局部输出接收映射，在发布成功句柄或失败清理句柄前无分配地交接该引用。整个 VMO 的固定 extent、全部页面及原始 cgroup charge/pin（如有）都随句柄保留，包括 first/count 之外未映射的页；硬件只获得请求视图覆盖的页。多个视图分别持有 VMO 引用，原始 charge 仍只计一次，不转移给导入者或设备。kind 4 的内核分配本身没有该 cgroup charge。设备地址可能不连续，必须逐页查询。

参数/元数据分配/policy 检查拒绝，或映射失败但已完整回滚时，输出为空并交还额外 owner；若回滚无法确认，则保留原始 errno 并返回非空清理句柄。部分 map、非法 IOVA 及部分 unmap 的失败都保留整个 VMO 和 charge，直到重试确认所有翻译失效，不能因一部分页已解除而提前交还 owner。清理句柄不允许地址查询、sync 或继续提交。可变 ANONYMOUS/PAGED/SHARED_ANON 和物理 VMO 不使用这一入口，也不能在拒绝后静默降级为页快照映射。

`driver_dma_map_vmo` 继续只保留快照帧，不保留源 VMO/charge；需要整个固定对象所有权时应显式选择 owned 入口。固定 extent 来自 MM 工厂，不是 mapper 新增的驻留策略。两种接口都不授予对象访问权、不提供 GPUVM，也不证明 DMA 停止。调用者仍须停止 producer 或观察真正的硬件完成，协调 CPU/device 排他，再 unmap；fence 元数据完成、设备 offline 或最终 caller put 不能替代这些条件。

普通 unmap 和构造回滚共用退休协议：所有可失败失效与必要 CPU sync 成功后，移除 mapping、更新 domain mapping 计数；try_unmap 同时清空调用者指针。buffer/VMO 的最终 put 在退出 domain mutex 后执行，try_unmap 的临时 domain 引用跨越全部 owner 析构回调，随后才交还；构造回滚由调用者贯穿整个调用的 domain 引用保护。失败时不移动 owner。原来的裸页 frame_put 仍在已确认失效之后按页路径执行。这一规则也覆盖 allocation owner 与 mapping device 不同的 buffer：最后 put 可能触发另一设备的 release 并重新进入 domain shutdown。provider 的 map/unmap/deactivate 回调仍不得重入本 domain；锁外 owner 退休是另一个阶段，不放宽 provider 的约束。

新增函数已导出给 `.a20drv`；内部 owner 交接 helper 不导出。公开结构与模块 ABI 12 不变。

### 不可变物理约束与映射入口检查

`driver_dma_policy_create(rules, count, &policy)` 在任务上下文复制并按起点排序至多 1024 个 `driver_dma_rule`，不借用固件、设备或输入数组。规则使用 inclusive **CPU 物理字节区间**，不是 compact PFN、CPU 虚拟地址或 GPUVA；末端可以是 0xffffffffffffffff。`access` 的 READ/WRITE 从设备角度定义，0 表示禁止任何 DMA；`flags` 的 IDENTITY 要求设备 DMA 地址（IOVA）等于物理地址。重叠规则取权限交集、合并 identity 要求；无规则的区间和空 policy 没有额外限制。约束不授予内存所有权、占用或 DMA 启用权限。

`driver_dma_policy_check(policy, physical, length, direction, &identity)` 检查整个字节 span：TO_DEVICE 要求设备 READ，FROM_DEVICE 要求 WRITE，双向要求两者。任何相交部分不满足即 `-EACCES`；任一相交规则要求 identity，则整个被查询 span 要求 identity。长度为 0、溢出、方向/参数非法返回 `-EINVAL`，所有错误清空 identity。create 的未知 access/flags 或反向区间返回 `-EINVAL`，过多输入返回 `-E2BIG`，分配失败 `-ENOMEM`，失败清空输出。输入在 create 返回前保持不变；返回后可释放或改写原数组。`get/put` 保留/释放独立引用，最终 put 在任务上下文执行；对象没有可变规则更新接口。

`driver_dma_domain_create_managed_policy(dev, ops, context, policy, &domain)` 成功后接管 context 的一份所有权，并独立保留 policy 引用；失败不消费二者。调用者可立即 put 原 policy 引用。domain 在 provider `release(context)` 返回之后才 put 自己的 policy，因此 provider 清理阶段仍可查询它；失败的映射清理或 shutdown 继续保留 domain 时也保留 policy。原有 ops 结构、构造器和模块 ABI 不变；旧 domain 不自动获得约束，需要约束的 provider 应显式选择新入口。

`driver_dma_map_pages()` 在 pin 任何 frame、分配 mapping 或调用 provider 前，先对全部输入页执行物理地址转换与整页权限检查。一个受限字节也约束整个 DMA 页；后面的页被拒绝不会使前面的页先进入 provider。成功返回的 IOVA 继续检查页对齐、完整 mask 和 identity；违反地址要求返回 `-ERANGE` 并进入既有可失败回滚。失效失败仍交出不可提交的清理句柄，保留所有页、domain 和 policy，try_unmap/shutdown 的关闭、重试与 CPU 同步时序不变。

provider 必须在发布翻译前落实方向权限和 identity 地址分配，可在 context 中查询同一 policy；核心检查不读取硬件 PTE，也不能撤销回调返回前已经发生的错误 DMA。对象不自动建立 Unity 映射、Unity 存储保留、排除旁路或 IOMMU 页表。它仅提供可执行的映射入口约束，不替代 provider 的 requester、硬件状态与停止协议。

## AMD-Vi 的 IVRS 发现与路由候选

`firmware/ivrs.h` 与 `kernel/firmware/acpi/ivrs.c` 提供分配无关的 IVRS 读取接口。格式依据 [AMD IOMMU Specification 3.10 第 5 章（AMD 原文镜像）](https://kib.kiev.ua/x86docs/AMD/IOMMU/48882-3.10.pdf)，并对照 [ACPICA 表定义](https://raw.githubusercontent.com/acpica/acpica/master/source/include/actbl2.h)；实现使用字节读取，没有导入 Linux/ACPICA 驱动代码。

`acpi_ivrs_begin()` 在暴露首个条目前校验整张不可变表，包括签名、长度、校验和、revision 1/2、block/device 边界、已知范围配对和内存区间溢出。支持 10h、11h、40h IVHD，以及 20h–22h IVMD；HID 设备条目的可变 UID 长度单独处理，不按固定长度跳过。未知但有界的 block/fixed entry 保留原始类型，无法确定长度的 variable entry 返回 `ACPI_IVRS_UNSUPPORTED`。校验失败清空 iterator，不把前面合法的条目交给调用者。

iterator 和 typed hardware/memory/device accessor 返回借用的表视图，使用结束后才可释放 ACPI mapping；底层读取器保留 alias、extended、HID 的原始字节和 IVMD 顺序。不同 IVHD 格式可以描述同一单元，记录数不能作为唯一硬件单元数。

`kernel/firmware/acpi/ivrs_policy.c` 在全表校验后提供只读路由与集合查询：

- `acpi_ivrs_route(table, segment, requester, out)`：返回硬件描述、实际 DMA requester、DTE settings、extended settings、alias/non-PCI 标记及相关 IVMD 数量。同一 `(segment, IOMMU DeviceID, capability offset)` 采用 A20OS 的 `40h > 11h > 10h` 选择顺序；这是一项显式软件策略，不表示规范要求所有平台使用相同顺序。同一格式重复、base/info 矛盾、不同单元的 16-KiB MMIO 窗口重叠或查询命中多个单元返回 `ACPI_IVRS_AMBIGUOUS`。ALL 是单元内的默认属性，具体 select/range 覆盖默认；ALL 与命中的 alias 并存则报歧义。ALL 不能自动解释成任意设备均可启用；仍须核对实际 PCI 拓扑、对应单元与当次有效的硬件约束，不能使用无效 Range 字段推断覆盖。
- `acpi_ivrs_route_memory(table, segment, requester, index, out)`：按固件顺序返回属于该单元、适用于原 requester 或 alias DMA requester 的 IVMD。所有替代 IVHD 下的约束都会保留，未命中的设备区间跳过；不去重、不合并权限、不建立 unity mapping，也不执行 exclusion。

查询支持普通/extended select 与 inclusive range，以及单层 alias select/range。alias 目标须有描述且归属同一单元；缺失目标或 alias 链/循环返回 `ACPI_IVRS_UNSUPPORTED`，跨单元归属报歧义。查询匹配到 special/HID requester 时返回 unsupported；其他 requester 可以得到带 `non_pci_entries` 标记的候选，后续驱动仍须完成这类设备的绑定和策略。未选中的替代格式同样检查未知条目、保留字段和 range 邻接要求，不因选中了较新格式而静默跳过未知策略。

没有匹配返回 `ACPI_NOT_FOUND`，格式错误返回 `ACPI_INVALID`。查询失败清空输出；超过 64 个单元或 1024 个 block 返回 unsupported。输出借用不可变表映射，调用者不得释放映射后继续使用 `hardware.entries`。这些是路由候选接口：没有建立 alias 隔离组、合并共享 DTE 属性或落实 IVMD 权限，不授予启用 IOMMU/DMA 的条件。

x86 启动时调用 `amd_iommu_firmware_report()`，先完成全表校验，再记录 IVHD 的类型、segment/requester、寄存器地址及 capability offset，并释放表映射。该报告仍使用原始记录，不自动执行上述路由查询，也不读写 IOMMU 寄存器。`acpi_ivrs_unit_count()` / `acpi_ivrs_unit_get()` 则按单元首次出现的顺序枚举去重后的候选描述，复用路由查询的全表结构与条目策略检查。输出视图借用表映射；出错清空 count/view。单元枚举不代替对具体 requester 的作用域和 alias 查询。

### 固件声明的共享 requester 集合

`acpi_ivrs_requesters(table, segment, requester, &view)` 使用相同的已选 IVHD 策略，返回原 requester、DMA 目标和该 segment 中所有指向同一目标的 alias select/range 来源。结果为排序去重的最多 128 个完整 ID；从目标或任一 alias 查询得到同一成员集合。逐一解析来源以拒绝跨单元/冲突归属，并在完成集合后再次扫描指向成员的 alias，拒绝记录顺序无关的传入链。超限或不支持的链返回 `ACPI_IVRS_UNSUPPORTED`，失败清空整个 view，不暴露部分集合。

`view.memory_count` 与 `acpi_ivrs_requester_memory(&view, index, &memory)` 枚举适用于任一成员的 IVMD，覆盖同一单元的全部替代 IVHD；同一原始记录即使匹配多个成员也只返回一次，保留原始顺序与 flags。原 `acpi_ivrs_route()`/`route_memory()` 的数量和枚举仍只面向原 requester/DMA 目标，语义不变。view 借用表描述符及映射，两者都须保留且不得修改 view；这不是设备引用、占用或权限合并。

这里的完整性限于固件在已选策略下声明的集合，不证明所有物理 alias、ACS 或 peer 隔离。共享 DTE settings/extended 属性的兼容性、特殊设备与实际硬件接管仍须 provider 明确处理；不创建 Linux group 编号或隐式 parent/dependency。

### PCI 与 MMIO 只读探测

`drivers/core/amd_iommu.h` 与 `kernel/drivers/core/amd_iommu_probe.c` 提供 `amd_iommu_probe_unit()`。调用者提供已校验的 IVHD 和只读 transport，返回不含借用指针的 `amd_iommu_snapshot_t`。transport 的 PCI dword 读取须支持指定 segment/requester；MMIO callback 只对已开启 decoder 的单元执行 uncached、对齐的 64-bit 读取，offset 限于 control（18h）和 EFR（30h）。callback 返回负 errno，正数失败规范化为 `-EIO`；调用者负责设备、固件和映射的生命周期及串行化。

探测检查 PCI vendor 是否有效、class `08:06:00`、type-0 header、完整 capability 链以及 IVHD 指定的精确偏移。链表越界、未对齐、循环、匹配项后的损坏链尾和指向目标 capability payload 的其他 header 均拒绝；不能只取第一个 ID 为 `0Fh` 的 capability。目标须为 Secure Device/type 3、interface revision 1，并有完整的 20-byte 或 CapExt 24-byte 空间。PCI base 须与 IVHD 一致，地址宽度须为当前支持的 PA 40/48/52、VA 32/40/48/64。Range register 只描述同一 bus 的 integrated device/root port 范围，不能直接用它判断下游 requester 是否覆盖。

[QEMU 10.0 的 capability 定义](https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/i386/amd_iommu.h) 未设置 CapRev，实际报告 revision 0。实现仅为 PCI `1022:1419` 且 subsystem `1af4:1100` 的组合接受这一差异，在 snapshot 和启动日志标记 `revision_quirk=1`；其他 revision 0 或未知 revision 仍拒绝。这不证明物理 revision-0 IOMMU 的接口可用。

`mmio_enabled` 是 PCI capability 的 MMIO decoder Enable 位，不是 IOMMU translation enable。decoder 未开启时不映射、不访问 MMIO，`registers_read=false`，此时 control/efr 的零值不表示硬件实际为零。已开启时才读取 control，且仅在 EFRSup 为 1 时读 EFR。PCSup 决定 16-KiB 或 512-KiB aperture 和相应对齐；11h/40h IVHD 的 PCSup 与读到的 EFR 矛盾则拒绝。10h IVHD 没有 EFR image，若 decoder 关闭且支持 EFR，完整 aperture 尚未知，`register_size=0`，不猜测为 16 KiB。其他情况下按固件描述或读取结果返回 aperture 大小。

没有对应函数/capability 返回 `-ENODEV`；不支持的 class/header/interface/address width 返回 `-EOPNOTSUPP`；畸形链或 aperture 返回 `-EINVAL`；base 或 PCSup 矛盾返回 `-EIO`；transport 错误原样传播。错误清空整个 snapshot。探测成功不等于资源已独占或设备已接管，也不保证后续时刻的硬件状态；硬件初始化时必须重新验证。

PCI host 枚举后，x86 调用 `amd_iommu_pci_report()`，对去重候选做上述探测。当前架构 PCI HAL 没有 segment 参数，启动适配器明确拒绝非零 segment。适配器在 discovery 事务内先检查 active claims/存储保留，再按需映射首个 16-KiB 寄存器窗口并配对 unmap（x86 映射仍遵循架构既有的共享、永久映射契约）。日志报告原始 control/EFR、地址宽度和 aperture；`verified` 只统计只读探测成功的单元。该诊断不建立持久占用。下述被动驱动负责与已发布声明/claims 核对完整 aperture。PCI 路径/Range、IVRS/绑定路由及 requester 集合占用已提供，IVMD 可转换为独立约束并用于通用映射检查。全部 alias peer 的停止/归属协调、硬件 PTE/Unity/Exclusion 和 DMA domain provider 仍待实现。

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

probe 失败按相反顺序先 unmap、再释放各 claim，最后释放 IVRS 映射和上下文；部分成功时同样回收。remove 先将上下文标为 detached 并从活跃绑定索引摘除：没有路由句柄时立即回收，否则保留全部映射、claims 和 IVRS，直到最后一个句柄释放。claim 保留 device 引用，绑定本身由核心串行化。当前没有硬件写入、DMA、IRQ 或后台生产者；未来增加翻译/队列时，还须落实停止、失效失败时继续保留硬件可达页面和上下文的协议。被动路由句柄不能替代硬件停止。

`AMDVI_OWNER: bound ... writes=0 translation=0` 只证明被动软件绑定成功。已有受管 RAM 与 ACPI 表页保护，但 PFA 以外的 RAM/其他固件预留仍未完整导入；raw MMIO/PIO 不受 claim 拦截，物理热拔插也不被 lifecycle mutex 冻结。该绑定不授予启用翻译、重置控制器或改变固件既有 DMA 路由的权限。

PCI 另提供 `pci_device_find_get(segment, requester, &device)`：在 discovery 事务内查找已发布的 function，并返回需由调用者 `device_put()` 的引用。失败清空输出；非零 segment 返回 `-EOPNOTSUPP`，不存在返回 `-ENODEV`。设备未绑定时仍可查询；旧引用跨设备替换保留原元数据，不被新身份覆盖。引用不固定绑定或物理设备，若 lookup 和后续 config/绑定观察必须协调，调用者须用外层 `driver_discovery_begin/end` 覆盖完整操作。

显式自检须同时指定 `driver.runtime_test=1 driver.amdvi_owner_test=1`。它在启动客户端前检查竞争 owner 被拒绝，注销整个被动驱动后确认占用可重新申请，再注册并仅恢复此前绑定的控制器、确认占用恢复；不会重试其他未绑定设备。测试无硬件写入，成功输出 `AMDVI_OWNER: PASS claim=1 rebind=1 writes=0` 后随运行时自检关机；普通启动不会执行注销/重绑。实例入口为 `tools/a20 test smoke-amdvi-owner-x86_64`。

### 保留路由对象与控制器生命周期

`amd_iommu_route_acquire(client, &route)` 是任务上下文中的显式对象接口。它在 discovery 事务中，从当前被动绑定保留的 IVRS 解析原 requester 与实际 DMA requester，选择唯一匹配的 controller/capability，重新探测该单元，并核对绑定时的 ID、capability、Range、地址宽度、完整 aperture、MMIO 开关、control/EFR。有效 Range 是额外覆盖约束；无效 Range 采用 IVRS 路由并核对实际 PCI 路径，不将未定义字段解释成全覆盖。原 requester 与 alias 目标都必须是已发布、身份有效且路径无歧义的 PCI function；alias 目标未发布时拒绝，不猜测其存在。多个匹配控制器或固件歧义返回 `-EBUSY`，缺少绑定/路由/目标返回 `-ENODEV`，其余固件、探测和桥查询错误按既有规则传播；失败清空输出。

成功获取观察句柄后，它独立保留 client、实际 DMA function 和两条查询的 anchor 引用，并持有控制器上下文的一份租约。仅 acquire 不建立 parent/dependency、不接管 client、不独占 requester/alias，也不启用 BME、翻译、队列或 DMA。原始 settings/extended、non-PCI 标记与 IVMD 都是后续 provider 必须处理的策略，不能当成已落实权限。

`amd_iommu_route_validate(route, &info)` 重新核对当前绑定、硬件快照与两条 PCI 路径，成功返回不含借用指针的值副本。binding 解绑/移除后返回 `-ESHUTDOWN`，即使同一 function 已重新绑定也不会使旧句柄重新有效；单元快照或所保留 alias/anchor 身份变化返回 `-ESTALE`，消失/注销等错误由 PCI 查询传播。所有失败清空 info。查询不冻结硬件；如验证后要继续观察绑定/配置，调用者须用外层 discovery 事务覆盖操作。硬件启用仍需完整 provider 协议。

`amd_iommu_route_requesters(route, ids, capacity, &count)` 复制完整固件声明集合，不返回借用指针。NULL ids/capacity=0 可查询数量，容量不足返回 `-ENOSPC` 并报告所需 count；缓冲区在所有错误下不变，其他错误清零 count。元数据在控制器解绑后仍可读取，这项查询不执行活跃接管检查。

`amd_iommu_route_reserve(route)` 重新验证路由，再检查完整集合的每个成员：必须已发布、内核所有、PCI 路径有效且满足可用 Range，配置读取正常且 BME 关闭。缺失成员返回 `-ENODEV`，不假定固件声明但未发布的 function 已安全停止；BME/用户所有权冲突返回 `-EBUSY`，读取全 1 返回 `-EIO`。全部检查成功才原子占用所有 requester，并保留每个成员的 device/anchor 引用。同一有效句柄重复 reserve 返回 `-EALREADY`，任一重叠占用返回 `-EBUSY`，没有同 client/owner 豁免。分配或其他检查失败回收全部临时引用，没有部分占用，也不消费原观察句柄，可修复后重试。

预留后的 `validate` 还检查所保留的每个成员：身份/anchor 替换返回 `-ESTALE`，消失/注销返回 `-ENODEV`，转交用户所有权返回 `-EBUSY`。失败清空 info，但不释放占用或刷新旧身份。占用独立于控制器绑定代次；解绑后旧句柄返回 `-ESHUTDOWN`，仍阻止新绑定/新 BDF 身份取得重叠占用，直至显式 release。`info` 保持原值结构。

调用者须先通过各设备的所属驱动停止全部成员的 DMA，再 reserve；接口不自动解绑、调用其他驱动的 quiesce 或排空硬件。device 引用不固定成员驱动绑定，validate 也不重新证明 BME 关闭或 DMA 已停止。raw 配置与旧 provider 未被强制接入占用登记。硬件使用和归属变更仍须外层事务与 provider 协议协调；停止/失效不确定时继续保留路由与占用，确认停止后才 release。

`amd_iommu_route_memory(route, index, &memory)` 按固件顺序复制原 requester 与 alias 的相关 IVMD，不归并或执行 unity/exclusion/读写权限，越界返回 `-ENODEV` 并清空输出。保留的不可变固件策略在解绑后仍可读取。`amd_iommu_route_release(route)` 在任务上下文独占释放句柄，禁止与同一句柄的查询并发；释放已 reserve 的路由也会释放 requester 集合；最后一份路由释放后，detached 控制器才按先 unmap、再释放 claim、最后释放 IVRS 的顺序回收。控制器是内建实现；句柄不提供通用模块绑定保活契约。

`tools/a20 test smoke-amdvi-route-x86_64` 在 q35 root port 下提供 EDU，并显式设置 `driver.amdvi_route_test=1` 与运行时自检参数。测试获取路由后先释放原始 client 引用，再验证 requester、单元和 IVMD 枚举，释放句柄并输出 `AMDVI_ROUTE: PASS route=1 policy=1 scope=1`。普通启动不执行；该实例不提交 DMA。解绑后的保留、重绑、alias 与有效 Range 分支另由主机后端验证。

实例 `tools/a20 test smoke-amdvi-requester-x86_64` 再显式指定 `driver.amdvi_requester_test=1`：两个独立观察句柄竞争同一 EDU requester，验证重复 reserve、冲突与持有占用的句柄释放后的重试，输出 `AMDVI_REQUESTER: PASS reserve=1 conflict=1 release=1`。不启用 BME 或提交 DMA；正常启动不执行。

上述 requester 实例及 policy 实例还验证集合的容量查询和复制契约，输出 `AMDVI_PEERS: PASS query=1`。未新增启动参数；QEMU EDU 只有一个声明成员，多 alias 成员检查由主机模型验证。

### 从 IVMD 构造独立 DMA policy

`amd_iommu_route_policy_create(route, &policy)` 在 discovery 事务中重新验证活跃路由，再为完整固件声明集合复制全部保留 IVHD 替代格式下的匹配 IVMD，包括 sibling alias 的独有约束；每个原始记录只复制一次。按 [AMD IOMMU 3.10 表 110–111（303–304 页）](https://kib.kiev.ua/x86docs/AMD/IOMMU/48882-3.10.pdf)，IR/IW 转为设备 READ/WRITE，Unity 转为 IOVA identity；不同记录的限制由通用 policy 交集处理，不能只取选中 IVHD 的记录或忽略目标/其他 alias 成员。原 `info.memory_count` 和 `amd_iommu_route_memory()` 仍保留原 requester/DMA 目标两者的枚举语义，完整 policy 的规则数可更多。

相关 ExclusionRange 或非零高四位返回 `-EOPNOTSUPP`，不把硬件旁路降格为普通规则或无约束 domain；非排除记录的 Unity=1 且 IR=IW=0 返回 `-EINVAL`。其他路由错误原样传播，失败清空输出并回收部分规则，原 route 仍由调用者持有。没有匹配 IVMD 时返回空 policy；这表示没有额外内存限制，不证明设备可启用。

policy 是不含借用指针的值快照，其引用不保留 route、设备或控制器；关闭路由、解绑控制器并释放固件映射后仍可检查旧约束。创建新 policy 仍要求活跃路由，旧快照不因重绑而刷新。创建 policy 不要求先 reserve，也不会自动占用 requester 或初始化硬件；它不替代成员已发布/停止的接管检查。未来 AMD provider 仍需协调全部 alias peer、完整控制器策略与活跃状态，选择带 policy 的 managed 构造器，并在硬件中落实权限和必需 Unity 映射。只检查本路由匹配的记录，不能推导其他 requester 没有 ExclusionRange。

`tools/a20 test smoke-amdvi-policy-x86_64` 显式增加 `driver.amdvi_policy_test=1`，在路由释放后检查独立 policy，输出 `AMDVI_POLICY: PASS create=1 retained=1`。QEMU fixture 没有 IVMD；它只验证空 policy 的启动集成和引用，不验证有效 IVMD 或实际硬件映射。权限、Unity、跨 alias/替代格式的交集、错误、源表释放后的独立存储与分配回滚由主机后端验证。

## 原生异步 fence

头文件：`kernel/include/drivers/core/fence.h`；实现：`kernel/drivers/core/fence.c`。

`driver_fence_create()` 创建 pending 对象。`status()` 返回 0 表示 pending、1 表示成功、负 errno 表示终止错误。`signal(fence, 0)` 完成成功；负 errno 表示失败；竞争 signal 中只有第一次生效，后续返回 `-EALREADY`。signal 不分配内存，可以在 IRQ 中调用，唤醒不发生在 fence 自旋锁下。

`driver_fence_wait(fence, deadline, interruptible)` 使用既有 Park/Wake 和 wait queue，deadline 是 `clock_get_ticks()` 同单位的绝对截止时间，0 表示无限等待。成功返回 0；失败返回 producer 的错误；等待期限到返回 `-ETIMEDOUT`；中断返回 `-EINTR`；无法建立等待返回 `-EAGAIN`。最后一次检查已可见的终止结果优先于超时。

超时或等待者退出不完成 fence、不取消设备任务，也不允许释放 DMA 页面。请求队列和完成中断的 producer 必须持有自己的 fence 引用，直到完成处理返回。等待者持有独立引用，wait 过程再额外保留一份。IRQ producer 不执行最后一次 put；由任务上下文回收对象。

native request 的完成生命周期：

1. request 保留 owning RAM/映射所需存储及自己的完成 fence，submit 成功后由队列持有 accepted 引用。
2. issue 发布硬件；等待者超时或退出不取消 DMA。
3. IRQ/poll 确认硬件交还请求后 complete，任务 retire 成功完成缓存交接、映射失效和 cookie 清理。
4. 锁外 signal 请求 fence，再交还 accepted 引用。stop/retire 失败保留请求和 pending fence，显式重试。

独立的低层 fence 可以由 IRQ signal 一个明确的内部阶段，但它不是 backing 或 DMA 停止凭据；新通用执行路径按[请求队列契约](request-queues.md)处理最终退休。

这是原生内核同步基础，不是 Linux `dma_fence` 的结构兼容实现；没有接入 DRM syncobj、timeline、sync_file 或用户态等待 ABI。现有 DRM 能力位不会因此自动改变。

### 一次性通知与显式撤销

`driver_fence_watch_create(fence, notify, context, &watch)` 在可分配的任务上下文为 pending fence 登记一次性通知，调用者全程持有 fence 引用。成功交出一个显式 owner，额外保留 fence；notify/context 不由登记对象管理，调用者必须保留代码、模块及上下文，直到 try_remove 成功。它不是新用户 handle、第二个完成对象或 Linux dma_fence 结构；没有新全局注册表。

有效输出在失败时清空：无 fence/callback/out 返回 `-EINVAL`，分配失败返回 `-ENOMEM`；若登记准入时 fence 已终止，返回 `-EALREADY`，不安装也不重放 callback，调用者随后查询不可变 status 即可推进依赖。初次观察已终止时无需分配登记存储，注入 heap OOM 也不阻断这个快速路径；分配期间发生完成则在发布锁内重新核对并释放临时登记存储。成功登记与 signal 竞争时不会丢失通知；callback 可能早于 create 返回，因此 context 必须预先初始化，不能依赖注册调用后的补充赋值。

获胜 signal 先发布原有 terminal status 并唤醒阻塞等待者，再在锁内逐个选择登记、标记 RUNNING，退出 fence spinlock 后调用 notify(context, status)。status 为 1 或 producer 的负 errno。只有获胜 producer 派发；同 fence 的通知由该 producer 串行执行，其他 fence 可以并发。callback 继承 producer 的任务或 IRQ 上下文；signal 调用者不得持有通知所需的外层锁。callback 必须短小、IRQ-safe、不阻塞，不做分配/VFS/等待或最终对象回收；典型用途是 driver_work_schedule。signal 不分配、不释放 watch，也不交还 watch 的 fence 引用。producer 自己的引用必须覆盖整个 signal（包括全部 callback），不能因 status 已终止就先交还。

`driver_fence_watch_try_remove(&watch)` 限任务上下文、单 owner 串行调用，始终不等待：pending 时 O(1) 撤销，callback 已返回时确认退休；成功清空指针，在锁外交还 fence 引用和登记存储。若已选中 callback 且尚未返回，返回 `-EBUSY` 并保留指针、引用及代码/context，调用者显式重试。NULL 参数返回 `-EINVAL`，已清空指针成功。terminal status 不等于 callback 已返回；即使 status 已终止，尚未选中的 pending 通知仍可被撤销。成功通知后也必须 remove，不能让 IRQ 路径执行最终 put。撤销不会改变 fence、停止 GPU/DMA、取消已排入的 work 或释放 callback context；这些由各自 owner 协调。callback 已调度的工作须另行 cancel/flush 后才可销毁上下文。

fence spinlock 只保护 status、登记链及 PENDING/RUNNING/DONE 交接，回调、wait queue wake、最终 put/free 均在锁外；callback 返回后发布 DONE，producer 不再读取该 watch，避免 owner 随即释放造成 UAF。signal 对登记数量线性遍历；大量依赖宜通过短通知调度后台工作处理，不在 IRQ callback 中展开任务提交或资源退休。native queue 已使用通知推进依赖并在取消/退休时撤销 watch；厂商 BO/GPUVM、硬件调度/恢复和 DRM 同步 UAPI 仍需独立实现。

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

cancel 成功后其他 producer 仍可重新提交，释放 context 前必须先停止这些 producer。新驱动在 execution stop 中禁止新请求并报告失败，排空或停止/复位硬件，并解除回调依赖的完成等待，同时保留回调仍访问的存储；随后停止 IRQ 来源并 `free_irq()`，销毁/排空工作队列，最后让 remove 释放实例存储。不能先关掉回调所依赖的完成中断，再无限等待该回调退出。回调不得等待同队列上的后续工作，也不能在持有其需要的锁时同步取消它。跨队列依赖环由驱动避免；多个 native execution queue 可共享一个 driver_workqueue；这不等于动态全局 worker 池、完整 Linux workqueue ABI 或通用死锁检测器。

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
python3 -m unittest tools.tests.test_driver_memory tools.tests.test_driver_resources tools.tests.test_resource_pci_integration tools.tests.test_pci_resources tools.tests.test_amd_iommu_probe tools.tests.test_acpi_ivrs tools.tests.test_amd_iommu_owner tools.tests.test_pci_scope tools.tests.test_pci_requester tools.tests.test_amd_iommu_route -v
python3 -m unittest tools.tests.test_driver_protocol tools.tests.test_driver_queue tools.tests.test_driver_quiesce tools.tests.test_driver_dma_mapping tools.tests.test_driver_fence tools.tests.test_driver_workqueue tools.tests.test_driver_irq_sync tools.tests.test_driver_firmware tools.tests.test_riscv_iommu_queue tools.tests.test_riscv_iommu_watch -v
make ARCH=x86_64 BOARD=qemu-virt-x86_64 kernel-only -j4
make smoke-driver-lifecycle-x86_64 -j4
tools/a20 test smoke-driver-runtime-x86_64
tools/a20 test smoke-driver-runtime-smp2-x86_64
tools/a20 test smoke-amdvi-owner-x86_64
tools/a20 test smoke-pci-scope-x86_64
tools/a20 test smoke-amdvi-route-x86_64
tools/a20 test smoke-amdvi-requester-x86_64
tools/a20 test smoke-amdvi-policy-x86_64
tools/a20 test smoke-driver-iommu-native-riscv64
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=linux BRINGUP=1 DRIVER_DEPLOYMENT=embedded kernel-only -j4
```

主机测试编译生产 C 并使用 ASan/UBSan。quiesce 测试运行真实设备核心和 class registry，覆盖在途调用、发布失败、重新绑定和无 class 设备。DMA 测试替代硬件翻译/页分配器，覆盖非恒等地址、96 页映射、页面存活、掩码、逐页失败回滚、分配失败和架构未就绪。fence 测试以 pthread 替代调度器，注入 prepare/commit 边界，并覆盖 80 等待者与竞争 signal。

QEMU 生命周期门禁还保留一个在途 class 调用，由 quiesce 结束它，验证生产内核能继续完成 unregister。新增运行时门禁通过显式 `driver.runtime_test=1` 在 init_kthread 中测试实际内核 worker 与 Park/Wake，验证延迟/重设、取消、自重排、销毁超时保留对象和重试。测试成功自动关机，普通启动不执行。它们没有执行实体 GPU DMA，也没有证明 AMD/NVIDIA 硬件初始化成功。主机 fence 测试中的调度和 IRQ 是替代后端，不应声称为真实 IRQ/SMP 调度压力验证。

native queue/protocol 的运行路径另由 `tools/a20 test smoke-driver-gpu-stop-x86_64` 验证真实 worker、停止重试和 call drain，并检查 DRIVER_QUEUE 完成标记；generic VirtIO GPU 模块回归见 `make PYTHON=python3 smoke-gpu-access-x86_64`。主机模型、QEMU 设备和实体 GPU 的证据分别记录。

## 后续规划与演进

1. **固件管理与中断演进**：完善固件目录的系统级部署与启动交接，引入可选线程化 IRQ，优化工作队列资源回收与故障恢复策略。
2. **IOMMU 硬件端到端集成**：在 AMD-Vi 被动绑定与完整 aperture claim 基础上，基于显式 PCI 集合占用和 IVMD 约束补齐全部 alias peer 协调与硬件页表策略，完善失效完成与故障恢复协议。
3. **GPU 内存与命令调度**：在已有每设备实例、每文件 adapter 所有权和固定 resident RAM 底座上，逐步实现 BO 驻留迁移策略、GPUVM 映射、硬件上下文与调度队列，复用已落地的 Native request/queue/VMO/fence 所有权模型。
4. **图形硬件适配**：完善特定硬件（如 AMD Phoenix / VirtIO-GPU virgl）的固件与 IP 初始化、SDMA 传输与像素呈现，打通上层 Mesa 驱动与呈现通道。

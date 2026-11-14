# 原生驱动运行时：GPU 开发的第一阶段基础

本文区分已经接入源码的接口和后续工作。已提供设备 quiesce、资源占用与物理存储保留、页级 DMA mapping、原生异步 fence、可取消的工作队列、IRQ 排空和有界固件读取接口；不表示 AMD/NVIDIA 硬件加速已经可用。DMA/fence/workqueue/firmware 和本页运行时自检面向 hosted 内核；资源占用接口随完整 driver core 编入，相应 MCU 配置不包含 hosted 异步运行时。

## 分层与边界

| 层次 | 职责 |
|---|---|
| 设备核心 | device/driver/bus/class、绑定、引用、依赖、资源回收、下线顺序 |
| 通用运行时 | DMA mapping/domain、IRQ 排空、工作队列、等待与原生 fence、固件读取 |
| GPU 子系统 | 已有 DISPLAY 默认选择与带引用的调用入口；后续实现每设备 BO、GPUVM、上下文、任务队列和同步 UAPI |
| 硬件驱动 | IP/固件初始化、命令协议、硬件完成中断、停止与复位 |
| 可选 Linux 源码兼容层 | 将上游源码接口映射到原生运行时，不成为原生驱动的依赖 |

内置驱动与 `.a20drv` 使用同一 `driver_t`。ABI 9 增加了 `driver_t.quiesce`；当前 ABI 为 **12**，在 GPU vtable 末尾增加显式原生 `get_capabilities`；ABI 11 已增加 owning RAM `get_scanout`；ABI 10 已扩展 `usb_hcd_ops_t.release_endpoint`，所有旧模块必须重建。后文带日期的验证保留当时 ABI，不表示当前接受旧模块。新增 DMA/fence/workqueue/firmware 函数已加入模块导出表。此处没有承诺跨内核版本稳定的二进制 ABI。

## 下线与资源生命周期

成功绑定的设备按以下顺序解除绑定：

1. 若 probe 已显式注册 remove prepare，先 offline provider class 并执行准备动作；失败即返回，尚不解绑 consumer。
2. 准备成功（或未选择准备）后，先解除 consumer/child，再处理 provider/parent。
3. 状态变为 `REMOVING`，`class_device_mark_offline()` 拒绝新类调用，并执行一次 offline observer。
4. 调用可选 `driver_t.quiesce(dev)`。此时既有 class 调用可能尚未返回，`drv_priv`、IRQ、DMA 和受管资源仍然有效。
5. `class_device_unpublish()` 撤销 registry publication，等待在途 class 调用结束。
6. 调用 `remove()`；只有返回 0，核心才逆序释放受管资源及依赖并清空绑定。

`quiesce` 必须先禁止新的硬件提交并停止软件生产者，唤醒未完成请求使其成功结束或以错误结束，并尝试排空/停止硬件。无法确认停止时，记录私有失败状态并由 remove 继续确认或返回错误；软件请求以错误结束不授予 DMA 页回收权限。驱动负责同步中断和取消其后台工作；`remove` 只有确认安全后才释放请求队列、DMA mapping 和实例存储。驱动私有入口也必须检查自己的 stopping 状态，class offline 不会自动保护绕过 class adapter 的调用。

不能依赖 `remove()` 唤醒所有在途请求：核心在进入 `remove` 前已经等待这些调用。没有异步操作的现有驱动可以保持 `quiesce=NULL`。

成功 probe 后 class 发布失败也会执行 `quiesce -> drain -> remove`，只有 remove 成功才进入 devres 回收。未选择失败清理的旧 probe 仍自行撤销已经启动的硬件，再返回错误；选择了 `device_probe_use_remove()` 的实例使用下述可失败管线，核心不推测硬件阶段。

### 可失败的解绑与显式重试

`driver_t.remove()` 已有 int 返回值，现在由核心执行：非零结果阻止资源回收，负 errno 原样返回，正数按 `-EIO` 处理。驱动必须在失败时保留有效的 `drv_priv`、硬件可达存储和恢复所需寄存器/代码；不能先释放后报错。`device_unregister()`、`platform_device_unregister()` 和 `device_hotplug(REMOVE)` 返回错误，保留设备注册表引用、绑定、devres、匹配身份及尚未释放的依赖。自己的 remove 已执行失败时，设备保持 `REMOVING`，class 已 offline/撤销 publication；不会重新开放旧句柄。调用者保留对象并在条件修复后重试同一接口，核心只重试 remove，不重复 quiesce/offline observer。正常 unregister 完成前 `device_is_registered()` 仍可为真，不能把成员关系当作在线许可。

未选择 remove prepare 时先处理 consumer/child，它们停止失败时 provider/parent 尚不 quiesce 或释放。选择 prepare 的 provider 可以先冻结硬件，但必须保留 consumer 清理仍需的接口和存储；不能把这个显式协议推广到所有 provider。部分 consumer 可能已解除绑定并进入 deferred；失败不是事务回滚，不自动重建原图。移除意图与递归栈由 lifecycle mutex 下的私有栈帧保护：正在退出的驱动不能接收新匹配绑定，递归退出同一对象返回 `-EBUSY`；正在移除的 provider 拒绝新 dependency（`-EPROBE_DEFER`），其 parent 链拒绝新 child 发布（`-EBUSY`），即使它仍处于等待 consumer 停止的阶段。未新增公开 group/上下文标识或布局字段。

`driver_unregister()` 只有在剩余绑定全部成功解绑后才移除驱动登记；失败保留登记与代码所有权，调用者不能释放模块/driver 对象。已完成解绑的设备保持已完成状态，调用者显式重试处理余下绑定。成功 probe 后 class 发布失败且 remove 也失败时，同样保留绑定/资源；`probe_error` 保存原 publication 错误，不允许下一候选驱动覆盖它。未选择受管失败清理的旧 probe 仍须自行安全回滚；核心无法从普通错误码推断未停止的硬件。

REMOVE 仅在成功后发出 bus notification，核心为通知另持一份 device 引用，防止 registry 最后一次 put 提前销毁元数据。PCI rescan 停止失败时保留旧 BDF 记录，不发布替代身份，也不将缺失 function 的槽标为空闲。已发布 USB 端口先 offline interfaces，再检查 `abort_slot` 的返回；缺少停止回调返回 `-EOPNOTSUPP`，失败保留 slot/address、interface/endpoint 和 HCD 登记。slot 停止成功但后续 remove 失败时，重试不重复 Disable Slot；port 的 pending 状态会在重新枚举前重试，即使物理连接已经恢复。`usb_core_unregister_hcd()` 返回错误，xHCI/hub remove 传播它并保留实例。USB 枚举的部分 slot 和接口图现在也交给同一端口清理路径，见下文；xHCI 的 start/probe 已接入下述退出准备，其余 HCD 和错误阶段仍须各自确认。

公开 `device_t`/`driver_t` 布局未因这项返回值变更而改变；新增模块导出 `device_unregister`，源码调用方须检查上述 int 返回值。关机/kexec 的 `shutdown`、失败 probe 的硬件回滚、实际 stop/失效完成仍需要各 provider 的明确协议；核心不把超时转换成可释放证明。

当前生命周期回调仍持有全局 lifecycle mutex，禁止等待需要该锁的工作或递归注销自己。quiesce 没有通用超时后强制释放内存的语义：如果不能证实 DMA 已停止，驱动不能继续释放硬件仍可能访问的存储。按设备并发的生命周期协调与通用恢复策略仍需后续开发。

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

选定后，probe 非零返回会交给既有 consumer-first、quiesce、class drain、可失败 remove 管线。回调须能处理部分初始化乃至 NULL `drv_priv`；probe 不再自行释放交给 remove 的存储。remove 返回 0 才允许 devres/依赖回收及清空绑定。若 provider 构造失败同时返回非 NULL DMA 清理句柄，应保存在实例中；remove 重试 shutdown，成功后才 put。不能因为初始化返回错误就丢弃清理句柄、页表或 requester 租约。

核心保存原 probe 错误于 `probe_error`（正数规范化为 `-EIO`）。清理成功时 probe 调用路径仍返回原错误，设备转为 UNINIT 或原错误为 `-EPROBE_DEFER` 时的 DEFERRED；清理失败则返回清理错误，保留原 `probe_error`、绑定、匹配身份、私有状态、devres 和依赖。device_register/driver_register 接受登记不表示 probe 成功，仍须观察绑定结果。保留的失败绑定不进入下一候选或自动 deferred reprobe，只能显式注销重试清理；这不是硬件恢复/重新初始化接口。

子设备/consumer 停止失败可能阻止父设备执行 quiesce。此时失败 probe 保持 `DEV_STATE_PROBE_CLEANUP`，其 class 先 offline，恢复后重试仍须执行首次 quiesce；进入自己的 remove 后才变为 REMOVING，后续仅重试 remove。`device_is_stopping()` 同时观察两种状态；checked resource、PCI 查找/范围观察、managed DMA 创建/新映射及 class 发布均拒绝它们。旧引用和旧资源保留供停止，不授予新使用权限。该 helper 只观察元数据，调用者仍须持有引用并按需使用 discovery 事务协调绑定/配置。

probe 意图保存在 lifecycle mutex 下的私有调用栈，不增加公开布局字段或 ABI 版本（当前为 12）。嵌套 probe 各自选择、返回后恢复上一帧；构造尚未返回时对自己或祖先递归注销返回 `-EBUSY`。新 API 已导出给模块。旧驱动不会自动接入；迁移前必须逐一审查部分初始化的 quiesce/remove、错误传播和硬件停止证明。

### 在 consumer 解绑前准备退出

控制器的 IRQ/ring 可能继续访问 child/consumer 存储，等到 provider remove 才停止会太晚。`device_add_remove_prepare(dev, callback, context)` 让驱动显式选择提前准备：只能从该设备当前 probe、且 `device_probe_use_remove()` 已成功后登记。记录在 probe 中分配，失败返回 `-ENOMEM`，不安装动作；错误参数或阶段返回 `-EINVAL`，未注册的合法当前实例返回 `-ENODEV`。公开 device/driver 布局不增加字段，模块导出此函数，该接口不改 device/driver 布局；当前全局模块 ABI 为 12。

```c
int ret = device_probe_use_remove(dev);
if (!ret) ret = device_add_remove_prepare(dev, controller_prepare, dev);
if (ret) return ret;
/* Initialize drv_priv, then activate hardware. Prepare handles NULL too. */
```

准备动作按登记的逆序执行，先于任何 consumer/child unbind；只返回 0 才记为完成。正数错误按 `-EIO` 处理，失败保留绑定、匹配身份、依赖、devres、上下文和未动的消费图；之前成功动作的硬件副作用不回滚。显式注销重试从未完成的动作继续，已成功的动作不会因之后的 consumer 或 remove 失败而重复。普通退出停在 `DEV_STATE_PREPARING`；失败 probe 保持 `PROBE_CLEANUP` 及原 `probe_error`。`device_is_stopping()` 包含新状态，阻止新 class 发布、checked claim、PCI lookup/scope 与 DMA 创建/映射，已有映射仍可关闭回收。准备完成后才进入原来的 consumer-first、quiesce、class drain、remove、devres 流程；未选择者保持旧顺序。

prepare 必须容忍部分初始化，冻结新提交并排空可能访问 consumer 存储的回调/硬件；可以撤销 IRQ，但不能释放 consumer/remove 仍需的存储。callback/context 有效至最终回收，context 的销毁另行安排；完成的准备是单向退出，不能恢复硬件后期待核心重复它。回调在 lifecycle mutex 下、不持 registry spinlock；不能等待需要该锁的 consumer，也不能递归移除自己。未结束的传输可返回 `-EBUSY` 让核心释放锁、保留图后显式重试。准备不是通用并发隔离或超时后的强制释放授权。

xHCI 在 probe 最前选择失败清理并登记准备动作。准备先封闭提交/轮询、撤销并排空 IRQ；同步传输仍持有 event ring 时返回 `-EBUSY`，不在 lifecycle mutex 下等待它。已取得固件控制权的实例还须确认 RUN/INTE 写回清零、RESET 完成、HCHalted、CNR 清零，以及 PCI BME 清除读回；全 1 读取或超时均保留控制器和所有尚未解绑的子节点。尚未取得控制权的早期失败未发布自己的 DMA 上下文，不执行控制器停机。成功证明在私有上下文中单向保留；之后已知 slot 用软件清理，不再向已停止的命令环提交 Disable Slot。remove 只在确认后释放控制器，start 失败也不再自行释放不确定的 ring 存储。

USB 的拓扑协调与锁外回调保留另按下述协议实现，不能仅凭 prepare hook 推导。HID/hub 的独立退出和 Configure Endpoint ring 的错误保留已按下述协议接入；其他 class 迁移仍需完善；xHCI 同步 bulk 已通过下述独立 DMA buffer 保留暂存数据，不再把调用者缓冲区交给硬件。关机/kexec 的 shutdown 也未改为可失败协议。

### 总线拓扑的原生生命周期协调

`driver_lifecycle_begin/end()` 复用核心现有的生命周期 mutex，供任务上下文把总线私有拓扑、注册和绑定操作放入同一个协调域。阻塞入口可在所属任务或启动上下文嵌套；它不增加 discovery 深度，不压制同步 probe。需要批量发现后再 probe 的总线仍使用 `driver_discovery_begin/end()`，不能用它代替普通进度协调。两组 API 均不固定设备、绑定或模块代码，调用者另持有效引用。

`driver_lifecycle_try_begin()` 只尝试新的最外层入口：成功返回 0，忙（包括本上下文已经持有）返回 `-EAGAIN`；只有成功后才配对 end。它避免进度轮询等候正在排空 class 调用的 teardown，也避免在部分修改的图中重入。成功取得协调权后的驱动操作仍可能进行有界硬件等待；这不是整个函数无等待的保证。三项接口均导出给模块，不用于 IRQ 或持有 spinlock 的路径，这三项函数不改公开布局；当前全局模块 ABI 为 12。

USB register/enumerate/scan/unregister 全程使用这个协调域，保护 HCD 注册表、端口状态和协议所有权。`usb_core_poll()` 忙时清除防重入标记并跳过，不推进轮询时间；class 持有协议存储期间通过 `usb_device_poll()` 尝试进度。后者忙返回 `-EAGAIN`，端口不再归属该设备、待移除或任一上游 HCD 正在退出返回 `-ENODEV`，均不进入 HCD。HID read/poll 在自身 spinlock 外使用此入口；不能阻塞等候 teardown 所持的生命周期锁。

同步回调可以注册或操作其他 HCD。相同 HCD 的递归 register/enumerate/unregister 返回 `-EBUSY`，嵌套 scan 跳过；活动下游操作（包括尚未发布的 start）也阻止注销上游。重复注册返回 `-EEXIST`，无效/未注册的 parent 链和正在退出的祖先拒绝新注册；已注册下游尚存时上游注销返回 `-EBUSY`。start 前预留注册表容量，防止不同 HCD 的重入注册越界。scan 每轮从当前注册表重新选择未访问地址，以最多 32 项的栈内集合处理回调造成的增删和压缩；同一地址被重新分配时可以延后到下一轮，绝不遍历借用的旧注册表行。

### xHCI 锁外回调与端口退役

xHCI 在 controller 锁下收集 completion，并在解锁前增加 controller、slot 和 endpoint 在途计数；回调结束重新取锁后递减。跨回调只保存标量 slot ID 与计数保留的私有 endpoint 记录，回调可释放自己的 URB，退役过程不会再次读取该 URB。controller prepare 发现同步传输或任何回调在途时返回 `-EBUSY`，保留图和存储，不能持生命周期锁等待这些回调。

端口 abort 在成功 Disable Slot 后，于同一锁内清 DCBAA、封闭 endpoint 并清 pending；对应 slot 回调未结束时返回 `-EBUSY`，保留其借用的 endpoint 记录和 ring，归零后才摘链释放。重试利用 DCBAA=0 的停止记录，不重复 Disable Slot；其他 slot 的回调不阻止该端口清理。EP0 control 和 interrupt submit 在锁下拒绝已停 slot，interrupt 拒绝发生在清 DMA buffer 和敲 doorbell 之前。

硬件可立即复用 Disable Slot 释放的编号，因此仅记住停止还不够。USB core 在同一物理控制器任一 root/hub 端口仍有 REMOVE_PENDING 时，暂停新的端口枚举，显式入口返回 `-EAGAIN`，scan 跳过且不扣枚举尝试次数；reset 回调后再检查停止和退役状态。直到回调、class 和协议所有者全部交还，才重新开放分配。已有设备的完成处理及其他物理控制器继续运行。该策略保持现有 slot token，不增加另一套公开 generation/所有者模型。

### 接口独立退出与 endpoint 释放

`usb_interface_release_endpoints(iface)` 从当前接口的 remove 回调调用，先由核心 quiesce/offline 并排空 class 调用。接口/协议存储由调用者保留；helper 在生命周期协调域内核对 stopping owner、plat_data 和端口归属，再逐个调用 `release_endpoint(hcd, dev, addr)`。在线或错误 owner 返回 `-EINVAL`，协议不再属于已登记 HCD 返回 `-ENODEV`，非空 endpoint 清单缺少回调返回 `-EOPNOTSUPP`；正数后端错误规范化为 `-EIO`。已证明整个 slot 停止时无需再次向硬件发 endpoint 命令。

HCD 回调返回 0 才证明该非 EP0 endpoint 不再 DMA/回调访问 URB 和 buffer，并释放其 controller 侧记录；之后同地址的再次释放无操作。失败保留仍存续的全部所有权，已释放的前序 endpoint 不回滚，重试只清理剩余项。此操作永久关闭当前 endpoint，不是单 URB 的取消/恢复 API；其他 endpoint 和 EP0 保持可用。HCD 字段最初在 ABI 10 扩展；当前 loader 和 drvctl 只接受 ABI 12，拒绝其他版本，不能通过强制读取短 vtable 兼容旧模块。

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

这些 USB 清理变更保持其公开结构布局；当前全局模块 ABI 12 另由 GPU vtable 扩展。它们保证已交还的已知 slot 不会因错误码而丢失所有权，不证明未知 Enable Slot 结果、HCD 自身 start/probe、致命控制器恢复或硬件停止超时已经解决；控制器必须在 `abort_slot` 返回 0 前确认不再访问相关存储。同步 control/descriptor 回调也必须在返回前解除对调用者临时缓冲区的访问，不能靠保留端口代替该回调契约。

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

当前所有 `.a20drv` 与用户驱动描述符必须按 ABI 12 重建。kernel descriptor guard、loader 入口与 `drvctl` 均拒绝旧 ABI 11，不能通过强制读取短表兼容。RAM `get_scanout` 引入于 ABI 11，device/driver/public class 布局未在此扩展；本次版本变化来自 GPU vtable 的新尾部回调。

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

`gpu_device_get_default()` 保留为旧的借用快照，使用它必须由外层协调整个使用期间的生命周期，不能据此读取可失效的 ops。DRM 已按下述协议迁移到固定文件 publication、每 publication 状态及显式 scoped access；厂商 BO/context/request/fence 与旧物理映射退役仍需后续实现。上述消费者改造不等于 GPU 热拔插或持久资源生命周期已经完成。四项 access 函数已导出，device/driver 布局不变；RAM scanout 回调引入于 ABI 11；当前模块 ABI 12 新增上述原生能力回调。

## 连续 RAM VMO 与 owning scanout

`vmo_create_contiguous(bytes, &memory)` 是内核 MM 的专用 RAM 工厂，不是新用户态对象类型或设备 DMA 分配接口。调用者在可分配的任务上下文、不持 MM/spinlock；零长度/NULL 输出返回 `-EINVAL`，超出 `PAGE_SIZE << MAX_ORDER` 返回 `-E2BIG`，分配失败返回 `-ENOMEM`，地址/CPU alias 范围异常返回 `-ERANGE`，PFA 转换失败返回 `-EIO`。错误清空有效输出且不遗留 owner。长度向页取整，物理地址 0 有效，全部暴露字节包括末页 padding 先清零。

工厂在发布前分配高阶块，经已有 `pfa_split_allocated()` 一次转换为各有独立引用的 canonical order-0 页；逻辑范围只保留所需前缀，未用的幂次尾部通过 frame_put 交还。转换前的失败按原 order 释放，转换后不再原阶释放。随后发布内部 `VMO_CONTIGUOUS` 类型，沿用 VMO 原有结构、Rust ref/pin/destroy 和 TLB 后 frame_put 路径。`vmo_contiguous_info(memory, &physical, &bytes)` 要求调用者持有 VMO 引用，返回稳定的 CPU 物理基址和页取整 extent；输出在失败时清零。不可变指 extent 与 canonical slot，不是像素内容只读：公开 resize 返回 `-EOPNOTSUPP`，直接 Rust resize 的锁边界也拒绝修改（错误返回沿用旧约定），缺失 slot 返回 `-EIO`，不能 demand fault 出新页填洞。

该工厂不捕获设备 mask、不建立 IOVA、不授予 DMA 权限，也不转移 cgroup charge。它适合可信内核 RAM backing；有 DMA 地址/权限要求时继续通过 domain 和 `driver_dma_map_vmo()` 检查。kind 4 已加入 canonical pin 白名单；这些页在转换后具有真实 order-0 引用，并非把高阶尾页伪装成可 pin 页。普通可变 VMO 的页快照仍不冻结 extent，连续 VMO 的 extent 固定来自其自身类型约束。

GPU vtable 的 `get_scanout(device, &snapshot)` 引入于 ABI 11；当前模块 ABI **12** 另追加原生能力查询，旧模块必须重建。`struct gpu_scanout { struct vmo *memory; uint64_t offset; size_t size; }` 持有一份 VMO 引用；offset 页对齐，size 是正的可见字节长度，向页取整的覆盖范围必须完全落在上述不可变 RAM extent 中。提供者须在有效 class 调用内、与模式替换协调地捕获同一 backing/offset/size；即使返回错误，只要 memory 非空，也向 helper 交出该份引用。没有回调返回 `-EOPNOTSUPP`；存在回调但失败不能降级成借用物理映射。当前只接受连续 RAM，不把 VRAM/MMIO 当作普通 VMO。

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

`virtio_transport_reset(transport, timeout_ms)` 位于原生 bus 层并导出给模块，不改变 transport vtable 或当前 ABI 12。任务上下文、不持 spinlock，调用者持有 transport/MMIO/代码和设备所有权，并排除新提交、并发 reset 及重新初始化。参数要求完整 read32/write32、1–1000 ms，否则 `-EINVAL` 且不访问寄存器。函数写 status=0、执行 barrier 后反复读回，只有读到 0 才成功。规则依据 [OASIS VirtIO 1.3 的 2.4、4.1.4.3.2](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)：设备在报告 reset 完成后停止队列交互和通知，PCI 重新初始化须等 status 读回 0。

时间预算以频率分解计算 ceil，避免毫秒乘法溢出；elapsed 差值允许计数器回绕，起点 0 也是有效时钟。另有 100 万次读取上限，保护早期启动、频率未知或停滞时钟；因此 poll 预算可能先于时间预算耗尽，并非保证等待足额毫秒。任务存在时每 256 次尝试 yield；无任务时有界 poll。耗尽返回 `-ETIMEDOUT`，不隐式清理任何资源。它只确认合规 transport 的状态，不排空软件调用、不注销 IRQ、不解除 IOMMU 或回收存储，也不证明恶意/损坏硬件或物理拔除安全。

virtio-gpu 在任何 PCI/MMIO 初始化前选择 `device_probe_use_remove()`，并把已初始化 mutex/wait queue 的部分实例交给 dev->drv_priv。验证 transport 身份后，首次 reset 也必须读回确认，再协商特性/发布 controlq。失败仍可设置 FAILED，但由原生 offline/quiesce/drain/remove 管线清理；FAILED 本身不授权释放 DMA。reset 不完成时返回错误，核心保留绑定、原 probe_error、MMIO/devres、IRQ、命令暂存和 framebuffer owner，后续显式重试同一个 remove。

quiesce 原子关闭提交，先于 class drain，不等待 command mutex。四条 controlq issuer 在同一 mutex 内检查 closing/command_failed，再分配、填充和发布 descriptor；closing 返回 `-ENODEV`，已故障队列返回 `-EIO`。等待发现 closing 可结束本次调用。未确认完成返回 `-ETIMEDOUT` 并封闭整个共享 queue，不能只封闭该 ioctl；错误 used id 或 used index 增量不等于 1 同样封闭，不再复用、扩容或释放设备仍可能访问的暂存区。已确认返回链的设备错误响应本身不等于 DMA 未结束。未发布的 staging OOM 返回 `-ENOMEM`，交还软件锁，不凭空将队列判为故障。

remove 先拒绝 command_active，再 trylock；这同时防止原生递归 mutex 把同上下文的活动命令误当成可退出。忙返回 `-EBUSY`，不在 lifecycle 锁下等候 issuer。所有合法 class 调用已由核心排空，随后在 mutex 内执行可失败 reset；确认成功后才 free_irq 排空 handler、撤销默认选择并交还命令/framebuffer owner。reset 失败保留全部状态，既不清 drv_priv，也不释放 RAM 或 IRQ。snapshot/VMA 引用可在 producer 成功退出后继续保留旧 RAM。

### VirtIO GPU 的每设备实例

virtio-gpu 不再使用全局静态实例或 owner 槽。每次 probe 在选择原生失败清理后，先以 `driver_dma_buffer_create(dev, sizeof(virtio_gpu_inst_t), __alignof__(virtio_gpu_inst_t), &allocation)` 分配独立的零填充、物理连续且满足当前 mask 的实例。该 owner 保留设备元数据，覆盖内嵌 controlq、固定命令暂存、mutex 和等待队列；transport、EDID、feature、fence 序号、动态 staging 与 framebuffer VMO 引用也都在同一 `dev->drv_priv` 中。每个绑定仍只有一个同步 controlq，不是每请求或每进程队列。驱动无需另建实例注册表。

分配失败保留原生 errno，不访问 transport、不发布 drv_priv，也不留下设备 pin；同一 device 已有 drv_priv 返回 `-EBUSY`，其他 device 不受影响。probe 后续失败交给既有 quiesce/remove，保留这一个部分实例。关闭、未知完成和 reset 失败只封闭该设备；不覆盖或重置另一设备的队列。成功 remove 先确认 reset、排空 IRQ，再注销默认选择并清 drv_priv、交还动态 staging/framebuffer producer 引用；最后释放 command mutex，才 put 包含 mutex 本身的实例 allocation。失败则完整保留 allocation、设备 pin、IRQ 和 backing，显式重试同一绑定。

这复用已有 DMA buffer 的分配与所有权，实例化不扩展 device/driver/GPU vtable；当前 ABI 12 的能力回调见上文。allocation 地址是 CPU 物理地址，现有队列仍使用平台恒等 DMA 假设；没有由此建立 IOMMU domain、GPUVM、厂商任务/fence 或安全物理拔除协议。旧 scanout 的 VMO/VMA 和 DRM 的 publication/GEM 各自持有独立所有权，实例释放不撤销这些 CPU 存储引用。两个 VirtIO 设备能独立运行，不表示 card0/renderD128 默认聚合入口已变成多 GPU 节点枚举或跨 GPU PRIME attach。

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

这些消费者更改不扩展公开布局，当前模块 ABI 为 12，不实现厂商 BO/VRAM、驻留/预算、GPUVM、通用 job scheduler、DRM 同步 UAPI、完整模块卸载或实体 GPU 加速。fbdev open 仍按操作选择默认设备，旧 get_fb 持久物理视图仍借用存储。

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

分配不取得 lifecycle mutex，可从 active class 调用使用，避免与持该锁排空 class 的退出互相等待。它只构造存储所有者，不批准硬件提交。这些函数均导出给模块，这些 buffer 函数不改 device/driver/HCD 布局；当前全局模块 ABI 为 12。

### 将 buffer 显式映射到 domain

`driver_dma_map_buffer(domain, buffer, direction, &mapping)` 把请求大小向上取整到完整页，复用 domain provider、权限预检、设备地址校验、同步和可失败回滚；每个 mapping 只保留一份整块 buffer 引用，不对高阶尾页调用 frame_get/put，也不拆分原始分配。33 字节、64 KiB 对齐的对象只映射一页，PAGE_SIZE+33 字节映射两页，不暴露其余用于强对齐的 backing。最后一页的清零 padding 也属于设备可访问范围，policy 必须允许这一整页；不可用只覆盖请求字节的检查绕过 padding 限制。

调用者在 map 调用期间持有 buffer 和 domain；成功后可交还原 buffer 引用，mapping 继续保留原分配及 allocation owner 元数据，直到全部失效确认成功。allocation owner 与 domain device 可以不同：存储引用不授予设备权限，准入、mask、policy 由目标 domain 决定。每页 IOVA 可能不连续，必须用 `driver_dma_page_address()` 逐页读取，不能从 buffer PA 或首个 IOVA 推算其余地址。buffer 与 order-0 数组共享 `mapping_count/sync_for_device/sync_for_cpu/try_unmap_pages`；count 是保留的页条目数，失败清理期间可能只有已发布的前缀，但 buffer 的整个高阶分配仍然保留。

map 返回非零也可能交出非空清理句柄，必须继续保留并重试；unmap 开始后禁用地址/同步，部分失效成功不会提前释放 buffer 或执行 CPU invalidate。全部成功后才执行必要的 CPU 同步、交还 buffer/domain 引用并置空句柄。managed domain 的 map_buffer 与原有 map_pages/unmap/shutdown 使用同一 mutex；最后 put 和 provider 操作均限任务上下文。不替驱动停止 DMA，也不协调同一 buffer 在其他 mapping 或 CPU 中的并发访问。

RISC-V native provider 已按此契约借用通用 mapping 持有的存储，私有 IOVA 的 pinned 条目为 PFN_NONE；不再二次 pin 高阶尾页。用户态正 PID API 仍保留独立 order-0 frame 引用以覆盖失败 quarantine；旧 map/unmap 拒绝负 native owner，禁止绕过通用 mapping 的计数和所有权。硬件 PTE、IOVA 和失效/关闭重试协议保持独立；这不提供 GPU 页表、BO 驻留或自动选择 provider。

### xHCI 同步 bulk 暂存

xHCI probe 在启动自己硬件前预分配一份 64 KiB、64 KiB 对齐的 DMA buffer，每控制器一份，HID-only 控制器也承担这项固定占用。`xfer_busy` 串行保护整个 copy/submit/wait；OUT 先拷入，IN 先清零暂存区并同步给设备。Normal TRB 只引用该物理地址，不引用 URB 或调用者存储，调用者内存无需满足设备地址窗口及 64 KiB 边界。copy 后重新检查 fatal、slot 停止和 endpoint closing，拒绝期间发生的退出。

bulk 是精确长度同步接口，不运行 complete 回调：0 表示完整长度完成；超过 64 KiB 或短包返回 `-EMSGSIZE`，残余计数超过请求返回 `-EIO`。只有完整成功的 IN 才同步并拷回调用者，所有失败均不发布部分输入。当前接口不提供变长 bulk 的 actual_length，也不做多 TRB 链；调用方按已知长度分块。边界及 Transfer Event residue 依据 [Intel xHCI 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf) 的 6.4.1、6.4.2.1。

超时后即使 HALT 未被证明，硬件仍只能访问 controller 持有的暂存区；fatal gate 阻止复用。controller prepare 失败继续保留这份引用，remove 只有确认 controller 停止且 USB 图清理成功后才 put；外部额外引用仍能继续保留存储。临时 URB 和调用者缓冲区在任意返回后都可结束自己的生命周期。这不代表 class 静态实例已迁移，也不把软件 fatal 标志当作 DMA 停止证明。

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
python3 -m unittest tools.tests.test_driver_memory tools.tests.test_driver_resources tools.tests.test_resource_pci_integration tools.tests.test_pci_resources tools.tests.test_amd_iommu_probe tools.tests.test_acpi_ivrs tools.tests.test_amd_iommu_owner tools.tests.test_pci_scope tools.tests.test_pci_requester tools.tests.test_amd_iommu_route -v
python3 -m unittest tools.tests.test_driver_quiesce tools.tests.test_driver_dma_mapping tools.tests.test_driver_fence tools.tests.test_driver_workqueue tools.tests.test_driver_irq_sync tools.tests.test_driver_firmware tools.tests.test_riscv_iommu_queue tools.tests.test_riscv_iommu_watch -v
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

## 下一阶段

1. 固件目录的系统级部署与启动交接、可选线程化 IRQ，以及工作队列资源/故障策略。
2. 在 AMD-Vi 被动绑定与完整 aperture claim 基础上补齐剩余 RAM/固件保留来源，基于显式 PCI 集合占用和 IVMD 约束补齐全部 alias peer 的停止/归属协调、硬件 PTE/Unity/Exclusion 策略，再实现翻译、失效完成、故障处理及失败保留/恢复协议；扩展 IOMMU 地址空间能力与共享 worker 策略。被动绑定和只读快照均不能直接转为硬件启用入口。
3. 在已有每设备实例、每文件 adapter 所有权和固定 resident RAM 底座上实现 BO 预算/迁移/驻留策略、GPUVM、上下文与请求队列，明确 VMO/请求/原生 fence 的引用关系；补齐 master/auth 和共享访问权。
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

2026-11-14 保留路由对象提交：`67d67cc70`。独立导出的待提交树中 115 项主机回归通过；新增 9 项测试方法、20 个场景以生产 core/PCI、AMD owner/probe/scope 与 IVRS 代码配合 ASan/UBSan，验证固件路径与有效 Range、原/alias 两条路径及各自 IVMD、分配失败、固件歧义、快照变化、核心注销、client/alias/anchor 替换、控制器移除和两份句柄跨解绑/重绑的最后释放。检查每个路由操作均无配置写入、不添加依赖、最终元数据引用和分配回到基线。提前销毁 detached 上下文的负控触发 ASan use-after-free，省略 alias 路径校验的负控触发覆盖范围断言。

保留其他工作的工作区中，x86_64 双核 embedded 与 generic、AArch64 bringup embedded 内核构建通过；113 个实例与 32 个驱动组件清单检查通过。按新实例参数直接启动双核 x86_64 embedded/generic 内核，退出码 0，匹配全部预期标记，包括 `AMDVI_ROUTE: PASS route=1 policy=1 scope=1` 和 `2/2 configured CPUs online`。这里验证 QEMU 的 IVRS → 保留路由对象 → PCI 路径与只读绑定快照，RngValid=0 的固件路径；有效 Range、alias 和解绑后的资源保留由主机模型验证。没有提交 EDU DMA，没有启用 AMD-Vi 翻译或 GPU 加速。

完整 `tools/a20 test smoke-amdvi-route-x86_64` 已重试，仍在 Native `ufsd` 编译处因 `ARCH_TIMER_FREQ` 未定义退出，不能将直接启动结果记为完整 CLI/用户态门禁通过。文档行号引用检查通过；导航检查仍报告其他工作中的 7 个未索引页面（包括新增 LLVM 文档），本轮六份文档未增加导航/链接错误。日志保存在本地 `.kernel-build/amdvi-route-review/`，不作为长期接口契约或实机翻译证据。

2026-11-14 requester 集合占用：通用 PCI 接口提交 `d12391173`（隔离树 63 项相关回归通过），AMD-Vi 接入提交 `b823664a8`（完整隔离树 128 项主机回归通过）。新增 8 项 PCI 测试覆盖输入复制/去重、完整 ID 位、同 owner 排斥、集合原子失败、分配失败、跨注销 owner pin、最大集合，以及各 64 轮真实线程的重叠竞争和注销竞争；新增 5 项 AMD 测试方法覆盖原/alias/sibling 竞争、BME/用户所有权/读取失败、分配失败和跨 BDF 替换/控制器重绑保留。host 后端替代硬件和调度器，使用生产 core/PCI/AMD/IVRS 代码与 ASan/UBSan。省略 PCI 重叠检查、省略 alias DMA 键两个负控均触发行为断言；路由操作配置写入计数不变。

参数传递修正提交 `423e4ea94`：`machine.extra_qemu` 保留 argv 边界及字面量，Make 查询不执行参数中的表达式。独立提交树的 2 项实际 Make 查询回归通过；当前工作区 216 项 CLI 回归通过（1 项因缺少 arm-none-eabi 工具链跳过）。114 个实例及 32 个驱动组件清单检查通过。

保留其他工作的工作区中，x86_64 双核 embedded 与 AArch64 bringup embedded 内核构建成功，按新实例直接启动 embedded QEMU 获得全部标记并以 0 退出。完整 `tools/a20 test smoke-amdvi-requester-x86_64` 首次已越过此前的 `ufsd` 编译缺口，但因内核参数被拆成多个 argv 失败；修正参数传递后重跑，generic 内核、开发镜像构建与客户机测试均通过，退出码 0，7 个预期标记全部匹配，包括 `AMDVI_REQUESTER: PASS reserve=1 conflict=1 release=1`。客户机确认 `2/2 configured CPUs online`。本次完整门禁使用共享工作区中其他工作的 fscompat 适配改动，这些文件未纳入上述提交；隔离提交树的驱动主机测试不依赖这些改动。

QEMU 验证同一个 EDU requester 的软件占用、冲突和释放后重试，没有执行 EDU DMA、AMD-Vi 翻译或 GPU 加速。alias 竞争及解绑后的持续占用由主机模型验证；原有 RISC-V provider 和 raw 配置写入未统一接入新 namespace。全部 alias peer 的停止/归属协调、IVMD 权限与硬件翻译/故障恢复仍待实现。文档行号引用检查通过，导航仍有其他工作的 7 个未索引页面，本轮未新增链接/导航错误。本地日志位于 `.kernel-build/pci-requester-review/`。

2026-11-14 不可变 DMA policy：通用接口与 managed 映射入口提交 `7d5d736ae`，其独立提交树 129 项回归通过。新 harness 使用独立的逐字节 oracle 检查重叠权限/identity，覆盖 access/flags 变化、空规则、1024 条倒序输入、UINT64 末端、输入复制和边界错误；生产映射代码另验证 compact PFN 转物理地址、部分页/后续页拒绝时零 provider 调用和零 pin、方向权限、identity 成功/失败、回滚超时及 detach 重试保留，provider release 期间仍可访问 policy。profile 和映射代码配合 ASan/UBSan，硬件翻译与 frame/调度器后端为替代实现。

AMD 转换提交 `54959a1ba`，最终隔离树 132 项主机回归通过。新用例覆盖 IR/IW/Unity 合法组合、禁止映射、原/alias 与交叠记录、替代 IVHD 记录保留、ExclusionRange/保留 flags/非法 Unity 拒绝、两阶段分配失败、快照变化，以及源表解除映射/复用后 policy 仍独立。省略整批预检、省略 identity 返回地址核对、忽略 ExclusionRange 三个负控均触发行为断言，未以编译失败代替负控；路由转换的配置写入计数不变。

保留其他工作的工作区中，x86_64 双核 embedded、AArch64 bringup embedded 内核构建通过；完整 `tools/a20 test smoke-amdvi-policy-x86_64` 完成 generic 内核与开发镜像构建、QEMU 测试，退出码 0，8 个预期标记全部匹配，包含 `AMDVI_POLICY: PASS create=1 retained=1`，客户机确认 `2/2 configured CPUs online`。115 个实例与 32 个驱动组件清单检查通过。工作区同时包含并行 LLVM 构建支持等改动，本轮提交未打包其他工作的未提交变更。

QEMU IVRS 没有 IVMD，本轮启动验证只证明空 policy 的构造、保留和集成，不证明有效 IVMD 的硬件执行；没有提交 EDU DMA，没有启用 AMD-Vi 翻译或 GPU 加速。带 policy 的通用 mapper 已检查请求权限/返回地址，但实际 PTE 权限、必需 Unity 映射、完整控制器 Exclusion/alias peer 协调仍须由硬件 provider 实现。旧 domain 不自动获得新 policy。文档行号引用检查通过，导航仍有其他工作的 7 个未索引页面，本轮无新增链接/导航错误。验证日志在本地 `.kernel-build/dma-policy-review/`。

2026-11-14 完整声明 requester 集合：固件查询提交 `e33e0d9ad`，原生接管检查与保留提交 `764192677`。固件阶段隔离树的 20 项 IVRS/路由回归通过；最终隔离提交树 138 项主机回归通过。新增固件用例覆盖 select/range、重复、FFFF 上界、128 项限制、替代 IVHD、跨单元歧义、从目标反查所有 alias、记录顺序无关的传入链，以及任一成员 IVMD 按原始记录只计一次。新增 5 项原生测试方法覆盖查询容量/错误不写缓冲、第三成员缺失/BME/用户归属/读取失败/Range 外、两阶段分配失败、冲突原子性、独立成员引用、成员替换/移除/转交用户后的失败保留、解绑后查询与重绑冲突，以及 sibling 独有约束/Exclusion 拒绝。使用生产 core/PCI/AMD/IVRS 代码与 ASan/UBSan，硬件与任务同步由主机 fixture 替代。

去掉传入 alias 链二次扫描、跳过第三成员 BME、跳过已保留成员校验、仅收原/目标 IVMD 的四个负控均触发运行时行为断言。最终恢复导出源码并核对与代码提交一致；没有以编译失败计为负控。路由 API 测试逐次确认无 PCI 配置写入，未自动解绑其他驱动或新增隐式依赖。

保留其他工作的工作区中，x86_64 双核 embedded、AArch64 bringup embedded 内核构建通过；完整 `tools/a20 test smoke-amdvi-policy-x86_64` 完成 generic 内核/开发镜像构建及 QEMU 启动，退出码 0，9 个标记全部匹配，包括 `AMDVI_PEERS: PASS query=1`，客户机确认 `2/2 configured CPUs online`。115 个实例与 32 个驱动组件清单检查通过，文档行号引用检查通过。导航仍有其他工作的 7 个未索引页面，本轮无新增页面或导航条目。

QEMU 只验证单个 EDU requester 的集合查询/预留和空 policy 集成；多 alias、有效 Range 与带约束 IVMD 由主机模型验证。没有提交 EDU DMA、启用 AMD-Vi 翻译或 GPU 加速。已实现固件声明集合的接管前检查与持续身份占用；跨驱动停止协议、归属变更协调、全部物理 alias/ACS 证明、共享 DTE 属性兼容、实际硬件权限/Unity/Exclusion 与故障恢复仍须实际 provider 完成。验证日志位于本地 `.kernel-build/amdvi-peers-review/`。

2026-11-14 RISC-V requester 登记接入：代码提交 `a7e1149b0`。隔离提交树中 146 项主机回归通过；新增 8 项测试方法、15 个场景使用生产 core/PCI、RISC-V provider、managed mapper 与 policy，以 ASan/UBSan 验证双向冲突、用户幂等、设备替换/移除、原生三阶段分配和三个页表分配失败、BME 读取/清除失败、上下文安装/失效失败的设备/页表/页面/占用保留、最后 put 后重试。硬件寄存器、队列完成、frame 与调度器由主机后端替代；其中 64 轮真实线程竞争验证用户 domain 与通用 lease 只有一个获胜。另一个线程测试在主线程持有 discovery 时完成 native shutdown，检查回调不等待该锁。

跳过共享登记、失效失败时丢弃 lease、成功 detach 时提前释放、deactivate 中等待 discovery、登记前清除 BME 的五个负控均触发运行时行为断言；导出源码随后恢复并与代码提交核对一致。旧队列 fixture 已补充其发布/lease 替代后端，真实 PCI 登记的集成由新增 fixture 验证；没有以 stub 的返回值代替共享登记测试。既有 `test_udriver_mmio_rollback_host.py` 脚本通过。x86_64 双核 embedded、AArch64 bringup embedded 与 RISC-V `ABI=both` embedded 内核最终构建通过。

最终 `tools/a20 test smoke-driver-iommu-native-riscv64` 完成 generic 内核/开发镜像与 QEMU 启动，退出码 0，3 个标记全部匹配，包括新增 `DRIVER_IOMMU_REQUESTER` 和既有真实跨页 EDU DMA、unmap 后 fault、隔离/watch 的 PASS。这证明 QEMU RISC-V 后端的实际 DMA 路径与共享软件占用集成；失败保留/锁顺序来自主机模型，不表示物理热插拔、控制器重绑、实体 GPU 或 AMD-Vi 翻译已验证。

`make smoke-iommu-multidevice` 也完成开发镜像构建与双 EDU 用户态测试并以 0 退出，7 个预期标记全部匹配：两个 domain 已 claim、dev0 fault、dev1 unaffected、domains released、UEDUD PASS、IOMMU enabled 与正常 power-off，未出现禁止标记 `UEDUD: dev1 affected`。这覆盖实际 `udriver` 首次 claim/BME enable、故障与 release 协议，不能代替实体平台或任意用户并发/归属变更验证。

115 个实例与 32 个组件清单、文档行号引用检查通过；导航仍有其他工作的 7 个未索引页面，本轮未新增文档页面。隔离导出仅包含本轮代码，构建/启动使用保留其他改动的共享工作区；`udriver.c` 中既有文档迁移链接未混入代码提交，实际 `docs/zh` 迁移保留，文档提交仍按现有 canonical 路径记录本轮差异。原生和用户后端均未获得自动停止 engine、原子用户归属转移或跨 alias 协调；这些仍由调用者/provider 协议承担。验证日志位于本地 `.kernel-build/iommu-requester-review/`。

2026-11-14 可失败解绑：代码提交 `5cbe6e70c`。隔离导出树中 163 项主机回归通过。新增 9 项核心测试方法、13 个场景逐一运行生产 C/Rust 注册表与 core/class/I²C 路径，验证停止失败的 class 离线/调用排空、私有存储/claim/匹配身份/注册引用保留、quiesce 仅一次、显式 retry、部分 driver 退出、consumer/parent 失败、移除期间的新 child/dependency/绑定拒绝、递归退出拒绝、发布清理失败不覆盖原绑定，以及 bus notification 的最后引用保护。真实 I²C adapter 引用使 remove 返回 `-EBUSY`，put 后重试成功。新增 6 项 bus 测试方法、7 个场景用生产 PCI/USB/core 验证 BDF 替换/缺失时保留、USB slot abort 失败或缺少 callback 时保留所有协议存储和 address、remove 失败后的停止阶段保留、重连前 retry，以及已注销 interface 的 publisher 最后释放。硬件、allocator、任务同步为主机替代，启用 ASan/UBSan。

忽略 remove 返回、忽略 consumer 失败、放行退出中的驱动、发布失败后继续下一匹配、PCI 提前替换、忽略 slot abort、重复已完成的 slot abort、丢失通知引用的八个负控均触发行为断言或 ASan UAF；没有以编译失败作为负控。恢复导出源码后与提交逐文件核对；最后只补充了 header 注释，核对去注释后的声明相同。新测试已接入 `check-firmware-model`，该门禁通过 386 项测试及单独的 1 项 sleep ABI 检查；既有 xHCI 源码顺序检查已适配 unregister 的 int 签名，保留原有顺序断言。

最终 x86_64（`DRIVER_REGISTRY_IMPL=rust`）和 RISC-V64/AArch64（`DRIVER_DEPLOYMENT=embedded`、C 注册表）生命周期 QEMU 门禁均以 0 退出，全部原有标记及新增 `DRIVER_REMOVE: PASS retained=1 retry=1 devres=1` 匹配。RISC-V 首次运行状态断言均通过，但重复的合成 remove 错误加上容量故障注入触发控制台 warn/error 限流，缺少一条串口诊断；启动 fixture 改为一次 remove 失败，重复失败边界仍由主机双注册表用例验证，最终原匹配项全部通过。AArch64 默认 generic 首次构建被既有用户态 `external/sbase/id.c` 缺失阻塞；明确指定 embedded 后内核构建和启动通过，不能将此记录写作 AArch64 generic 门禁通过。

完整 `tools/a20 test smoke-amdvi-policy-x86_64` 完成 generic 内核、模块/开发镜像及双核 QEMU 启动，退出码 0，9 个预期标记全部匹配，客户机确认 `2/2 configured CPUs online`。它验证新的注销语义与既有被动 AMD owner、路由、requester、空 policy 和运行时集成，没有启用 AMD 翻译或 GPU DMA。115 个实例、32 个驱动组件清单与文档行号检查通过；导航仍有其他工作的 7 个未索引页面，本轮没有新增文档页面。

这里只证明核心对驱动已报告错误的保留语义、主机替代后端及合成启动行为，不证明所有旧驱动的 stop 都已正确报告硬件状态。普通失败 probe、未发布 USB 枚举清理、关机/kexec shutdown 和真实厂商 DMA/失效超时仍待明确协议与实机证据。独立提交/导出排除工作区其他未提交修改，实际 `docs/zh` 迁移与原有源码文档链接改动保留；验证日志位于本地 `.kernel-build/driver-remove-review/`。


### 失败 probe 清理验证（2026-11-14）

代码提交 `d166313e4`。隔离导出提交树中 171 项主机回归通过；核心新增 7 项测试方法、11 个场景逐一运行 C/Rust 注册表，覆盖无分配选择、未初始化实例、正数错误规范化、deferred 原错误、清理失败的绑定/claim/依赖保留、下一候选拒绝、子设备阻塞后的首次 quiesce、嵌套帧恢复、构造期递归退出拒绝、缺少 remove/未注册设备，以及旧 probe 的自回滚契约。RISC-V 集成另增 1 项方法、2 个场景，使用生产 core/PCI/provider/managed mapper，验证构造失败的非 NULL 清理句柄、页表及 requester 租约保留，失效恢复后才释放；子设备阻塞时保留开放 domain，同时新映射返回 `-ENODEV`，不增加 frame pin 或硬件命令。硬件 MMIO/完成、allocator 和调度器由主机 fixture 替代，启用 ASan/UBSan。

跳过显式清理、漏掉停止状态 admission、提前进入 REMOVING、允许修改错误 probe 帧、放行构造期递归退出、把旧 probe 自动转入 remove、允许 pending-cleanup domain 新映射的 7 个负控均触发运行时断言；编译失败不计为负控。恢复后完整隔离回归通过，并逐文件核对导出树与代码提交一致。

`make check-firmware-model` 通过 393 项及单独 1 项 sleep ABI 检查；115 个 instance、32 个 driver component 和 193 个 smoke case 审计通过。x86_64 Rust 注册表和 RISC-V64/AArch64 C 注册表 embedded 生命周期 QEMU 门禁均以 0 退出，原标记及 `DRIVER_PROBE_CLEANUP: PASS managed=1 rollback=1` 全部匹配。`tools/a20 test smoke-amdvi-policy-x86_64` 完成 generic 内核/模块/开发镜像与双核 QEMU，9 个标记匹配。AArch64 embedded 结果不代表其 generic 用户态依赖问题已解决。 `tools/a20 test smoke-driver-iommu-native-riscv64` 也完成 generic 内核/开发镜像和 QEMU，3 个标记全部匹配，包括 requester 冲突/释放、真实跨页 EDU DMA、只读映射、unmap 后 fault、detach 与 watch。失败 probe 的故障注入由上述主机集成验证，不能将正常 QEMU DMA 路径记作硬件停止超时证据。

这里只证明显式协议和所列模型/启动路径。旧硬件驱动不会自动拥有部分初始化安全的 stop；实体 GPU、AMD-Vi 翻译、关机/kexec 的可失败 shutdown 和未发布 USB 枚举清理仍须独立完成。文档引用检查通过 540 条；导航仍有原工作区 7 个未索引文档，无新增链接/锚点错误。实际 `docs/zh` 迁移与其他工作区修改保留，验证日志位于本地 `.kernel-build/probe-cleanup-review/`。


### USB 部分初始化清理验证（2026-11-14）

USB 代码提交 `5ed8f02e6`，既有 HID 查询导出修复 `866b6e6d2`，QMP/热插拔证据门禁修复 `801343055`。隔离导出树通过 186 项不同的主机测试方法；执行列表中 PCI resource 的 26 项重复选择了一次，实际执行 212 次，全数通过，未把重复执行计为新增覆盖。新增 USB 8 项方法、25 个场景使用生产 core/PCI/USB 代码验证已知部分 slot 的交还、显式/隐式地址、无 token 错误、正数错误、EP0 MPS/四阶段 descriptor/config/control 失败、分配失败、无 abort 回调、预分配 owner OOM、地址耗尽、停止失败的端口保留、三次连接预算、部分发布后的停止/解绑顺序和 remove 重试。类发布后端、硬件、分配器和同步由主机 fixture 替代，启用 ASan/UBSan。部分发布用例通过显式 ADD observer 先绑定首个接口，再注入后续发布失败，检查停止未确认前不释放其 devres。隐式地址同号测试同时覆盖正常释放与失败回滚，确保另一控制器的软件预留仍在。

xHCI 另增 1 项方法、9 个命令场景，执行生产 alloc_slot 回调，以替代命令后端验证 Enable/Address 错误、fatal、非法/缺失 token 和成功路径；Address 失败不自行忽略第二个 Disable Slot。模块边界测试实际编译 NVMe、PS/2、r8169、xHCI、USB HID/hub/storage 共 7 个包装器并核对全部未定义符号可解析。8 个 USB 行为负控与 1 个导出审计负控均按预期失败；恢复后重新执行完整隔离回归并逐文件核对源码与代码提交一致。

首次 `make smoke-usb-hotplug-x86_64` 的 generic 内核/镜像构建和 slot 释放正常，但 HID 因缺少 `fbcon_active` 导出未加载，门禁失败；修复已有只读查询后通过。逐段检查又发现旧 gate 只要求首次 ready/一次释放，BOOT REPORT 重放可伪造第二次枚举，而且 QMP greeting 被当作命令响应会导致最终 device_add 未确认就断开。现以请求 ID 匹配响应，key/action pump 共享握手；post-check 排除报告重放，要求真实拔出后的 address=1 和第二次 ready。最终严格热插拔门禁以 0 退出，日志明确显示首次 port 5、拔出释放、再于 port 6 分配 address=1 并重新 ready，均发生在报告重放之外。`make smoke-usb-x86_64` 的键鼠就绪、中断完成和真实按键 code=30/value=1 匹配；`make smoke-usb-hub-x86_64` 验证嵌入式共享实现的 hub 下游键盘/鼠标枚举。以上不是实体设备停止超时证据。

门禁工具新增 5 项测试：真实 Unix socket peer 注入 greeting、event、无关响应、capabilities 拒绝并延迟最终响应，确认连接保持到匹配回包；另拒绝只由重放组成的 replug 记录及共享槽位忙时读取旧日志。QMP/重放/旧日志的 3 个负控均触发行为失败。Python 负控恢复后清除导出树字节码缓存，避免同秒等长替换残留的 pyc；隔离树 18 项工具回归最终通过，源码与门禁提交逐文件一致。

`check-firmware-model` 通过 402 项及单独 1 项 sleep ABI 检查，最终 `check-pc-drivers` 通过 58 项；115 个 instance、32 个 driver component、193 个 smoke case 审计通过。AArch64 embedded 共享驱动构建通过；一次并行 QEMU 被共享槽位阻止，旧解释器错误地用旧日志显示 PASS，该次不计启动。串行重跑实际启动并匹配所有生命周期标记；解释器现在对状态 75 直接返回，不执行 post-check 或旧日志认证。这不代表 AArch64 USB 设备、generic 用户态或实体 GPU 已运行验证。

公开结构布局/ABI 9 保持不变。未知 Enable Slot 结果、HCD 自身 start/probe、fatal 控制器恢复、scan/unregister 的通用并发协调及厂商 GPU 停止/失效仍有后续工作；已交还的端口对象不会以普通错误码为由提前释放。文档引用检查通过 540 条，导航仍是工作区既有 7 个未索引页面。实际 `docs/zh` 迁移和其他源码修改保留；本地日志位于 `.kernel-build/usb-enumeration-review/`。


### 提前退出准备验证（2026-11-14）

代码提交 `6a53f5b55`。隔离导出树中 196 项不同的主机测试方法通过。核心新增 3 项方法、4 个场景逐一执行生产 C/Rust 注册表和 core/class：检查显式选择/阶段拒绝、登记 OOM、部分初始化、失败 probe 原错误保留、准备动作 LIFO、前一动作失败时不调用后一动作、正数错误规范化、逐项成功记录、准备失败期间的 class 离线/在途调用保留、claim/child/dependency 拒绝，以及后续 consumer/remove 失败时不重复成功准备。quiesce 与 remove 仍按原阶段执行，成功后引用、存储和资源回到基线。RISC-V 另增 1 项方法，运行生产 mapper/provider/PCI，检查 PREPARING 拒绝新 DMA pin/命令和 PCI lookup，旧 domain 可安全 shutdown，requester 租约在最终 put 前仍保留。

xHCI 新增 6 项方法、12 个场景，执行生产 wait、xfer admission、prepare、remove 和 abort_slot，配合真实 core/USB 图；HCD 初始化、MMIO、PCI、IRQ 排空与分配器由 host 替代。注入 command/status 全 1、RUN 写回失败、HALT/RESET/CNR 不完成、PCI 读取和 BME 清除失败、active transfer，以及模拟 start 返回错误/未取得控制权；检查 prepare 失败前不解绑 child、不释放 ring/controller，IRQ 撤销只一次，修复后继续；child 后续失败时不重复硬件准备，已确认停止后不发送新的 Disable Slot。此处的失败初始化由替代 start 构造，不是对每个真实固件/启动错误阶段的硬件注入。ASan/UBSan 和分配存活计数启用。

跳过准备、重做成功准备、漏掉 PREPARING admission、接受正数准备结果、跳过 HALT、跳过 BME 读回、忽略 active transfer、向停止的控制器继续提交 slot 命令的 8 个负控均触发运行时断言；编译失败不计为负控。恢复后执行完整隔离回归，并逐一核对 14 个导出源码路径与提交一致。

`make check-firmware-model` 通过 411 项及单独 1 项 sleep ABI 检查，`make check-pc-drivers` 通过 64 项；115 个 instance、32 个 driver component 和 193 个 smoke case 审计通过。x86_64 Rust 注册表、AArch64/RISC-V C 注册表 embedded 生命周期 QEMU 均实际启动，以 0 退出并匹配新增 `DRIVER_REMOVE_PREPARE: PASS partial=1 before_remove=1`。没有把共享槽位忙或旧日志当作启动通过。

强化的 `make smoke-usb-hub-x86_64` 使用 embedded、单核、显式 `driver.runtime_test=1 driver.xhci_remove_test=1`。真实 xHCI/hub/键鼠初次枚举后，核心注销整个控制器驱动，确认 BME 清零及旧 child/class 句柄已离线，再重新登记静态驱动、重置控制器并枚举新 child；`DRIVER_XHCI_REMOVE: PASS prepare=1 stopped=1 children=1 rebind=1` 与原 hub/键鼠、资源和运行时标记全部匹配。旧 child metadata 和 class 引用保留到检查完成，未以相同地址的新对象冒充旧引用恢复。`make smoke-usb-hotplug-x86_64` 的 generic 内核/模块/开发镜像和严格 live unplug/replug 检查也通过，验证新增模块导出与正常拔插兼容。

这些证据覆盖核心显式契约、主机失败模型和单核 QEMU 控制器退出/重新绑定。没有验证实体 stop 超时、任意跨 CPU poll/scan/unregister、接口单独退出、Configure Endpoint 的独立 ring 错误回收或同步 bulk 调用者缓冲区保留；也未将关机/kexec shutdown 变成可失败接口。公开布局/ABI 9 保持不变。文档行号检查通过 540 条；导航仍为工作区既有 7 个未索引页面，实际 `docs/zh` 迁移和其他改动保留。本地日志在 `.kernel-build/remove-prepare-review/`。


### USB 拓扑与锁外回调验证（2026-11-14）

代码提交 `6ec6c9685`。新增 13 个主机测试方法、18 个场景，运行生产 core/USB 图及 xHCI dispatcher、prepare、abort 和 EP0 admission；任务调度、MMIO/PCI/IRQ 和命令后端由 host 替代，启用 ASan/UBSan。pthread 屏障验证 scan/enumerate 期间并发 unregister 等待，而进度 try 不等待；启动上下文和任务嵌套均保持普通 probe 与 discovery 的不同策略。另覆盖相同 HCD 重入、未发布 start 借用上游、注册容量预留、回调增删/压缩注册表、stopping 祖先、reset 后状态改变、同控制器退役暂停且不扣尝试次数。锁外 callback 的全局/每 slot 保留、其他 slot 独立清理、自释放 URB、停止记录重试，以及停止后 EP0/interrupt 不提交均有行为断言。

14 个负控分别撤掉 try 规则、poll 忙时标记释放、scan 协调、活动下游保护、start 容量预留、slot 退役暂停、reset 后 stopping 检查、退役不扣预算、controller/slot 回调保留、自释放 URB 的标量快照、已停 slot 的重试记录、control/interrupt admission，全部触发运行时失败；编译失败不计入。恢复后的完整隔离回归通过 209 个不同方法，最后再跑 16 项 USB/xHCI 定向测试，并核对 14 个导出路径与代码提交一致。

`make check-firmware-model` 通过 424 项及单独 1 项 sleep ABI，`make check-pc-drivers` 通过 77 项；115 个 instance、32 个 driver component、193 个 smoke case 审计通过。`make smoke-driver-lifecycle DRIVER_DEPLOYMENT=embedded` 实际启动 RISC-V64 并匹配生命周期/失败退出标记。`python3 tools/smoke.py smoke-usb-hub-x86_64 --nr-cpus 2` 在真实 guest 确认 2/2 CPU online，并两次通过 hub/键鼠、旧句柄离线、BME 停止检查与 `DRIVER_XHCI_REMOVE: PASS prepare=1 stopped=1 children=1 rebind=1`。generic 的 `make smoke-usb-hotplug-x86_64` 通过严格 live 拔出/重插/地址复用检查；`make smoke-usb-x86_64` 通过 HID 中断和实际按键输入检查，覆盖新增模块导出及 class 进度入口。

这些证据覆盖受协调的拓扑变更和所列线程交错，不等于任意 SMP 压力或实体硬件停止超时验证。interface 驱动独立退出、Configure Endpoint 的独立 ring 错误、同步 bulk 调用者缓冲区和可失败 shutdown 仍待完善。公开结构布局与 ABI 9 不变。文档引用检查通过 540 条；导航仍有工作区既有的 7 个未索引页面，没有新增链接/锚点错误。实际 `docs/zh` 迁移及其他改动保留，本轮本地证据位于 `.kernel-build/usb-concurrency-review/`。


### 接口 endpoint 交还与 ABI 10 验证（2026-11-14）

PCI 修复提交 `9e65e222d`，USB/ABI 实现提交 `be7bca2ec`。新增 9 项 endpoint 主机测试方法、14 个命名场景，运行真实 driver core/USB、HID probe/quiesce/remove、hub remove、xHCI 配置/释放/收集/提交及锁外回调；调度、硬件命令和寄存器是替代后端，启用 ASan/UBSan。覆盖部分配置错误和模糊结果保留、首次 submit/SET_INTERFACE 失败、独立退出失败后的 URB/模块所有权、重新绑定、回调仍在途、其他 endpoint 保持运行、Halted/Error 转移、Stop/Drop 分段读回与重试、重复配置、在线/失主/缺少 callback 拒绝，以及下游 HCD 已注销后的 hub remove 重试。PCI 新增 1 项方法、内核绑定/只读绑定/user_owned 三种所有者，确认查询前后配置空间、BME、BAR、资源和 IRQ 路由均未改变。真实 drvctl parser 接受 ABI 10、拒绝 ABI 9 与其他不匹配 descriptor。

13 个负控撤销 SET_INTERFACE 失败检查、已持有 PCI 查询保护、endpoint/slot 回调保留、completion/submit 关闭、Stop/Disabled 读回、停止重试记录、Configure Endpoint 错误所有权、HID probe 显式清理和 HID/hub remove 错误保留，均触发运行时断言或 ASan；编译失败不计入。恢复后隔离导出树通过 219 个不同方法，再通过 12 项 endpoint/encoding 定向回归，19 个导出源码路径逐一与提交核对。

最终 `check-firmware-model` 通过 434 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 86 项；115 个 instance、32 个 driver component、193 个 smoke case 审计通过。双 CPU 的 embedded hub guest 确认 2/2 CPU online，匹配 `DRIVER_USB_INTERFACE: PASS hid=1 hub=1 siblings=1 rebind=1` 和 `DRIVER_XHCI_REMOVE: PASS prepare=1 stopped=1 children=1 rebind=1`：在根控制器保持绑定时单独注销/重绑 HID，然后 hub，再执行整个控制器退出。旧 class/child 引用保留用于离线检查，新接口重新选择 altsetting、配置 endpoint 并提交 URB。此处没有注入重绑后的真实按键或对实体 USB 包序列做分析。

ABI 10 generic 内核、模块和镜像由严格 USB 热插拔与 HID 实际输入门禁构建/运行；RISC-V64 embedded 生命周期实际启动并匹配失败退出/注册表标记。这些正常 QEMU 路径不等于实体停止超时或任意 SMP 压力证据。storage 静态实例、同步 bulk 超时后调用者缓冲区、拒绝 SET_INTERFACE 设备的安全恢复、可失败 shutdown 和厂商 GPU 硬件恢复仍待推进。文档行号检查通过 540 条，导航仍是既有 7 个未索引页面；实际 `docs/zh` 迁移及其他改动保留。本地证据位于 `.kernel-build/usb-endpoint-review/`。


### 独立 DMA buffer 与 bulk 暂存验证（2026-11-14）

代码提交 `07b735bc7`。新增 DMA buffer 的 7 项测试方法、8 个命名场景，运行生产 core/registry/refcount/allocator 代码与 ASan/UBSan；PFA、CPU alias 和架构就绪由主机后端替代。覆盖小请求/强对齐、8 MiB 上限、物理地址 0、整个分配块的 mask 检查、compact PFN 与真实分配阶数回收、参数和元数据/页 OOM、不可用 alias/对齐、分配期间停止/注销/mask 改变、跨 owner 注销的最后引用，以及 8 个真实线程各 10000 次引用交接。另一个线程在 lifecycle 已被持有时完成分配，验证 active class 不会因隐式获取该锁而阻塞退出。

新增 bulk 的 7 项主机方法使用真实 xHCI 提交、ring、prepare/abort/remove、USB/core 与上述 buffer 实现，验证 IN/OUT/零长/64 KiB 私有地址、完整成功才拷回、短包/非法 residue/错误不发布输入、无效/关闭/忙状态拒绝，以及复制期间退出后的提交前复查。事件等待、MMIO/PCI/IRQ 和 DMA 由故障后端替代：超时后立即释放调用者缓冲区，再模拟迟到 DMA 写入仍保留的暂存对象；真实 unregister 的 prepare 在 HALT 无法确认时失败，原对象和引用数不变，重试成功只交还 controller 引用，最后外部 put 才释放。没有在实体控制器注入停止超时。

15 个负控分别破坏实际阶数回收、compact PFN 换算、owner pin、对齐、整个块的 mask、架构就绪、buffer get、分配后 owner 检查、无 lifecycle 等待、私有 DMA 地址、复制后退出检查、prepare 失败保留、remove 最后释放、错误不拷回和短包拒绝，全部触发运行时断言/ASan；编译失败不计入。恢复后隔离导出树通过 233 个不同测试方法，工作区再通过 18 项 buffer/bulk/IRQ/endpoint encoding 定向检查。提交前逐一核对 17 个导出源码路径；最后仅移动 smoke case 的说明注释，门禁表审计仍通过。

`make check-firmware-model` 通过 448 项及单独 1 项 sleep ABI，`make check-pc-drivers` 通过 100 项；115 个 instance、32 个 driver component、194 个 smoke case 审计通过。双 CPU embedded hub guest 通过独立 HID/hub 与整体 controller 的退出/重新绑定；新增 `make smoke-usb-bulk-x86_64` 确认 2/2 CPU online，在专用 8 MiB 空白盘读写、比对、恢复，并匹配 `DRIVER_USB_BULK: PASS read=1 write=1 verify=1 restore=1`。两者均匹配真实 PFA 的 `DRIVER_DMA_BUFFER: PASS align=1 owner_refs=1 lifetime=1`。generic 的严格热插拔与 HID 实际输入门禁通过，覆盖新 API 的模块导出/加载。

AArch64 的 `ARCH=aarch64 ABI=linux BRINGUP=0 DRIVER_DEPLOYMENT=embedded RAMFS_USER=1 kernel-only` 构建成功；QEMU virt/cortex-a57 从该构建的 raw `kernel.bin` 启动，传入 `-append driver.runtime_test=1`，实际匹配 DMA buffer、资源和 runtime PASS 并正常关机。完整 dev-build 仍受既有 `user/external/sbase/id.c` 缺失阻断，不计为成功；先前用 ELF 启动未运行到该 runtime，亦不计入证据。raw 启动的单 CPU 结果不代表跨架构 USB 或任意 SMP 压力已验证。

这些改动提供显式存储所有者及首个硬件使用者，不提供 GPU BO/驻留、GPUVM、IOMMU 高阶映射或停止证明。每 controller 的预分配占用为 64 KiB；storage 实例/独立退出、BOT 恢复、变长 bulk/多 TRB、拒绝 SET_INTERFACE 设备的恢复和可失败 shutdown 仍待推进。公开布局与 ABI 10 保持不变。文档行号检查通过 540 条；导航仍有迁移工作区既有 7 个未索引页面。实际 `docs/zh` 迁移及其他改动保留；本地证据位于 `.kernel-build/dma-buffer-review/`。


### 高阶 buffer 的原生 domain 映射验证（2026-11-14）

代码提交 `9bbd5ae1a`。新增 9 项 buffer mapping 主机测试方法、11 个命名场景，运行生产 core/registry、buffer allocator/refcount、mapper 和 policy，启用 ASan/UBSan。PFA、cache、IOVA/provider 为替代后端。覆盖物理与 IOVA 不同且各页不连续、allocation owner 与目标 device 不同、原 owner 注销后保留整块存储、33 字节强对齐对象只暴露一页、末页 padding 的预检拒绝、未映射 backing 的限制不误拒绝、TO/FROM/BIDIRECTIONAL cache 交接、分配/准入/IOVA/identity 错误、当前页残留、部分 unmap 超时及 pthread 的 map/shutdown 串行。所有高阶尾页的 frame_get/put 均会触发断言，不能以虚构尾页引用掩盖回收错误。

实际 RISC-V provider 的 requester 集成新增 2 项方法、3 个场景；生产 allocator/core/PCI/provider/mapper 组合使用仅头页持引用的 order-4 PFA 模型，验证成功、map 失效失败和 unmap 失败。native IOVA 借用通用 mapping 的存储，不另 pin RAM；用户态旧 API 的独立 order-0 pin/失败 quarantine 回归继续通过。旧 map/unmap 传负 native owner 被拒绝，不改翻译或命令计数。失败时保留高阶块和 requester/domain，失效恢复后才按原 PFN/order 回收。

20 个负控分别撤销 buffer 引用/最后释放、提前释放、错误地 pin/put 尾页、暴露整个对齐 backing、跳过完整页预检、错误 cache alias/物理地址、跳过 identity 校验、丢弃清理句柄、移除 managed map 锁、提前 CPU sync、遗漏 domain 计数、provider 二次 pin/错误 pinned 记录/错误 put、旧 map/unmap 允许 native owner，以及移除用户态 pin，均触发运行时断言或 ASan；编译失败不计入。恢复后的隔离导出树通过 244 个不同方法，之后 22 项 provider/buffer 定向检查通过。最终 18 个源码/测试配置路径逐一与提交核对；末次调整仅补公开契约注释、fixture 分类和门禁/实例配置。

主门禁 `check-firmware-model` 现纳入通用映射、RISC-V requester/queue 和新 buffer 组合测试，通过 473 项及单独 1 项 sleep ABI；`check-pc-drivers` 通过 109 项。115 个 instance、32 个 driver component、194 个 smoke case 审计通过。双 CPU x86 embedded hub 与 USB bulk guest 均实际匹配 `DRIVER_DMA_BUFFER_MAP: PASS pages=2 retained=1 unmap=1`，运行真实 PFA/direct domain 的两页映射、caller put、owner 注销和最后 unmap；原 endpoint/controller 退出和专用盘读写、比对、恢复也通过。USB 本身仍走 identity-DMA 私有暂存路径，不能把它记为 IOMMU 传输。

`tools/a20 test smoke-driver-iommu-native-riscv64` 的最终 manifest 显式选择 embedded；构建普通 dev 内核/镜像并启动同一配置，避免 make 额外覆盖只改变构建而运行旧配置。实际 guest 匹配全部 5 个要求标记，包含 `DRIVER_IOMMU_BUFFER: PASS high_order=1 iova=1 dma=1 retained=1`：64 KiB 对齐的 PAGE_SIZE+33 字节 buffer 只映射两页，先交还 allocator caller 引用，EDU 经非恒等 IOVA 跨页读入并写回，再逐字节检查传输区及其余映射页内容，确认 DMA/BME 停止后解除映射。原有物理不连续页跨页 DMA、只读 PTE/TR_REQ、unmap 后非法 IOVA fault、watch、requester 冲突/释放和 detach 仍通过。构建成功但未运行新 guest 的尝试不计入这些证据。

这补齐高阶分配所有者到原生 domain 的生命周期衔接，不提供 GPU BO/驻留、GPUVM、共享访问排他、IOMMU 自动选择或硬件停止。末页 padding 是明确暴露的页覆盖；GPU/用户 ABI 若需要字节级访问限制，不能依赖这项页级接口。RISC-V provider 仍限每 domain 64 页；实体失效超时和厂商 GPU 硬件恢复未验证。ABI 10 不变。文档引用检查通过 540 条；导航仍是迁移工作区既有的 7 个未索引页面，实际 `docs/zh` 迁移和其他改动保留。本轮本地证据位于 `.kernel-build/buffer-map-review/`，成功的 RISC-V 启动记录为 `qemu-iommu-embedded.log` 与 `iommu-guest.log`。


### VMO 页快照与 DMA 导入验证（2026-11-14）

MM 提交 `3c6f93fcc`，驱动/首个使用者提交 `1f981757b`。VMO 生命周期 Rust 单测共 7 项，新增 4 项覆盖整批验证后发布、第二页缺失或非法 PFN 时输出/引用不变、ANONYMOUS/PAGED/SHARED_ANON、physical/unknown 拒绝、零/空/越界参数、4096 页上限及合法 PFN 0。另运行既有 VMO page/materialize/resize/destructor 的 14 项 Rust 事务回归。snapshot 的上限来自 C 定义的窄查询，Rust 不维护另一份生产上限。

新增 `test_driver_dma_vmo` 共 12 个方法，包含上述 7 项 Rust 单测入口及 14 个命名 C 场景。C fixture 链接生产 Rust lifecycle 对象，提取生产 MM 锁/slot/PFA 原语，编译实际 DMA adapter/mapper/policy；C 部分启用 ASan/UBSan，Rust lifecycle 原样编译。源 resize/destructor 副作用、PFA、provider/cache 是模型，不把它们记为执行了实际 Rust resize/destructor。覆盖 source 元数据/原始页引用退出后的 mapping 存活、页 1–2 的非恒等且不连续 IOVA、稀疏/分页源、OOM/policy/closed/架构拒绝后的临时引用回收、当前失败页的清理句柄、部分 unmap 保留与 cache 时机。pthread 在 batch pin 内暂停，另一线程尝试 resize，验证 VMO lock 排斥；释放该锁后 provider 等待 resize 完成仍能映射原帧。模型帧引用计数为原子，避免测试自身引入 PFA 竞争。

14 个负控撤销 snapshot pin、遗漏临时 put、允许 physical、越过范围尾、错误缺页结果、失败前写部分输出、在 pin 前解锁、接受非法 PFN、丢弃清理句柄、错用 first=0、持 VMO lock 调 provider、把 map 错误当成功、失败快照仍 put 及遗漏 paged/sparse 支持，全部触发断言、ASan 或 Rust panic；编译失败不计入。恢复后隔离导出树通过 256 个不同 unittest 方法。最终 16 个源码/测试配置路径逐一与提交核对，两次代码提交合成的树与导出树一致；最后仅修正了 peek 的借用语义说明。

`check-firmware-model` 通过 485 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 121 项；115 个 instance、32 个 driver component、194 个 smoke case 审计通过。双 CPU embedded 的 USB bulk/hub guest 实际匹配 `DRIVER_DMA_VMO: PASS snapshot=1 resize=1 lifetime=1`：原生 VMO 缺页先返回 EAGAIN，物化后导入 direct domain 的页 1–2，再执行真实 resize(0) 和原 VMO 销毁，独立帧仍可访问并在最后 unmap 释放。旧高阶 buffer、USB 读写/比对/恢复、接口及 controller 退出/重绑均通过。generic 内核/模块/镜像的严格热插拔也通过；没有把这些 USB 传输记作使用了 VMO IOMMU 地址。

`tools/a20 test smoke-driver-iommu-native-riscv64` 在实际 guest 匹配全部 6 个要求标记，并另有通用 VMO resize PASS。新增 `DRIVER_IOMMU_VMO: PASS snapshot=1 offset=1 dma=1 lifetime=1` 验证真实原生 VMO 的页 1–2 导入 IOVA，先销毁源 VMO，再由 EDU 跨页读入/写回，核对模式及未传输区域，确认 DMA/BME 停止后解除映射。原高阶 buffer DMA、不连续页、只读权限、unmap 后 fault/watch、requester 归属与 detach 检查继续通过。这里使用实际 MM/Rust/PFA 和硬件翻译模拟器，不等于在实体设备注入了停止/失效超时。

这些接口只提供稳定 RAM 帧和通用映射所有权，不建立 BO 驻留、cgroup charge 转移、VMO extent/CPU 关联租约、GPUVM、进程 context 或任务调度。现有 DRM/GEM/virgl 路径没有因此自动改用原生 domain；未来 GPU 提交须另保留请求/fence、访问权和预算，协调源变动及硬件停止。ABI 10 不变。文档引用检查通过 540 条，导航仍为工作区迁移既有 7 个未索引页面。实际 `docs/zh` 迁移及其他改动保留；本轮本地证据在 `.kernel-build/vmo-dma-review/`，RISC-V 完整 guest 在 `iommu-guest.log`，双 CPU guest 在 `usb-bulk-guest.log` 与 `usb-hub-guest.log`。


### GPU publication 与消费者调用验证（2026-11-14）

代码提交 `d1518f841`。GPU 默认策略保留 device 元数据，新增四个 class publication/access 导出；fbdev ioctl、mmap 安装、后台刷新及 sysfs GPU 内容生成使用一次同绑定的 device/const ops。共享 class rank 枚举修正为先过滤 offline 再计序号，稳定 publication index 查询保持独立。没有增加第二套设备模型，既有 device/driver/vtable 布局及 ABI 10 不变。

新增 12 项主机方法运行生产 core/class/GPU 策略和提取的生产 framebuffer/sysfs 函数，使用 pthread 与 ASan/UBSan；加既有 class binding 定向回归共 13 项通过。覆盖选择与 class lookup 间的 device pin、重复注册/提升/最终回收、旧 publication 不跟随同 device 重绑、解绑等候 active call 且离线 reader 不等 lifecycle、生命周期修改后的成员复查、丢失 provider 撤销时的元数据存活和旧 ops 不再读取，以及 promotion 中刷新/属性的同绑定一致性。21 设备用例包含一个仍在 registry 的 offline publication，并特意把末项设为 2D，避免 unpublish 的末项交换掩盖超过 16 项的 live 3D fallback。host 中 3D 函数只作存在性占位、从不执行；PCI/usercopy/锁及硬件为模型，mmap 仅执行 NOMMU 分支。

19 个负控分别破坏 offline rank 过滤、默认元数据 pin、lookup 临时 pin、独立 publication 引用、旧 publication 准入、active-call 结束、最后引用交还、reader 不等 lifecycle、策略修改协调、缓存能力读取、16 项上限、候选 owning ref 转移、ioctl/mmap 配对 end、刷新设备一致性、sysfs end/设备一致性/提前退出，以及缺失方法检查，均触发断言或 ASan/UBSan/泄漏检查；编译失败不计入。恢复后最终隔离导出树通过 268 个不同 unittest 方法，13 个源码/测试配置路径逐一与提交核对。`check-firmware-model` 通过 497 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 133 项；115 个 instance、32 个 driver component 与 195 个 smoke case 表审计通过。

新增 `make smoke-gpu-access-x86_64` 固定 generic 配置，实际双 CPU 2/2 online，加载真实 virtio-gpu 模块并匹配 `DRM_SYSFS: PASS` 与 `videoplay: presented 10 frames`，覆盖 MMU 的 framebuffer 安装/ioctl/2D 刷新。双 CPU embedded hub guest 匹配 `DRIVER_GPU_ACCESS: PASS publication=1 offline=1 refs=1`，保留旧 endpoint/controller 退出与重新枚举及 VMO/buffer runtime 标记。假 GPU 总线仅匹配本地 driver 身份；最初通配匹配导致其他驱动抢占的失败尝试已修正，不计为成功证据。最终两次 guest 均在共享 QEMU 资源串行协调下运行，成功日志保存在 `.kernel-build/gpu-access-review/gpu-guest.log` 与 `hub-guest.log`。

这些结果证明调用准入、引用和现有 2D 消费者回归，不是图像视觉核验、实体 GPU 加速或设备停止/复位证明。fbdev mmap 返回后仍是裸物理 VMA，文件不固定 GPU；DRM/GEM/virgl 全局对象与旧借用 getter 尚未整体迁移。每设备 BO/context、GPUVM、请求/fence 及映射退役仍为后续工作。文档引用检查通过 540 条；导航仍是迁移工作区既有 7 个未索引页面。文档迁移及其他会话修改保留，本轮完整本地证据位于 `.kernel-build/gpu-access-review/`。


### 驱动 CPU 物理映射事务验证（2026-11-14）

主代码提交 `2f7070da5`。fbdev mmap、旧 `FBIO_MAP_FB` 和原生用户驱动服务已共用 MM 的 eager CPU 物理映射事务：Rust 复用既有 fixed/hint 策略并拥有准入/回滚，C 暴露记录/架构/锁/TLB 原语。修正了 fbdev 持 mm->lock 调用公开 mm_munmap 的递归锁问题、只检查 hint 起点的碰撞遗漏、失败页空路径清理与 TLB 退休，以及用户驱动物理映射的 total_vm/RSS 记账。新视图用既有 VM_NATIVE_CAP 表达独立于 ABI 的 READ/WRITE ceiling；保护变更保留 Device 缓存类型与 dirty 历史。scanout 地址相加前拒绝整体物理范围溢出，fbdev 经 mm_addr_error 解码窄 VA 的负返回，不把它零扩展为成功地址。

新增 12 项方法运行实际 Rust mmap policy、提取的生产 C 桥接、PTE 转换/保护 helper、PT unmap、fbdev MMU mapper/旧 ioctl 分支与 udriver adapter；C 启用 ASan/UBSan，Rust 按生产策略普通编译。CPU 页表安装、分配故障、锁、区间和 munmap 副作用为模型。覆盖物理 0 的旧叶不可覆盖、长度/PA/VA 取值与溢出、32-bit PA/VA 收窄模型、完整 hint 区间、固定覆盖与旧映射删除后安装失败、NOREPLACE、CPU Device/Normal 属性、权限 ceiling 和记账。窄类型模型不等于完整 ILP32 ABI 或 32-bit guest 验证。测试内 3 个负控破坏当前失败页、映射前缀和空路径清理，均须编译成功后触发运行时失败；旧 MMIO rollback 脚本改为同一套测试的兼容入口。既有 lazy mmap 主机脚本仍通过 106 项检查。

另在隔离导出树分别破坏完整 hint 检查、fixed 错误传播、旧叶保护、失败页/前缀清理、TLB finish、RSS/total_vm、ceiling、保护时 Device 属性保留、初始 CPU 属性、目标 PA 宽度、未知属性拒绝及锁外记录分配，共 14 项控制全部触发断言或 ASan/UBSan；编译失败不计入。最终导出树通过 281 个不同 unittest 方法，15 个主代码/配置路径逐一与提交核对；相较负控执行树，只有下述独立测试 oracle 提交的两个文件发生变化，14 项控制所使用的策略、mapper、桥接与消费者源码一致。

独立测试修正提交 `a8cc065bd`：此前 VMO provider/cache hook 用 trylock 判断“任何线程持有 source lock”，在合法并发 resize 时误报。fixture 改为按当前线程记录锁所有权；另一个线程持锁时回调允许通过，本线程持锁则必须在指定 ownership assertion 失败，不能用无条件失败代替该负控。13 项 VMO 方法通过；完整门禁中的那次误判不计为成功。最终 `check-firmware-model` 通过 510 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 146 项。115 个 instance、32 个 driver component、195 个 smoke case 的表审计通过。

双 CPU generic virtio-gpu 2D guest 实际 2/2 online，新增 `fbdev_map_test` 匹配 `FBDEV_MAP: PASS fixed=1 noreplace=1 hint=1 ceiling=1 legacy=1`：匿名邻页内容在 fixed 替换后保留，跨 gap 的 hint 不覆盖邻页，RO view 不扩权，旧 ioctl 使用 NOREPLACE；同时继续匹配 sysfs PASS 和 videoplay 10 帧。embedded 双 CPU hub guest 匹配 `DRIVER_PHYSICAL_MAP: PASS placement=1 protect=1 accounting=1`，实际 MM/Rust/PFA/VMO 组合检查叶和 VMA 的 PCD/PWT 保留、固定覆盖/权限拒绝及计数，销毁视图后才交还源 VMO；旧 GPU publication、USB endpoint/controller 退出/重绑及 VMO/buffer 标记继续通过。这些启动串行使用共享 QEMU 资源；成功日志在 `.kernel-build/physical-map-review/gpu-guest.log` 与 `hub-guest.log`。

本接口借用存储，不取得 frame/backing/device/resource/module 引用，不提供 DMA/IOMMU、停止证明或映射撤销。持久 framebuffer backing、DRM 每设备 BO/context/request、GPUVM 和失败退役仍待实现；没有增加设备模块的用户映射导出或改变 ABI 10。上述模拟器结果不证明实体 GPU 加速、完整 ILP32 运行或远端 TLB 的并发压力。文档引用检查通过 540 条；导航仍为迁移工作区既有 7 项缺失。实际 docs/zh 迁移与其他会话修改保留；本轮完整证据在 `.kernel-build/physical-map-review/`。

### 连续 RAM VMO 与 owning scanout 验证（2026-11-14）

MM 提交 `1c5172d60`，GPU/fbdev 与 ABI 11 提交 `3f150e766`。先分别导出 MM 中间树和最终完整树：前者通过 45 个驱动 unittest 方法及既有 Rust mmap policy 106 项检查，后者通过 296 个不同驱动 unittest 方法。7 项连续工厂与 7 项 GPU snapshot 方法包括实际 PFA split、Rust 引用/最终销毁、virtio producer 的 get/remove/release、fbdev adapter 与 unlocked MM ceiling wrapper；分配/地址、locked mapper、transport/IRQ 等边界仍为模型。独立 PFA geometry/split 脚本和完整 VMO page host 的 14 项 Rust 测试通过；直接 rustc --test 但没有测试体的运行不计入证据。

在隔离完整树逐项破坏 PFA split、清零、尾部交还、专用构造、两层 resize 拒绝、禁止补洞、producer 引用/释放、snapshot 释放及先清零时序、错误引用回收、不可变 kind 检查、offset 对齐、size 溢出、fbdev 临时引用/offset 和初始权限 ceiling，共 18 项负控均编译成功并在运行时失败。首个对齐输入也被范围检查拦截，不能独立证明对齐；fixture 收紧为仍在 extent 内的非对齐小范围后重新验证全部负控，并在恢复后运行上述 296 项回归。29 个代码/配置路径与恢复后导出树及提交逐一核对。

完整 `check-firmware-model` 通过 525 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 161 项；这两次门禁运行于对齐 fixture 收紧前，最终收紧由隔离回归/负控覆盖。初次 firmware gate 因 requester/queue fixture 缺少新工厂声明而失败，不计成功；补齐共同头后，两项完整门禁及相关 fixture 均通过。115 个 instance、32 个 driver component、195 个 smoke case 清单审计通过。

实际 generic virtio-gpu 2D guest 为双 CPU、2/2 online，要求 `FBDEV_MAP: PASS fixed=1 noreplace=1 hint=1 ceiling=1 legacy=1 fork=1`，检查 resident framebuffer 页在 fork 后共享写入，sysfs PASS 与 videoplay 10 帧继续通过。embedded hub guest 也是 2/2 online，新增 `DRIVER_GPU_SCANOUT: PASS vmo=1 unbind=1 split=1 fork=1 pins=1`，生产者完整解绑后以真实 VMO/VMA 引用完成 split、fork、部分 unmap 和 pin 交还；这个合成内核视图尚未 fault 用户 PTE，resident 路径由前一用户程序覆盖。旧 GPU access、物理视图及 USB 生命周期标记保留。

RISC-V embedded 单核 native IOMMU/EDU guest 匹配全部 7 项预期，新增 `DRIVER_IOMMU_CONTIGUOUS_VMO: PASS canonical=1 offset=1 dma=1 lifetime=1`；源 VMO 先销毁，真实 EDU 随后对页 1–2 的 IOVA 跨页往返，确认停止/BME 清除后 unmap。旧 ANONYMOUS VMO、高阶 buffer、不连续页、权限和故障/watch 用例同时保留。三个启动串行运行并观察到最终退出，成功 guest 日志保存在 `.kernel-build/scanout-vmo-review/guest-gpu-retained.log`、`guest-hub.log`、`guest-iommu.log`。

当前文档引用检查通过 540 条；导航检查仍报告共享迁移工作区既有 7 篇未编目文档，本轮没有新增导航/锚点错误。14 篇文档仅投影本轮内容到 canonical 路径提交，保留其他会话的 docs/zh 迁移。完整日志及负控在 `.kernel-build/scanout-vmo-review/`。此证据验证 RAM 引用链，未执行实体 GPU 加速或实际 virtio 解绑后的停止确认；raw 物理视图、DRM 全局对象/每文件身份和厂商 BO/GPUVM 仍需后续开发。

### VirtIO reset 与 GPU 失败退出验证（2026-11-14）

实现提交 `88ec7fadb`。新增 bus helper 沿用 read32/write32，不扩展 transport vtable 或 ABI 11；virtio-gpu 先选择原生失败 probe 管线，保留部分实例，再执行 transport 初始化。quiesce 封闭四条 issuer，未知完成封闭整个 staging/queue，remove 只有 reset 状态读回 0 后才交还 IRQ 和存储；失败保留原绑定/owner，静态实例不允许第二次 probe 覆盖。

16 个新主机方法执行实际 instance/claim、四条 issuer、init/probe/quiesce/remove/release、reset helper，以及既有 C/PFA split/Rust VMO 回收。寄存器/clock/IRQ/core admission 是模型，PCI 分支初始化 fixture 使用回调 transport，不能据此声称已注入硬件超时或执行真实 IRQ 排空。PC module boundary 另实际编译 virtio-gpu 包装器并核对 imports。原生核心的 19 项 remove/失败 probe 方法与相关回归一起运行；隔离导出的最终源码树共通过 312 个不同驱动 unittest 方法。

隔离树中的 16 项编译成功负控逐项破坏 reset 读回、deadline、poll 预算、初始 reset、probe opt-in、静态 owner、closing/fault 准入、超时封闭、used id/index、递归 active 拒绝、失败 reset 的返回，以及过早 storage/IRQ 释放和 owner 丢失，均触发运行时断言/ASan。最初“漏设超时故障标志”的变体逃过测试，因为 oracle 以该标志决定是否检查故障；补上独立的 command_failed 断言后，全部负控重新拒绝，恢复源码后再运行完整回归。11 个代码/配置路径与隔离输入和提交逐一核对。

最终 `check-firmware-model` 通过 541 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 177 项；均在独立超时断言补齐后运行。116 个 instance、32 个 driver component 的清单审计通过。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，没有运行 AArch64 guest。初次 x86 构建因新 bus 源文件缺少 clock API 声明失败，不计成功；补齐原生 driver_hwapi include 后，重新构建及下述 guest 通过。

`tools/a20 test smoke-driver-gpu-stop-x86_64` 显式 embedded、双 CPU，实际 2/2 online；真实 virtio-gpu PCI reset/退出后，旧 class publication 离线，旧 VMA 保留 framebuffer canary，重新登记后得到不同 VMO 并完成真实 flush，匹配 `DRIVER_GPU_STOP: PASS reset=1 unbind=1 retained=1 rebind=1`，旧 scanout/runtime 标记同时保留。内核视图未 fault 用户 PTE；另串行运行 generic 模块双 CPU 2D 门禁，保留 resident framebuffer fork、`DRM_SYSFS: PASS` 和 videoplay 10 帧。成功 guest 日志在 `.kernel-build/virtio-stop-review/guest-stop-final.log` 与 `guest-generic.log`。这两项 QEMU 运行没有模拟硬件拒绝 reset。

11 篇文档仅将本轮内容差异投影到 canonical 路径提交，其他会话的 docs/zh 迁移及工作区改动保留。引用检查通过 540 条；导航仍报告迁移工作区既有的 7 篇未编目文档，没有新增链接/锚点错误。完整证据在 `.kernel-build/virtio-stop-review/`。目前仍是单静态 queue，原始 DRM ops/全局对象、每文件 GPU 身份、外部 GEM backing 的提交/完成引用、多 GPU、模块 unload 和实体厂商 GPU 停止协议尚未因此完成。

### 原生停止凭据与局部 devres 回滚验证（2026-11-14）

新增 `test_class_stop` 的 7 个主机方法编译生产 driver core/class 代码，覆盖显式成功 remove、失败保留与重试、offline/调用排空不等于停止、旧凭据跨重绑稳定、缺少 remove、publication/action 分配失败，以及精确 action 回滚。分配和同步使用模型，ASan/UBSan 检查引用与存储归还；不声称已观察厂商硬件停止。

停止凭据、GPU access、class binding、可失败 remove 和 PC module boundary 在独立导出的源码树共通过 41 个 unittest 方法。5 个编译成功的负控分别破坏凭据发布、把 offline 当停止、忽略 remove 契约、漏撤销发布失败 action、放宽 callback/data 匹配，均由运行时断言拒绝；恢复源码后再执行回归。两个新 API 已导出，公开 class/device/driver 布局与 ABI 11 不变。证据在 `.kernel-build/drm-binding-review/stop-isolated.log` 和 `negative-stop*.log`。DRM 的每 publication 状态和自动退休仍是后续消费者迁移，不能由本凭据单独推导完成。

### DRM publication 绑定与失败退休验证（2026-11-14）

DRM 文件现在固定一次 open 捕获的 publication，GEM/FB/CRTC/EDID/事件与 virgl 身份按 publication 分区，硬件调用显式携带 scoped access。新增 `test_drm_binding` 的 23 个方法运行完整生产 DRM/core/class/GPU core 与实际 anonfd 安装器，覆盖默认 promotion、同 publication 共享/跨 publication 隔离、PRIME 拒绝、EDID 缓存、文件离线、重绑 ID 复用、未知 resource/context 创建、失败销毁保留与重试、导出 fd backing、缺少 remove、分配失败、原生 mmap 参数/错误、输出复制回滚和读等待丢失唤醒。在途调用排空使用 pthread 竞争；VFS/MM/PFA/worker/Park 边界仍是模型，不代表实体硬件故障注入。

审查还发现跨 GPU PRIME 拒绝时最后一次 VFS put 可能在另一个 GPU mutex 内关闭原 dma-buf。导入 fd 的 lookup/ref/put 与导出 fd 安装/失败 close 已移到 GPU 操作作用域外，wire request 只捕获一次；专门夹具模拟 fd 已并发关闭和最后一次 put，检查这些边界未持设备 mutex，并验证导入 copyout 失败不增加 handle 存活状态。

隔离导出的最终源码树通过 343 个不同驱动 unittest 方法。停止凭据的 5 个负控与 DRM 的 12 个编译成功负控均被运行时断言/ASan 拒绝；后者包括默认漂移、跨 publication 表/PRIME 混用、提前释放失败 unref、以 offline 当停止、未知 context 当 ready、扩大 mmap ceiling、copyout 泄漏、fd put/install 移回 GPU 锁内、导入 copyout 所有权泄漏和丢失唤醒。全部恢复源码后再次执行完整隔离回归。

最终 `check-firmware-model` 通过 571 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 207 项；116 个 instance、32 个 component 清单审计通过。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，未运行 AArch64 guest。

扩展的 `smoke-driver-gpu-stop-x86_64` 为显式 embedded 双核实例，实际 2/2 CPU online；`driver.drm_binding_test=1` 执行真实 VFS、原生 worker、VMO/VMA 退出，观察旧文件 ioctl 返回 ENODEV/poll HUP，成功 remove 后旧 GEM 退出而 VMA 独占保留 VMO，新绑定命名空间与 VMO 不同、handle 可同号，真实 virtio flush 成功，最后旧 namespace 被回收，匹配 `DRIVER_DRM_BINDING: PASS file=1 stop=1 reaper=1 vma=1 rebind=1`。该内核 fixture 没有 fault 用户 PTE 或执行 virgl/厂商 GPU DMA。

另串行运行 generic 模块双核 2D 显示门禁，保留 resident framebuffer fork、DRM_SYSFS 与 videoplay 10 帧；RISC-V `smoke-drm-prime` 用户态门禁通过 shared mapping、SCM_RIGHTS、export lifetime、80 exports 和 sync-file。成功日志分别保存在 `.kernel-build/drm-binding-review/guest-stop-final-serial.log`、`guest-generic-final-serial.log`、`guest-prime-final-serial.log`。x86 生命周期 guest 另通过 class 发布/解绑、失败 remove 保留/重试、受管失败 probe、remove prepare 和三类 registry 扩容标记；日志在 `guest-lifecycle-final-serial.log`。

当前文档引用检查通过 540 条，导航仍报告共享迁移工作区既有 7 篇未编目文档，没有新增链接/锚点错误。13 篇文档仅把本轮内容投影到 canonical 路径，保留 docs/zh 迁移及其他会话修改。完整本地证据在 `.kernel-build/drm-binding-review/`。同 publication 每文件 handle/auth、旧物理 framebuffer 视图、virtio 每设备队列、厂商 BO/VRAM/GPUVM、请求/fence 与模块 unload 仍是后续工作。

### VirtIO GPU 每设备 DMA 实例验证（2026-11-14）

virtio-gpu 已移除静态实例/全局 owner，改为每绑定原生 DMA buffer，队列、暂存、mutex/等待队列和失败状态独立。buffer 同时保留设备元数据；失败 probe/停止保留该实例，成功停止在 IRQ drain、producer 存储交还和 mutex 解锁后才 put allocation。公开结构和模块 ABI 11 不变；仍沿用原有恒等 DMA 假设，不自动创建 domain。此前历史记录中的单静态 controlq 限制由本轮关闭，每设备仍只有一个同步 controlq。

`test_virtio_gpu_stop` 现有 20 个 unittest 方法：保留既有 reset/四条 issuer/失败 probe/停止重试，新增实例分配 OOM/架构拒绝不发布私有状态、两设备 queue/framebuffer 独立、失败 remove 与 command fault 不影响 peer，并检查成功销毁释放 allocation 和设备 pin。fixture 执行实际 native DMA buffer create/data/put、GPU claim/init/command/remove、PFA split 与 Rust VMO 引用/最终回收；寄存器、clock、IRQ、核心注册状态、页分配和 CPU alias 仍为模型。旧 scanout fixture 也改为在 allocation put 时销毁 host 实例，再检查 owning VMO 可继续存在，不能在 remove 后读取已释放实例。第一次广门禁因旧 fixture 缺少 allocation 字段而编译失败，不计成功；同步其模型边界后重新执行下述最终回归。

隔离导出的最终源码树通过 347 个不同驱动 unittest 方法。7 个编译成功负控分别引入共享实例、覆盖已有绑定、丢失 allocation 所有权、漏 final put、保留悬空 drv_priv、先 put 再解锁和 reset 失败即释放，均被运行时断言/ASan 拒绝；还原后执行完整隔离回归。原生 buffer 的既有 7 项独立测试与 PC 模块包装器 imports 检查一并保留。最终 `check-firmware-model` 通过 575 项及单独 1 项 sleep ABI，`check-pc-drivers` 通过 211 项；117 个 instance、32 个 component 的清单审计通过。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，未运行 AArch64 guest。

新增 `smoke-driver-gpu-instances-x86_64` 是 embedded、双 CPU、两个真实 QEMU virtio-gpu-pci；日志观察到两个独立 1024×768 framebuffer 和实际 2/2 CPU online。fixture 通过 owning scanout、真实 transfer/set-scanout/flush、独立 DRM publication，逻辑注销默认 device 后确认旧凭据成立、peer 实例/文件仍在线并继续 flush、默认 promotion 到 peer；重新登记后新 publication/VMO 不同，peer 指针和两个 RAM canary 不变，旧 VMA 仍持有旧 VMO。成功标记为 `DRIVER_GPU_INSTANCES: PASS devices=2 independent=1 stop=1 retained=1 rebind=1`。这是原生 device unregister/register，不是物理 PCI 热拔出、真实 reset 超时注入、virgl 或厂商 GPU 加速；内核 fixture 不 fault 用户 PTE。

另串行完成既有 embedded 双核 stop/DRM reaper 门禁、generic 模块双核 2D 门禁和 RISC-V PRIME 用户态门禁；保留旧 VMA/重绑、resident framebuffer fork、DRM_SYSFS、videoplay 10 帧、shared mapping/SCM_RIGHTS/80 exports/sync-file 结果。完整成功 guest 日志位于 `.kernel-build/gpu-instance-review/guest-instances-serial.log`、`guest-stop-serial.log`、`guest-generic-serial.log`、`guest-prime-serial.log`。旧实例的退出没有使外部 scanout/DRM 引用重新依赖驱动模块。

九篇文档仅将本轮内容投影到 canonical 路径，保留共享 docs/zh 迁移和其他改动。工作区引用检查通过 540 条；导航仍报告既有 7 篇未编目文档，没有新增链接/锚点错误。本轮本地证据在 `.kernel-build/gpu-instance-review/`。多 GPU 用户节点/显示拓扑、跨 GPU PRIME attach、每文件 handle 权限、厂商 BO/GPUVM/任务与可失败关机/恢复仍需继续实现。

### 原生 GPU 角色、固件回退与 ABI 12 验证（2026-11-14）

GPU vtable 末尾追加 get_capabilities，kernel 与 drvctl 的驱动 ABI 同步为 12。四个现有 provider 均按已建立状态声明角色；普通查询复用 active access，注册/重新选择不再扫描 virgl 回调表。原始两级策略审查发现，仅比较渲染会让原生 2D 初始化后仍保留旧 GOP 默认视图，因此增加显式 BOOT_SCANOUT：先比较 RENDER，同级优先原生管理输出，等分稳定。此标志不是固件存储接管或撤销授权，输出连接状态/拓扑仍需独立协议。

GPU access 主机夹具执行生产核心/class/能力 helper，以及 simplefb、VMSVGA、lsdc 的实际缓存回调；新增参数/错误清零、未知位/角色依赖、初次 probe 拒绝无 publication/pin、native renderer 无 virgl 表仍优先、legacy 表完整却不虚报渲染、headless 排除、失败重选与四级固件回退。VirtIO fixture 检查实际缓存回调在 valid/closing/virgl 状态下的声明；正向 RENDER 状态由模型设置，真实 QEMU 本轮只运行 2D 协商。kernel header guard 与真实 ELF drvctl parser 分别拒绝旧 11/未来 ABI；PC module boundary 编译实际包装器/imports。LSdc 保留平面模型同步了新字段及真实能力头，不代表物理面板验证。

隔离导出的最终源码树通过 360 个不同驱动 unittest 方法，另有独立 gpu_select_test 通过。16 个编译成功负控分别引入缺 provider 回退、失败输出残留、正数错误泄漏、未知位/角色依赖遗漏、headless 抢占/重选、旧表推断、选择锁内执行 provider、忽略固件优先级、过早停止重选、优先级截断为 bool、未协商即报告渲染和接受旧 ABI，均由运行时断言/ASan 拒绝；还原源码后执行完整隔离回归。首次广门禁发现 LSdc 测试的精简 vtable 未增加新回调而编译失败，不计成功；更新模型后重新执行全部最终门禁。

最终 check-firmware-model 通过 585 项及独立 1 项 sleep ABI，check-pc-drivers 通过 221 项；118 个 instance、32 个 component 清单审计通过。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，未运行 AArch64 guest。

新增 smoke-driver-gpu-caps-x86_64 使用无硬件 GPU 的 embedded 双核实例，真实内核依次发布合成固件输出、headless、原生输出、保留固件输出的 renderer 和原生 renderer，检查稳定 tie、逐级重选、无旧 virgl 表的 native 声明、畸形 probe 拒绝及最终设备引用归还。实际 2/2 CPU online，匹配 `DRIVER_GPU_CAPS: PASS native=1 firmware=1 headless=1 election=1`；角色本身是合成声明，没有执行硬件渲染或固件接管。

另串行运行最终双 VirtIO 设备实例、generic 模块双核显示、embedded stop/DRM reaper 和 RISC-V PRIME 用户态门禁。真实两个 VirtIO 2D 设备的原生 RENDER 与实际 get_features 协商位一致，且仍保留独立 stop/rebind/旧 VMA；generic ABI 12 包实际加载并完成 DRM_SYSFS、resident framebuffer fork 和 videoplay 10 帧；PRIME 仍通过 shared mapping/SCM_RIGHTS/export lifetime/80 exports/sync-file。成功日志在 `.kernel-build/gpu-caps-review/guest-caps-serial.log` 与 `guest-{instances,generic,stop,prime}-final-serial.log`。没有在 guest 中注入旧包、硬件停止超时或运行 virgl/厂商 GPU 加速。

十四篇文档仅将本轮内容投影到 canonical 路径，保留共享 docs/zh 迁移及其他改动。工作区引用检查通过 540 条；导航仍是既有 7 篇未编目文档，无新增链接/锚点错误。完整本地证据在 `.kernel-build/gpu-caps-review/`。本轮关闭默认策略的旧表推断；通用 BO/GPUVM/job、完整权限/格式/队列限制、多 GPU 用户节点、显示拓扑/固件安全交接和厂商硬件仍需继续实现。

### DRM 每文件 handle 与 framebuffer 所有权验证（2026-11-14）

GEM 的对象 ID 仍在每 publication 内分配，每文件 handle 集合只保存 ID，并按对象记录 handle_refs。所有 wire handle 查找要求调用文件拥有该条目；PRIME/GEM_OPEN 显式安装幂等 owner。正常最后 close 交还本文件 framebuffer、全部 handle 和 host context，失败硬件清理仍保留原对象；dup/fork 的共享 vfile 只在最后引用退出时清理。framebuffer 保留 backing，GEM_CLOSE 后仍可由 owner 呈现；GETFB/GETFB2 不向未持有者授予 backing，owner 重建 handle 的失败输出会回滚。legacy name 由 adapter 分配，最后一个 handle 退出后失效。模块 ABI 12 与公开结构均不变。

`test_drm_binding` 共 39 个 unittest 方法，执行生产 DRM/core/class 与实际 anonfd 安装器，ASan/UBSan 覆盖未导入 handle 的 ioctl/mmap/导出/提交拒绝、重复导入、分配和输出失败回滚、framebuffer 保留和所有权、在线混合批次 close、共享 vfile、VMA/导出 pin 保留、最后 handle 的 name 撤销、停止后 ID 条目及清理失败重试。VMO/PFA、MM、VFS、work 和调度边界仍为模型；VMO release 另断言不持 store mutex。初始化先预分配 handle 条目，避免成功创建 VMO 后因条目 OOM 而在 store 锁内销毁。12 个编译成功负控破坏上述所有权、正常 close、幂等/输出回滚、FB 权限/保留、name 退休或恢复该错误分配顺序，均被运行时断言拒绝；还原后隔离源码树通过 376 个不同驱动 unittest 方法。

最终 check-firmware-model 通过 601 项及独立 1 项 sleep ABI，check-pc-drivers 通过 237 项；118 个 instance、32 个 component 的清单审计通过。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，未运行 AArch64 guest。

三项标准 guest 门禁最终完整通过：embedded 双核 stop/DRM reaper 门禁保留旧 VMA/重绑检查，并增加在线新文件不调用 GEM_CLOSE 而直接 close 后，真实 VMA 成为 VMO 唯一 owner 的断言；generic 模块双核显示保留 DRM_SYSFS、FBDEV_MAP/fork 与 videoplay 10 帧；RISC-V PRIME 新增两个独立 open 的未导入拒绝、显式共享、producer 正常 close 自动移除 FB/handle 与既有 VMA 保留，输出 `DRM_FILE: PASS ownership+close+framebuffer+prime+retained-vma`，并继续通过 SCM_RIGHTS、80 exports 和 sync-file。初次 PRIME guest 已输出成功标记，但门禁正则未转义加号，不能记为标准门禁通过；修复后重新执行目标，最终 exit 0。没有运行本轮 virgl 或厂商 GPU 渲染，也没有完成 master/auth、DMA-BUF 只读权限传播、命令流验证、预算或 GPUVM。

十二篇文档同步所有权/限制，并修正总览中已过期的 driver_manager ABI 包含与 RISC-V 动态 IOMMU 描述。共享 docs/zh 迁移及其他修改保留；canonical 文档仅投影本轮变更。工作区 540 条、canonical 653 条行号引用检查通过；两侧导航均保留各自既有的 7 个未编目项，列表与本轮前完全一致。源码快照、负控及最终日志位于 `.kernel-build/drm-file-review/`。

### 固定 resident RAM 与分配预算验证（2026-11-14）

新增内部 kind 5 与原生 vmo_create_resident，复用既有 VMO 结构/Rust 生命周期/析构；公开 vmo_create 拒绝直接构造该 kind，物化与 resize 的 C/Rust 锁边界保持固定 extent/slot。全部 order-0 页与页取整 padding 在发布前清零，支持超过最大 buddy 块的范围和非连续 PFN；无连续地址查询或 DMA 授权。owner 采用现有共享匿名 backing charge 与 cgroup namespace pin，随 VMO 交还，部分失败复用原生析构。DRM 三种分配入口已采用，外层在 GPU mutex 前捕获分配任务的 cgroup，RAM 构造在 store 锁外；attach 只读取既有页，正常 close/失败退休/VMA/导出沿用原来的引用。新增模块导出不改公开布局或 ABI 12。

新增 test_vmo_resident 的 12 个 unittest 方法运行生产工厂、真实 C cgroup 原语、Rust ref/pin/destroy，模型边界为 order-0 分配、CPU alias、TLB 与通知；覆盖碎片页、超过 8 MiB 的范围、padding、参数、预算/元数据/每一页失败、每一页 alias 错误、不可变范围、task RSS teardown、owner namespace 引用及独立页 pin。DRM fixture 共 44 个方法，factory/VMO 为模型，新增预算耗尽、原生 errno/输出失败退款、归属捕获、离线准入失败与失败 host 清理期间 charge 保留；全部 GPU/store mutex 都参与锁边界断言。独立页 pin 可跨源销毁保留 RAM，但不转移 charge，不能当作预算 lease。

16 个编译成功负控分别破坏工厂专用 kind、清零、输出清理、共享 backing 记账/padding charge、owner pin/失败回滚/最终 unpin、部分页释放、extent/slot 冻结、resident pin 白名单，以及 DRM 预算 owner、锁外物化和锁前任务归属捕获，均被运行时断言拒绝；还原后最终隔离源码树通过 393 个不同驱动 unittest 方法。另执行完整生产 C VMO 与 Rust materialize/resize/destructor 的共享匿名集成：resident fault 不向另一组重复 charge，legacy task RSS teardown 不扣对象 charge，拒绝 resize，独立 page pin 按其契约保留存储；保留原来的 256 个并发 first-touch 样本。VMO page Rust 测试 14 项、lifecycle Rust 测试 7 项通过。

第一次广回归发现两个 IOMMU 夹具的 API-only VMO 头缺少新声明，自检同时不应依赖 opaque 对象布局；改用 peek/resize 接口检查并补充声明，未把编译失败计为通过。共享匿名完整 C 夹具还缺少此前 contiguous/PFN 生命周期加入后的原语声明，已补齐 canonical order-0 引用模型，高阶构造仍由独立 contiguous fixture 验证。直接 Rust resize 拒绝时沿用旧负错误值，公开 C resize 才提供 `-EOPNOTSUPP`，同步修正相关文档，未改变旧 Rust 桥接返回协议。

最终 check-firmware-model 通过 618 项及独立 1 项 sleep ABI，check-pc-drivers 通过 254 项；118 个 instance、32 个 component 的清单审计通过。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，未运行 AArch64 guest。

RISC-V 原生 IOMMU/EDU 标准门禁最终通过，新增 `DRIVER_IOMMU_RESIDENT_VMO: PASS canonical=1 fixed=1 dma=1 lifetime=1`：新对象全部页已物化且拒绝 resize，源 VMO 销毁后真实设备通过非恒等 IOVA 跨页读写，并逐字节比对数据及 guard。使用内核 NULL budget owner，不表示实际 cgroup OOM 或实体 GPU 验证。x86_64 embedded 双核 stop/DRM/VMA 门禁检查真实 kind/预分配页、旧 VMA 保留与在线 close 后唯一 VMA owner；generic 模块双核显示完成 DRM_SYSFS、FBDEV_MAP/fork 和 videoplay 10 帧；RISC-V PRIME 保留 DRM_FILE、共享映射、SCM_RIGHTS、80 exports 和 sync-file。上述标准目标均返回 0；没有运行本轮 virgl/厂商 GPU 加速、真实硬件超时或完整 BO/GPUVM/job。

十二篇文档同步工厂、预算与锁边界；共享 docs/zh 迁移及其他工作区改动保留，canonical 路径仅投影本轮内容。工作区/canonical 行号引用检查通过，导航保留各自本轮前已有的 7 项未编目条目，没有增加链接/锚点错误。源码树、负控及最终主机/guest 日志位于 `.kernel-build/resident-vmo-review/`。

### 固定 RAM VMO 的 owning DMA 视图验证（2026-11-14）

新增 driver_dma_map_owned_vmo，复用原生快照和 DMA mapper，限定工厂固定 kind 4/5。adapter 在 provider 前取得 VMO 引用，在局部 success/cleanup 输出中无分配地交接后才发布；整个对象、未映射页与原始 charge/pin 保留，设备只映射选定页。旧 map_vmo 继续只保留帧。buffer 与 VMO 最终 put 在确认全部失效、sync、mapping 退休并退出 domain mutex 后执行；try_unmap 的临时 domain 引用跨 owner 回调，构造回滚依靠 caller 贯穿调用的引用。只新增函数导出和私有 mapping 字段，公开布局与 ABI 12 不变。

新增 15 个 resident mapping unittest 方法执行生产 C resident/contiguous 工厂、cgroup 原语、Rust ref/destroy 和 DMA mapper；PFA、CPU alias、cache hook 与 provider 是边界模型。验证整对象/charge、多个视图、kind 4 正例、拒绝可变/物理类型、参数/元数据失败、policy、部分 map、非法 IOVA、部分 unmap/重试、detach 失败和锁外 owner 回调重新进入 shutdown。provider 逐次断言所选 PFN，防止把保留的额外页暴露给设备。两个 buffer 方法另验证不同 allocation device 的最终 release，在正常 unmap 和映射失败所返回清理句柄的最终 unmap 后均可锁外重入 shutdown。

16 个编译成功的负控均被运行时断言拒绝，覆盖丢失/泄漏 VMO 或 buffer 引用、失败 cleanup 丢 owner、失效前退休、错误类型/视图、错误输出、锁内回调、domain 过早释放及旧 snapshot 语义改变。首次 wrong-first 负控删除了参数使用，被 -Werror 拒绝编译；改为仍使用参数但产生错误页索引后，确认由运行时 PFN 断言检出，未把编译错误计为通过。还原后导出源码逐文件等于 staged source tree，最终隔离回归 410 个不同 unittest 方法通过。check-firmware-model 通过 635 项及独立 1 项 sleep ABI，check-pc-drivers 通过 271 项，118 个 instance 与 32 个 component 清单审计通过。

五条 guest 门禁最终返回 0：RISC-V 原生 IOMMU/EDU 的 resident 分支采用 owning 视图，交还 caller 引用后核对包括未映射页的整个对象，再完成真实跨页 DMA/逐字节数据与 guard 比对，要求 owner=1；x86_64 embedded 双核 stop 验证 DRM/退休/VMA/重绑；generic 模块显示完成 DRM_SYSFS、FBDEV_MAP/fork 与 videoplay 10 帧；RISC-V PRIME 完成 DRM_FILE、共享映射、SCM_RIGHTS、80 exports 与 sync-file；x86_64 lifecycle 完成失败 probe/remove/prepare 与 registry capacity 注入。lifecycle 是 legacy smoke case，通过 python3 tools/smoke.py 调用，首次 tools/a20 test 因无对应 instance 拒绝，未计为运行证据。AArch64 embedded、Linux ABI、BRINGUP kernel-only 构建通过，未运行该架构 guest。

EDU 的 budget owner 为 NULL；真实 charge/namespace pin 生命周期由生产 C/Rust 主机集成验证。匿名/连续 EDU 分支仍使用旧快照，连续 owning 正例由主机集成覆盖。没有运行本轮 virgl/厂商 GPU、真实硬件停止超时或实板 cgroup OOM；本接口不是 BO 迁移、GPUVM、共享权限或 job 调度器。源码快照、负控与日志位于 .kernel-build/resident-map-review/。

十二篇文档同步 owning 视图、预算与锁外退休契约。工作区与 canonical 行号引用检查通过；导航检查各自保留本轮前已有的 7 项未编目条目，错误列表完全相同，没有增加链接/锚点错误。共享 docs/zh 迁移仅投影本轮内容，其他工作区改动保留。

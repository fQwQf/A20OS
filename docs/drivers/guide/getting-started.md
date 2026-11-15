# 从零开发第一个 A20OS 驱动

本页是一条从选接口到实际 I/O 验收的实施路线。先读[核心模型的架构总览](core-model.md#架构与开发入口)，再按本页动手。同步 PCI 驱动的完整 C 骨架集中在[核心模型](core-model.md#可编译的-pci-网络驱动骨架)，异步驱动的完整骨架集中在[原生请求与队列](request-queues.md#接入骨架)；硬件协议需要按手册实现，骨架本身不提供真实数据收发。

如果要修改框架接口、生命周期或公共对象，转到[驱动框架维护与扩展](framework-development.md)。

## 1. 确定设备、消费者和执行方式

先写清楚三件事：设备由谁枚举，谁消费它的功能，怎样取得 I/O 进度。

| 问题 | 选择 |
|---|---|
| 已有 PCI/PCIe、VirtIO、USB 枚举 | 使用对应总线的 device 和资源；功能驱动不再扫描/创建同一硬件 |
| 固定 SoC 外设或新板 | 通过 firmware/platform 描述地址、IRQ 和依赖；可复用实现不硬编码板地址 |
| 字符、块、网络、输入、显示、音频 | 实现对应 class；核心自动发布其 primary protocol |
| 传感器、控制服务等无合适 class 的功能 | probe 中登记原生协议；不必添加 class enum 或伪造 devfs 节点 |
| 有界同步控制 | 在协议方法内完成操作并明确超时、单位和返回值 |
| 异步 DMA/加速命令 | 使用 native request/queue、owning memory 和 fence，或明确说明既有专用数据路径的所有权 |
| 网络包等批量热路径 | 使用设备类已有的批量/SG 能力；不强制每包分配通用 request |

每个设备实例只有一个硬件 owner。Linux 文件/ioctl 和 Native handle 是消费原生功能的适配入口，用户权限由相应 broker/适配层检查。内核协议 ID 和 capability 位只说明接口/功能，不授予用户访问权。

## 2. 找到要修改的文件

| 位置 | 用途 |
|---|---|
| `kernel/drivers/<class>/` | 硬件协议、每实例状态和类操作 |
| `kernel/drivers/core/driver_core.h`、`driver_class.h`、`driver_hwapi.h` | 核心生命周期、既有类与硬件访问声明 |
| `kernel/include/drivers/core/` | protocol、queue、fence、workqueue、DMA、firmware 公共接口 |
| `kernel/include/drivers/bus/`、`kernel/drivers/bus/` | 总线接口、枚举与 transport |
| `kernel/include/drivers/<class>/` | 跨实现共享的功能接口 |
| `kernel/drvmod/examples/` | generic 模块的描述符和包装入口 |
| `tools/mk/driver-sources.mk`、`driver-modules.mk` | embedded 源码账本、generic 模块规则与架构清单 |
| `kernel/arch/<arch>/boards/<board>/` | 板级资源、固件来源与平台适配 |
| `tools/tests/`、`instances/` | 主机工作流和实际启动/I/O 验证 |

先选一个机制相近的参考实现，阅读其 probe、停止、remove 和模块包装，不必从头阅读整个驱动目录。VirtIO GPU 可参考 `virtio_gpu.c` 的 native queue 接入；网络扩展可参考 `virtio_net.c`、`rtl8139.c` 的 NET_STATS 与 lwIP 消费者。旧驱动有各自的历史限制，新的所有权和退出契约以[核心模型](core-model.md)与[运行时契约](runtime-contracts.md)为准。

## 3. 写一张硬件契约表

| 项目 | 需要确认的内容 |
|---|---|
| 身份 | 总线、vendor/device、subsystem ID 或 compatible、已实现的硬件变体 |
| MMIO/PIO | 资源长度、访问宽度、字节序、W1C/读清副作用 |
| DMA | 地址宽度、对齐、descriptor 格式、长度上限、cache 一致性 |
| IRQ/进度 | 来源、共享/触发类型、确认顺序，以及无 IRQ 时是否有受支持的 polling |
| 执行 | 能力、队列深度、背压、完成条件、错误和超时 |
| 停止 | 怎样证明不再 DMA/产生回调，停止失败时保留哪些对象 |
| 消费 | 数据单位、成功返回值、内核 buffer/用户边界和共享权限 |

使用设备手册与目标平台枚举日志核对，不通过猜测写寄存器取得身份。PCI 的 subsystem 通配显式写 `VENDOR_ANY/DEVICE_ANY`；数字零不是通配。只匹配实际实现的设备，不能用覆盖所有网卡的 class match 代替芯片支持。

## 4. 按统一生命周期实现

```text
总线/固件发布 device
  -> match（只检查身份）
  -> probe：选择失败清理 -> 建立部分实例 -> 登记协议/执行停止
            -> 取得 MMIO/IRQ/DMA -> 初始化并启动 -> 健康检查
  -> 核心发布 endpoint / class
  -> consumer 在 access 内调用；异步请求独立保留 backing
  -> 可选 pre-consumer prepare -> consumer/child 解绑
  -> offline -> execution stop -> quiesce -> active call drain
  -> remove=0 -> 逆序 devres 回收
```

probe 在硬件初始化前调用 `device_probe_use_remove()`，把部分实例交给 `dev->drv_priv`，并初始化退出路径会使用的锁/状态。此时实例尚未对消费者公开；probe 成功后的核心 publication 才是服务准入点。可以早存 `drv_priv`，ready 则在设备真正可用后设置。

有通用异步队列时，`driver_queue_create()` 自动登记 execution stop；其他有生产者的执行路径可用 `device_add_execution_action()`，须在启动之前登记。回调处理部分初始化，并在返回 0 前确认硬件/软件生产者停止。不能确认就返回负 errno，保留实例、IRQ、DMA 和代码供重试。remove 在 stop 和调用排空之后释放剩余所有权。

不要同时让 probe 的错误标签和 managed remove 释放同一实例。未选择 managed rollback 的旧式驱动，仍须在返回错误前自行安全停止并回滚；新驱动优先使用上述统一路径。更早冻结 consumer 借用存储的场景使用 remove prepare，不能靠较晚的 execution stop 补救。

具体控制流使用[可编译 PCI 骨架](core-model.md#可编译的-pci-网络驱动骨架)或[异步骨架](request-queues.md#接入骨架)。

## 5. 完成 I/O、存储与消费者接入

MMIO 使用 `pci_map_bar()` 或受管映射 API。`resource.start` 是 CPU 物理地址，不能直接当内核 VA 解引用；CPU 物理地址也不自动等于设备总线地址。DMA mask、native owner、mapping/IOVA、cache sync 各自按[运行时契约](runtime-contracts.md)处理。

每条异步请求持有自己的存储所有权。IRQ 确认设备已经交还请求后调用 complete；任务 retire 完成缓存/映射/cookie 清理后才发布 fence。等待超时不取消 DMA，不授权回收页面。worker 模式在调度器初始化后启动，多个队列可共享 workqueue；早期启动用有界 poll 驱动同一个后端。

consumer 持 endpoint 引用并配对 `driver_protocol_begin/end`，在 access 内调用 ops。异步方法可以返回独立 fence 引用并结束 access，但已接受请求必须自持 backing。现有六类优先用 class adapter，新增功能可发布独立协议，完整调用示例见[原生协议](core-model.md#原生协议组合能力而非扩大全局-vtable)。

类操作的返回值按[设备类接口](device-classes.md)实现：例如网络 send 成功返回字节数，块 read/write 成功返回 0；不能统一套用一个成功值。类方法接收内核 buffer，用户地址检查属于 ABI adapter。display 的默认选择和 scanout backing 另见[显示类](../classes/display.md)。

## 6. 接入构建与部署

默认 hosted generic 构建从 DriverStore 加载设备模块。以 `kernel/drvmod/examples/rtl8139.c` 为包装参考：复用共享 `.c`，登记精确描述符，由 DriverEntry 注册同一 `driver_t`。补齐 `driver-modules.mk` 的规则和实际支持架构清单；embedded 则补齐 `driver-sources.mk` 的显式设备源码账本。把文件放进类目录不会自动完成接入。

在仓库根目录执行下列命令，按目标平台替换 ARCH/BOARD：

```sh
# embedded：核对共享实现是否进入静态内核
make ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both DRIVER_DEPLOYMENT=embedded kernel-only -j4
# generic：连同驱动包、DriverStore 和启动所需镜像一起构建
make ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both DRIVER_DEPLOYMENT=generic dev-build -j4
```

generic 的 kernel-only 成功不代表新模块已构建或 imports 可解析。新增 API 还须确认 `kernel/drvmod/framework.c` 导出；模块描述符与 driver ID 表的匹配范围应一致。启动期需要的根设备进入 Early DriverStore；其他设备走 Runtime，详见[部署配置](deployment-profiles.md)。

创建适合目标设备的 `instances/` 配置，写明所需 QEMU 硬件、命令和 I/O 断言，再运行 `tools/a20 check` 和 `tools/a20 test my-driver-x86_64`（此处名称为你创建的实例）。实体硬件使用对应板级启动/部署流程；构建成功、绑定成功和真实 I/O 成功分别记录。

## 7. 用最小闭环验收

先验证单设备完成一次真实 I/O，再验证错误和退出；无需为文档改动重跑整个硬件矩阵。按代码改动范围从[测试与提交](../meta/testing-and-submission.md)选择门禁。

| 阶段 | 必须观察到的结果 |
|---|---|
| 发现与匹配 | 目标设备匹配；缺少设备或不支持的变体不误绑定 |
| probe/发布 | 资源与能力检查通过，正确的 class/protocol 消费者能找到绑定 |
| 实际 I/O | 数据/状态确实由硬件或目标设备模型改变，返回值符合类契约 |
| 分配或初始化失败 | 清理成功时回收；停止/失效未确认时保留部分实例，错误可重试 |
| 背压与并发 | 队列满不会越界；多实例、并发 consumer 不共享错误的私有状态 |
| 退出与重绑 | 新调用被拒绝，在途请求安全退休；旧引用不跟随新绑定 |

找不到 driver：检查源码账本、模块规则、descriptor 与 imports。设备存在但未 probe：检查 bus、ID、subsystem 通配和 match。probe 超时：检查映射、寄存器宽度和 reset/IRQ 进度。probe 成功但无数据：检查 class 返回值、协议/消费者、DMA 地址和 cache 交接。退出失败：保留现场，按 stop/retire/remove 返回值重试，不先释放再报告成功。

交付时记录支持的硬件/平台、构建和部署方式、I/O 与失败验证日志、锁顺序和当前限制。这样下一位开发者可以重现结果并继续扩展。

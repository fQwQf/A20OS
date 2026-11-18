# 驱动框架维护与扩展

面向修改公共接口、核心对象、生命周期或通用执行机制的开发者。只增加一个硬件实现时，先走[第一个驱动](getting-started.md)；理解整体结构从[核心模型](core-model.md#架构与开发入口)开始。本文说明扩展应放在哪里、必须保持哪些契约，以及怎样验证它确实能被驱动使用。

## 1. 设计边界

A20 的内部模型围绕设备身份、当次绑定、显式存储和执行所有权组织。Linux ABI 与 Native ABI 适配共同消费内部机制；Linux file/ioctl 结构不成为新内核机制的基础对象。内核协议 ID、功能 capability 和用户访问 rights 含义不同，不能用“支持某功能”代替授权。

总线是硬件发现/枚举的唯一来源。endpoint 复用当次绑定的 publication，传统设备类作为其 primary protocol/adapter 存在。新增能力复用这些身份和引用，不再建立一套 device registry、owner 槽或随重绑自动转向的新 getter。

核心负责准入、所有权和生命周期；具体驱动负责寄存器协议、DMA/cache 交接和停止证明。资源 claim 约束参与该模型的访问者，不能约束任意 raw MMIO。内核模块仍是受信任代码；用户服务另经 capability/broker 和可用的硬件隔离后端。

## 2. 源码地图与阅读顺序

| 关注点 | 实现与声明 | 配套文档 |
|---|---|---|
| 注册、匹配、probe/退出、devres、依赖 | `kernel/drivers/core/driver_core.c` 与同目录 `driver_core.h` | [核心模型](core-model.md) |
| publication、class registry、devfs 桥 | `driver_class.c`、`driver_class_mcu.c`、`driver_class.h` | [设备类](device-classes.md) |
| 协议快照、版本/大小检查、active call | `driver_protocol.c`、私有 `driver_protocol_internal.h`、公共 `protocol.h` | [原生协议](core-model.md#原生协议组合能力而非扩大全局-vtable) |
| 请求状态、依赖、完成/退休、执行停止 | `queue.c`、公共 `queue.h` | [请求与队列](request-queues.md) |
| 完成通知、任务工作、IRQ 排空 | `fence.c`、`workqueue.c`、`driver_hwapi.c` 及相应声明 | [运行时契约](runtime-contracts.md)、[锁顺序](lock-order.md) |
| native 存储、映射和失败清理句柄 | `driver_hwapi.c`、`dma_mapping.c`、`dma_vmo.c`；`kernel/mm/vmo.c` | [原生运行时](native-runtime.md) |
| 总线/固件、板级来源 | `kernel/drivers/bus/`、`kernel/firmware/`、`kernel/include/drivers/bus/` | [总线与平台](bus-and-platform.md) |
| 模块 exports、descriptor、包与部署 | `kernel/drvmod/framework.c`、`kernel/include/drivers/driver_descriptor.h`、`tools/mk/driver-*.mk` | [内核模块](kernel-modules.md)、[部署配置](deployment-profiles.md) |

表中未写全路径的 `.c` 位于 `kernel/drivers/core/`；公共 protocol/queue/fence/workqueue/DMA 头位于 `kernel/include/drivers/core/`。先顺读一次 probe→publication→access→stop→remove，再看所扩展机制；不必先读所有平台后端。

## 3. 把改动放在合适的层

| 新需求 | 推荐落点 |
|---|---|
| 单芯片的寄存器、quirk、descriptor、复位步骤 | 驱动私有实现/头文件 |
| 已有功能的一种新硬件实现 | 既有总线和 class/protocol 后端 |
| 同一绑定上的可选统计或设备能力 | 独立原生协议，声明 ID/version/实际 ops_size/capabilities |
| 多种驱动共用的内存、完成或执行机制 | native core 公共接口，配合实际 provider/consumer 接入 |
| 必须改变所有该类 consumer 的单位或操作语义 | 子系统接口与 adapter；评估 class 表及 ABI 的影响 |
| 仅为兼容现有 Linux 程序的线格式/命令 | ABI/subsystem adapter，翻译到原生机制 |
| 新硬件连接方式或新板 | 总线/固件/平台层；功能驱动不复制枚举 |

先完成一个实际使用场景，再确认哪些语义值得抽成通用机制。保留既有同步、批量/SG 等合理路径；通用 request 并不要求每个网络包或每个简单寄存器读取都分配新对象。借鉴上游硬件协议时，也按上述边界映射到 A20 的对象和生命周期。

## 4. 公共接口必须说明的契约

| 项目 | 需要给出的答案 |
|---|---|
| 身份与有效期 | 对象属于设备、某次绑定、某个 open、请求还是独立 backing？重绑后旧对象怎样表现？ |
| 引用与所有权 | 输入是借用还是转移？成功和失败各由谁持有？最后 put 能否执行模块代码？ |
| 上下文 | probe-only、task、早期 boot 或 IRQ？是否分配、等待、执行回调或最终析构？ |
| 锁与重入 | 哪把锁保护状态？回调前释放哪些锁？允许并发、inline completion 或回调重入吗？ |
| 失败输出 | errno、输出清零、部分初始化句柄、容量占用与显式重试如何处理？ |
| 硬件/存储 | 谁确认 DMA 结束和映射失效？超时返回后哪些页面/上下文仍须保留？ |
| 限制与权限 | 数量/长度/溢出检查、背压、能力协商及用户 rights 在哪一层验证？ |

声明位于公共头；仅核心内部共享的布局留在私有头。文档用状态图、所有权表和一条真实使用路径解释约定，不靠复制结构体字段维护另一份定义。字段和原型以当前头文件为准；上下文/分配行为同时核对实际调用路径。

例如，queue submit 与 IRQ complete 不分配 request 存储，但 task progress 初次为 pending dependency 建立 watch 时可以分配，provider prepare/retire 也按各自约定工作。不能把整个执行路径笼统称为“无分配”。fence 完成、native backing 引用和 DMA 权限也不能互相替代。

## 5. 生命周期与失败路径

完整退出顺序是：可选 pre-consumer prepare → consumer/child → offline → execution stop → quiesce → active-call drain → remove → devres。详细状态见[核心生命周期](core-model.md#生命周期的精确语义)。

修改时保持三条性质：停止/失效未确认就保留硬件可达资源；需要唤醒请求的停止工作发生在调用排空之前；成功停止也不提前释放仍被 CPU 调用使用的实例。execution action 自动选择受管 failed-probe 清理，登记成功才转移 context；其元数据 OOM 仍保留已选择的 managed rollback。

prepare 用于 consumer 仍借用硬件存储时提前冻结，execution stop 用于排空调用前停止执行，remove 用于最终交还资源。三个阶段不能合并成一个无错误返回的回调。各阶段成功项可以 checkpoint；失败是部分进度保留，不能假设整个设备图会自动回滚。

registry spinlock 只保护短元数据操作。lifecycle mutex 可以覆盖绑定退出，但完成路径不能等待它；provider 回调、fence signal、native owner 最后 put 的锁要求逐项检查。新引用不自动授予在线调用许可；active access 不自动保留异步数据。

## 6. 协议演进、模块与适用配置

新增可选功能优先发布独立协议。同一协议 ID 的语义版本必须匹配，consumer 声明实际需要的 ops 前缀和能力位；描述符被复制，ops/code 必须覆盖绑定生命周期。同版本兼容尾部增长仍需正确上报实际大小；改变语义则使用新版本，当前不在一个绑定下为同一 ID 同时发布多个版本。

协议版本与模块 ABI 是两件事。新独立协议或函数导出不自动改变既有布局；改变共享的 device/driver/class/HCD/GPU 表布局或调用约定时，按模块 ABI 策略更新并重建相应包。当前版本从 `A20_DRIVER_ABI` 读取，不在新指南里复制一份版本常量。模块 imports 必须能从 framework 导出表解析，内建成功不能代替 generic 包验证。

明确声明 hosted/MCU、MMU/NOMMU、架构和硬件后端的适用范围。MCU 使用轻量 publication 后端，不依赖完整 VFS/class registry；hosted queue/fence/workqueue 不等于 MCU 已具备同一异步运行时。平台未实现的 DMA/消息中断能力应显式拒绝或按驱动声明的路径回退。

## 7. 按改动选择验证

| 修改范围 | 最小验证方向 |
|---|---|
| 仅文档、API 注释 | 链接/导航、代码引用、完整 C 骨架语法检查；保持注释与实现一致 |
| 协议与 publication | `test_driver_protocol`，包含 hosted/MCU、旧引用、active call 与失败清理 |
| 请求、fence、worker、停止顺序 | `test_driver_queue`、`test_driver_fence`、`test_driver_workqueue`，加真实 kernel worker/lifecycle 的运行用例 |
| DMA/VMO | 对应 mapping、buffer、resident/VMO 工作流；验证失败句柄、最后 owner 和失效后真实访问行为 |
| class/模块公共面 | consumer 回归、`test_pc_module_boundary`、generic 模块加载与 embedded 接入 |
| 总线/驱动核心 | `check-firmware-model`、生命周期启动回归和受影响平台构建；平台范围按改动选择 |

快速运行示例（仓库根目录）：

```sh
python3 -m unittest tools.tests.test_driver_protocol tools.tests.test_driver_queue
python3 -m unittest tools.tests.test_driver_fence tools.tests.test_driver_workqueue
python3 -m unittest tools.tests.test_pc_module_boundary
```

需要实际内核执行时，`tools/a20 test smoke-driver-gpu-stop-x86_64` 覆盖 native worker、请求停止重试、调用排空及真实 VirtIO GPU 退出；`make PYTHON=python3 smoke-gpu-access-x86_64` 覆盖 generic GPU 模块接入。它们不替代实体 GPU 的停止/故障证据。完整门禁选择见[测试与提交](../meta/testing-and-submission.md)。

## 8. 让下一位维护者接得上

一次公共机制变更应同时交付接口契约、实际调用者、适用配置、关键失败工作流和可复现命令。更新本目录入口、相关运行时/锁契约与实现状态；把已实现、仅编译通过、模型验证和实机验证分开记录。

提交以完整使用场景组织：描述触发条件、最终行为和验证边界。新增原生机制若没有 provider/consumer，或只能靠绕过框架的私有 getter 工作，应先把接入路径做实，再扩大抽象。这样后续开发围绕已有架构增加能力，而不用重新猜测对象的归属和退出顺序。

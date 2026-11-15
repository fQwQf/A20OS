# 构建、测试与提交驱动

按实际改动选择验证：文档校验链接和示例，公共机制校验生产工作流，硬件实现还需目标 I/O 证据。锁或上下文变化同步更新 `lock-order.md`。

驱动的完成标准不是“文件能编译”，而是枚举、匹配、失败回滚、类消费和目标硬件 I/O 都有证据。外部贡献者按下面的命令矩阵执行。

## 把驱动接到构建里

full profile 不会通配包含类目录中的设备驱动。`tools/mk/driver-sources.mk` 的 `DRIVER_CORE_SRCS` 仍通配 `drivers/core/*.c` 与 `drivers/bus/*.c`，但 `GENERIC_KERNEL_SERVICE_SRCS` 和 `EMBEDDED_DEVICE_DRIVER_SRCS` 是显式列表；`tools/mk/driver-modules.mk` 也按架构显式列出 generic 的 `.a20drv` 包。新增设备驱动必须更新适用的源码账本、模块包装/规则和架构包清单，不能仅把文件放进 `kernel/drivers/<class>/`。公共声明放 `kernel/include/drivers/<class>/`；只在一个驱动内部共享的寄存器和私有结构放驱动私有头，不要进入公共 include 树。

MCU profile 只编译 `BOARD_DRIVER_DIR` 和选中的平台，新增文件要检查 Makefile 的 profile 分支。新平台还需要平台链接脚本和 `BOARD_INCLUDE_DIR`。

`#ifdef CONFIG_<ARCH>` 只应包围真正架构限定的 I/O（例如 x86 port I/O）。可跨架构的 PCI/MMIO 驱动不要按板名条件编译。

## 仅修订文档时

在仓库根目录运行 `python3 tools/docs/check_doc_citations.py` 与 `python3 tools/docs/check_doc_nav.py`。新增指南登记到文档导航；有完整 C 骨架时，用目标架构的当前头文件作语法检查。局部片段应说明其依赖的类型、调用上下文和省略的硬件实现，不将占位代码写成真实 I/O 成功。

文档导航存在迁移或既有问题时，记录改动前后的相同基线，保证本次新增链接/锚点可解析。仅文档/API 注释变化不要求重跑全套 QEMU 和硬件门禁；运行行为未变不增加一次“实机验证”的声明。扩展公共机制的验证路线见[框架维护指南](../guide/framework-development.md#7-按改动选择验证)。

## 原生执行接口的验证

修改 request/queue 或执行退出顺序时，运行 `python3 -m unittest tools.tests.test_driver_queue tools.tests.test_driver_protocol tools.tests.test_virtio_gpu_stop`。工作流验证并行请求、乱序完成、背压、依赖、取消、部分 prepare/retire 失败、停止重试和 class drain；不以模拟 callback=0 替代硬件停止证据。

`tools/a20 test smoke-driver-gpu-stop-x86_64` 在双 CPU 上执行真实 native worker/fence/lifecycle 请求链路，并检查 DRIVER_QUEUE、真实 VirtIO GPU reset/解绑/重绑与 DRM backing 保留。generic 模块部署另运行 `make PYTHON=python3 smoke-gpu-access-x86_64`，确认新增函数经模块导出实际链接和调用。早期 poll 与 worker 模式共用同一队列后端。

## 基础门禁

```sh
git diff --check
make check-firmware-model
make check-component-registry
# 修改PC输入/网络/桌面驱动时也运行：
make check-pc-drivers
make smoke-driver-lifecycle
make smoke-platform-irq-fallback
make smoke-hda
make smoke-audio-userspace
make PYTHON=python3 smoke-virtio-sound
make smoke-pci-portability
```

`check-firmware-model`、`check-pc-drivers` 在宿主编译和执行生产路径测试；`smoke-*` 会构建并启动 QEMU。按改动范围和宿主能力执行，并在提交证据中明确未运行项。

`smoke-driver-lifecycle` 用 RISC-V64 bringup 配置和 `CONFIG_DRIVER_LIFECYCLE_TEST=y` 启动合成 bus/device，验证注册、probe 失败解绑、class 发布、unregister 下线、陈旧引用返回 `-ENODEV` 和重新 probe，并以 `DRIVER_REMOVE: PASS retained=1 retry=1 devres=1` 验证失败退出保留/重试；宿主需要能运行仓库配置的 QEMU。只改平台私有轻量设备时可以不跑它，但修改 driver core、bus 或生命周期代码时必须跑。

`smoke-platform-irq-fallback` 同样用 RISC-V64 bringup 配置，但开 `CONFIG_PLATFORM_IRQ_TEST=y`，验证 `platform_device_irq()` 的四种返回（可用线号 / 无 IRQ 资源得 `-ENODEV` / `RES_IRQ` 区间得 `-EINVAL` / `request_irq` 重复占用得 `-EBUSY`）、`free_irq` 后可重新认领、`driver_irq_dispatch()` 能命中 handler，以及 DW-SDIO 的模式规则与 `a20.dw-sdio.poll=1` 参数解析。**QEMU 没有 dw-mshc 设备模型**，所以控制器侧中断从未被触发，handler 投递是经 `driver_irq_dispatch()` 模拟的；该门禁证明的是"有 IRQ 资源就注册成功、没有资源就干净回退"，不是硬件时序。

`smoke-hda` 在 x86_64 q35 上挂载 Intel HDA controller 和 duplex codec，验证 codec 拓扑识别、audio class 绑定以及一段静音 PCM 的 BDL DMA 完成。该测试使用 QEMU null audio backend，不依赖宿主声卡。

`smoke-audio-userspace` 构建完整 x86_64 用户态，在来宾 shell 中执行 `audioplay --tone 440 --duration 5000`，再由 QEMU WAV backend 捕获 HDA 输出。测试同时检查驱动绑定、命令成功、正常关机和 WAV 中的非零 PCM 采样，因此覆盖 UAPI、devfs、用户态短写循环、持续 HDA DMA 与 QEMU codec 的完整链路，且不依赖宿主声卡服务。

`smoke-virtio-sound` 使用同一个五秒用户态负载，但只挂载 QEMU `virtio-sound-pci`。它验证 modern PCI transport、PCM_INFO、SET_PARAMS/PREPARE/START、TX completion、DRAIN 后 STOP/RELEASE，以及 WAV 非静音和采样连续性。

`smoke-pci-portability` 在 LoongArch64 QEMU virt 上同时挂载 HDA 和 NVMe，验证未分配零值 BAR 的 sizing/assignment、HDA PCM DMA、NVMe queue/Identify 以及两个 class 绑定。目标创建专用 128 MiB 可丢弃镜像，写入超过单次 8 KiB bounce chunk 的数据，再执行 flush、读回和比较。`CONFIG_NVME_SMOKE_TEST` 会从 LBA 0 开始改写介质，只能由该目标配合自动生成的镜像启用。

检查源中没有板级硬编码泄漏：

```sh
rg -n "0x[0-9a-fA-F]{8}" kernel/drivers/<class>/my_driver.c
rg -n "CONFIG_BOARD_" kernel/drivers/<class>/my_driver.c
```

常量可能是协议寄存器或 ID，需要逐项解释，不是机械删除。

DRM 修改还运行 `python3 -m unittest tools.tests.test_class_stop tools.tests.test_drm_binding tools.tests.test_drm_console_owner`。主机夹具编译生产 DRM/core/class 与实际 anonfd 安装器；VFS/MM/work/调度边界为模型，不能替代 guest。`tools/a20 test smoke-driver-gpu-stop-x86_64` 在显式 embedded 双核实例中增加 `driver.drm_binding_test=1`，验证真实 VFS close、原生 worker、旧文件离线、VMA 独立存储与重绑命名空间；普通启动不执行。`test_drm_binding` 另覆盖每文件 handle/FB owner、未导入访问拒绝、重复导入、分配与输出失败回滚、正常 close、停止后的 ID 条目及 host 清理失败保留。原生 DRM 自检还确认在线新文件不调用 GEM_CLOSE 而直接 close 后，真实 VMA 成为 backing 唯一 owner。`make smoke-drm-prime` 同时要求 DRM_FILE 标记，验证用户态独立 open/显式导入/close/FB/保留映射及既有 PRIME/映射，generic 双核显示回归仍使用 `make smoke-gpu-access-x86_64`。

`tools/a20 test smoke-driver-gpu-instances-x86_64` 使用 embedded、双 CPU、两个 virtio-gpu-pci，并只在 `driver.runtime_test=1 driver.gpu_instances_test=1` 下执行：分别取 owning scanout、实际 flush，逻辑注销默认 device 后检查旧 publication 停止、peer publication/实例/DRM 文件保持在线并继续 flush，再重新登记并检查新 publication/VMO 与旧 VMA 数据。它不执行物理 PCI 热拔出，也不注入真实硬件 reset 超时。主机 `test_virtio_gpu_stop` 保留既有四条 issuer 与失败清理测试，并用两个 transport 模型验证实例独立、native allocation OOM/拒绝、失败停止和 queue fault 隔离。

ABI 12 GPU 能力修改运行 `python3 -m unittest tools.tests.test_gpu_access tools.tests.test_virtio_gpu_stop tools.tests.test_lsdc_boot tools.tests.test_drvctl_descriptor_host tools.tests.test_pc_module_boundary`，分别检查原生角色/默认策略、实际 provider 的声明、kernel descriptor guard/真实 ELF drvctl parser 与模块 imports。新增 `tools/a20 test smoke-driver-gpu-caps-x86_64` 无硬件 GPU，使用显式 runtime 参数在真实内核注册合成角色、验证固件/原生优先级、headless 排除和重新选择；它不证明 GPU 渲染或固件存储接管。双设备 `smoke-driver-gpu-instances-x86_64` 另比较真实 VirtIO 协商位与原生声明。generic 显示和 RISC-V PRIME 仍须回归，以验证 ABI 12 包实际加载。

## 架构构建矩阵

至少构建驱动目标平台和一个共享基础设施回归平台：

```sh
make ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both kernel-only -j4
make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=both kernel-only -j4
make ARCH=x86_64 ABI=both kernel-only -j4
make ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both kernel-only -j4
make check-stm32f103
```

跨架构结论分三级记录：只编译通过；设备枚举并绑定；实际 DMA/I/O 完成。只有第三级可以写“运行验证”。某架构的 board 没有 PCI enumerator 时，HDA/NVMe 编译成功只能计入第一级。

不相关架构可按改动范围缩减，但修改 driver core、class、PCI、VirtIO 或 hwapi 时应扩大矩阵。本地门禁入口见 [testing-gates.md](../../testing-gates.md)。

## probe 测试表

| 场景 | 预期 |
|---|---|
| ID 不匹配 | probe 不调用，设备保持 UNINIT |
| 必需 BAR/MMIO/IRQ 缺失 | 返回负 errno，无 ready；清理成功后清空私有指针，停止未确认则保留部分实例 |
| DMA 第 N 次分配失败 | 确认硬件/映射不再使用后回收前 N-1 次所有权；清理失败则保留句柄供重试 |
| feature/version 不支持 | 设备 FAILED/复位，无 queue 泄漏 |
| request_irq 失败 | 若支持轮询则明确降级，否则 probe 失败并清理 |
| 健康检查超时 | `-ETIMEDOUT`，设备停止后才释放 DMA |
| 成功 | `drv_priv`、class、容量/MAC/模式均有效 |

核心合成测试由 `CONFIG_DRIVER_LIFECYCLE_TEST` 覆盖基本 register/probe failure/unregister/reprobe，`CONFIG_PLATFORM_IRQ_TEST` 覆盖 platform IRQ 资源的"有则注册、无则轮询"回退契约。复杂驱动应增加可注入失败点或独立 host-side 静态门禁。

## I/O 测试

QEMU GUI 冒烟目标（`smoke-qemu-gui-*`）与 `tools/smoke_qemu_gui.py` 随 from-source GUI 栈一并退役，归档在分支 `archive/legacy-desktop`。当前 GUI 桌面由 Alpine `xfce` world 提供，验证走 `make run-world-gui PKG_WORLD=xfce`；完整门禁说明见 [testing-gates.md](../../testing-gates.md)。

block：首尾 LBA、越界、零长度、跨内部 chunk、读后写回、flush、错误恢复。不要在装有唯一数据的镜像上做破坏性测试。

network：最小/最大帧、ring full、RX 错误、连续 wrap、无包 poll、链路断开；验证 send 返回字节数，recv 无包返回 0。

input：按下/释放、modifier、多事件 buffer、ring wrap、SYN_REPORT、无事件 `-EAGAIN`、remove 后 read。

display：模式信息、pitch、全屏和边界矩形 flush、映射重叠拒绝、可见 backing 长度、remove 后 ioctl。

## 并发与清理

审查每一个共享字段由哪把锁保护，并更新 `lock-order.md`。新增代码不得在 spinlock 区间内执行 `kmalloc`、VFS、`sched`、`mdelay` 或长轮询；现有 PTY 锁内分配/释放、USB storage 锁内同步轮询以及 HID completion 锁覆盖缺口已在该文档列为待修限制，不能因已有先例而复制。触发 remove 时先禁止新 I/O，再处理 IRQ/waiter/DMA。运行 probe-remove-probe，确保静态槽和全局默认项可以复用。

## 日志证据

提交说明应附从枚举到消费的连续日志，例如：

```text
[BUS] pci ... id=8086:100e ...
[E1000] ready: mac=... link=up
[LWIP] netif en0 attached to ...
```

日志要包含构建命令、平台/VM 设备配置、成功 I/O，以及已知未验证能力。不要只附截图；串口文本更适合代码审查和回归比较。

失败 probe 协议还须运行 `python3 -m unittest tools.tests.test_driver_remove tools.tests.test_riscv_iommu_requester -v`。新增用例覆盖无分配选择、部分初始化、原错误保留、清理超时、嵌套/递归构造、子设备阻塞、旧契约兼容及真实 RISC-V domain/页表/租约保留；生命周期启动门禁新增 `DRIVER_PROBE_CLEANUP: PASS managed=1 rollback=1`。

修改退出路径还应运行 `python3 -m unittest tools.tests.test_driver_remove tools.tests.test_native_bus_remove tools.tests.test_i2c_transport_lifetime -v`。前者直接链接 C/Rust 两条生产注册表路径；bus 测试覆盖缺失/替换 PCI 身份、USB abort/remove 失败后的端点/address 保留与重试。检查失败期间无 devres 提前释放、无下一候选覆盖、无 REMOVE 完成通知；成功后再确认引用与占用回到基线。启动测试不替代真实设备停止超时与 DMA 可达性验证。

USB 初始化/回滚变更还须运行 `python3 -m unittest tools.tests.test_usb_enumeration_cleanup tools.tests.test_native_bus_lifetime tools.tests.test_native_bus_remove tools.tests.test_xhci_firmware_policy -v`。新用例使用生产 core/USB 路径验证部分 slot、描述符/配置/分配失败、地址保留、停止失败后的重试预算、部分发布及 remove 失败；xHCI 用生产 alloc_slot 回调和替代命令后端验证 Address Device 失败的 token 交还。两个相关常规门禁是 `make check-pc-drivers` 和 `make check-firmware-model`。正常硬件路径运行 `make smoke-usb-hotplug-x86_64`、`make smoke-usb-hub-x86_64`；这些是 legacy make smoke 名称，没有对应 `tools/a20 test` instance，不应将实例查找失败写作构建/启动失败。 热插拔门禁的 post-check 排除 BOOT REPORT 重放，要求首次 keyboard ready、拔出释放、再次 address=1 分配与第二次 ready 按顺序出现。QMP pump 按请求 ID 等待匹配回包，不能把 greeting/event/其他响应当作本命令完成。共享槽位忙（75）表示未启动，必须保留失败状态，不能用旧日志认证通过。工具变更须运行 `python3 -m unittest tools.tests.test_smoke_qmp tools.tests.test_smoke_ordered_expect tools.tests.test_smoke_build_paths tools.tests.test_smoke_log_dir -v`。

提前退出协议运行 `python3 -m unittest tools.tests.test_driver_remove tools.tests.test_xhci_remove tools.tests.test_riscv_iommu_requester -v`。核心用 C/Rust 注册表验证准备动作的顺序、逐项完成记录、失败保留与后续阶段重试；xHCI 使用生产 prepare/remove/slot 回收与真实核心/USB 图，注入停止/PCI 读写失败和 active transfer。生命周期门禁新增 `DRIVER_REMOVE_PREPARE: PASS partial=1 before_remove=1`。`make smoke-usb-hub-x86_64` 的 embedded fixture 现在显式传入 `driver.runtime_test=1 driver.xhci_remove_test=1`，在 hub/键鼠就绪后注销整个 xHCI 驱动、检查旧 child 句柄离线及 BME 清零、重新绑定并再次枚举，要求 `DRIVER_XHCI_REMOVE: PASS prepare=1 stopped=1 children=1 rebind=1`；普通启动不执行此动作。这些正常 QEMU 状态不等于实体设备超时失败证据。

USB 拓扑/回调变更运行 `python3 -m unittest tools.tests.test_usb_concurrency tools.tests.test_xhci_callback_lifetime tools.tests.test_xhci_endpoint_types -v`，两组新增测试已进入 `check-firmware-model` 和 `check-pc-drivers`。真实 core/USB 路径与 pthread 模拟任务竞争，验证阻塞拓扑与非阻塞进度、启动嵌套、probe 策略、注册表压缩/容量/父链、退役分配暂停及失败后重试；生产 xHCI dispatcher/prepare/abort 验证锁外回调保留、slot 独立性、自释放 URB 和停止后 admission，启用 ASan/UBSan。`python3 tools/smoke.py smoke-usb-hub-x86_64 --nr-cpus 2` 覆盖双 CPU 的真实控制器注销/重绑，下游热插拔和 HID IRQ 分别使用 `make smoke-usb-hotplug-x86_64`、`make smoke-usb-x86_64`。正常 QEMU 结果不代替实体硬件失败证据。

endpoint 生命周期变更运行 `python3 -m unittest tools.tests.test_usb_endpoint_release tools.tests.test_xhci_callback_lifetime tools.tests.test_xhci_remove tools.tests.test_pci_resources tools.tests.test_drvctl_descriptor_host -v`。新增测试执行真实 USB/core、HID probe/quiesce/remove 和 xHCI Configure/Stop/Drop/回调计数路径，替代硬件后端注入错误并启用 ASan/UBSan；PCI 用生产总线检查已持有查询的无配置写入。两组主门禁已纳入新 endpoint 测试。双 CPU hub fixture 另要求 `DRIVER_USB_INTERFACE: PASS hid=1 hub=1 siblings=1 rebind=1`，先单独注销/重绑 HID，再 hub，最后整个控制器，旧 class 句柄保持离线。ABI 10 的包由 generic 热插拔门禁构建/装载，旧 ABI descriptor 由真实 drvctl parser 拒绝。

DMA buffer/bulk 变更运行 `python3 -m unittest tools.tests.test_driver_dma_buffer tools.tests.test_xhci_bulk_lifetime tools.tests.test_driver_irq_sync -v`。生产 allocator/引用/registry 配合 PFA 故障后端验证强对齐小对象、原始 PFN/阶数回收、mask 与状态变化、跨注销/并发引用和生命周期忙时分配；真实 bulk/prepare/remove 验证私有 DMA 地址、IN/OUT/零长/64 KiB、超时后调用者释放与迟到 DMA、停止失败保留、短包和提交前退出。两组主门禁纳入新测试；整 HAL IRQ fixture 同时保留新原生函数的声明完整性检查。

`make smoke-usb-bulk-x86_64` 构建双 CPU embedded RAMFS 内核，创建专用 8 MiB 空白 USB 镜像；显式 `driver.usb_bulk_test=1` 只在 QEMU 控制器和 USB disk 身份/尺寸、待写范围初值均匹配时执行读写、比对并恢复，要求 `DRIVER_USB_BULK: PASS read=1 write=1 verify=1 restore=1`。通用 runtime 另执行真实 PFA 的 33 字节/64 KiB 对齐对象跨 owner 注销验证，在末尾同一条日志输出 `DRIVER_DMA_BUFFER: PASS align=1 owner_refs=1 lifetime=1`；hub 门禁同时要求该标记。它们不证明实体 stop 超时或任意 IOMMU/缓存后端正确。

buffer/domain 衔接运行 `python3 -m unittest tools.tests.test_driver_buffer_mapping tools.tests.test_riscv_iommu_requester tools.tests.test_riscv_iommu_queue -v`：前者运行生产 allocator/core/mapper/policy，验证整块引用、末页 padding、非连续 IOVA、cache 方向、map/unmap 失败保留和并发关闭；后两者运行实际 RISC-V provider，区分用户态 frame pin 与 native mapping 所有权，模拟高阶尾页零引用及失效失败。硬件/PFA/命令完成仍为 host 后端。

`tools/a20 test smoke-driver-iommu-native-riscv64` 在 manifest 中显式选择 embedded，使构建和启动使用同一驱动配置；增加真实高阶 buffer 的跨页 EDU DMA，要求 `DRIVER_IOMMU_BUFFER: PASS high_order=1 iova=1 dma=1 retained=1`；释放 allocator caller 引用后仍由 mapping 保留，再确认 DMA/BME 停止并解除映射。原有不连续页 DMA、非法 IOVA fault/watch 与 detach 验证继续执行。通用 runtime 的显式 direct domain 另要求 `DRIVER_DMA_BUFFER_MAP: PASS pages=2 retained=1 unmap=1`；USB hub/bulk 门禁纳入该标记，但 USB 读写本身仍使用原 identity-DMA 暂存路径。QEMU 正常失效不是实体失效超时的证据。

VMO 导入运行 `python3 -m unittest tools.tests.test_driver_dma_vmo -v`，执行生产 Rust lifecycle、C 锁/slot/PFA 原语及 DMA adapter/mapper/policy，验证源销毁、provider 中 resize、真正 pthread 的 pin/resize 排斥、缺页无部分输出/无 pin、physical/unknown 拒绝、临时引用在 OOM/policy/关闭/架构失败后交还，以及可失败回滚/部分 unmap 保留；其中另运行 7 项 Rust 生命周期测试，包含最大批次和合法 PFN 0。host 的源销毁/resize 副作用、PFA/provider/cache 为模型，不等于实际 Rust resize/destructor 已被该 fixture 执行。

通用 runtime 新增真实 VMO 的缺页拒绝、物化、direct domain 的页 1–2 映射、resize 到 0/销毁源、最后 unmap，要求 `DRIVER_DMA_VMO: PASS snapshot=1 resize=1 lifetime=1`，USB hub/bulk 门禁同时检查。RISC-V IOMMU 实例还执行真实 VMO 销毁后的 EDU 跨页 DMA，要求 `DRIVER_IOMMU_VMO: PASS snapshot=1 offset=1 dma=1 lifetime=1`；保留原有高阶 buffer、不连续页、权限、fault/watch 和 detach 验证。实体停止/失效超时仍需独立证据。

GPU publication/消费者变更运行 `python3 -m unittest tools.tests.test_gpu_access -v`，已纳入 `check-firmware-model` 与 `check-pc-drivers`。12 项方法运行生产 core/class/GPU 策略及提取的生产 fbdev/sysfs 函数，使用 pthread 与 ASan/UBSan；覆盖选择/lookup 间的元数据 pin、同 device 重绑的旧 publication、active-call drain 与不等待 lifecycle 的离线读取、超过 16 项且包含离线项的重新选择、promotion 中刷新/属性的一致 device/ops，以及错误/提前返回的引用平衡。3D 函数指针仅作存在性占位，绝不执行；不记为 3D 渲染。host framebuffer mmap 运行 NOMMU 分支，PCI/usercopy/锁与硬件使用替代后端。

`make smoke-gpu-access-x86_64` 使用双 CPU generic 内核、真实 `.a20drv` 与 virtio-gpu 2D，执行 `drm_sysfs_test` 和 `videoplay --test 32 32 1`，要求 `DRM_SYSFS: PASS` 与 `videoplay: presented 10 frames`；这是 MMU 的 framebuffer 安装/ioctl/刷新回归，不是图像视觉核验或硬件 3D 加速。embedded hub/bulk 的 runtime 另要求 `DRIVER_GPU_ACCESS: PASS publication=1 offline=1 refs=1`，验证实际 class publication 引用与离线，不把假 GPU 当成真实 framebuffer。fixture 总线按本地 driver 身份匹配，不能通配抢占其他驱动。

CPU 物理映射变更运行 `python3 -m unittest tools.tests.test_physical_mapping -v`，纳入主驱动及 PC 门禁。12 项方法运行实际 Rust mmap policy、提取的生产 C MM 桥接/PTE helper、PT unmap、fbdev MMU 分支和 udriver adapter；页表分配/CPU/锁/区间和 munmap 副作用是 host 模型。覆盖参数/地址宽度、窄 VA 错误解码、scanout 地址相加溢出、PFN 0、完整 hint 重叠、fixed 错误传播、noreplace、计数/ceiling、缓存类型与失败分配边界，ASan/UBSan 用于 C 部分；Rust 正常编译。测试内另破坏失败页清理、前缀撤销与空路径退休，必须在编译成功后触发运行时失败。旧 `python3 tools/tests/test_udriver_mmio_rollback_host.py` 委托同一套测试；`tools/host-tests/test-mmap-policy-host.sh` 保留 106 项既有 lazy mmap 检查。

`smoke-gpu-access-x86_64` 新增 `fbdev_map_test`，要求 `FBDEV_MAP: PASS fixed=1 noreplace=1 hint=1 ceiling=1 legacy=1`；在真实匿名邻接映射中替换一个 framebuffer 页，核对相邻内容、跨 gap hint 不覆盖、保护 ceiling 和旧 ioctl 的 NOREPLACE。原 sysfs/videoplay 验证继续保留。embedded hub/bulk 的 runtime 在 MMU 下另要求 `DRIVER_PHYSICAL_MAP: PASS placement=1 protect=1 accounting=1`，使用真实 VMO-owned frame 和独立 MM，检查实际叶/VMA 缓存属性、保护、固定覆盖和 total_vm/RSS，销毁全部视图后才交还源 VMO。它们不证明 framebuffer backing 跨设备解绑安全，也不代替实体 GPU DMA 或远端 TLB 并发压力。

## RAM scanout 与连续 VMO 回归

`test_vmo_contiguous.py` 的 7 项方法执行真实 C 工厂、PFA split、Rust 生命周期/最终销毁，验证专用构造、失败回滚、canonical 引用、清零 padding/多余尾部、不可变 extent/slot、pin 后销毁及并发引用。分配器、地址别名和通知为主机模型，C 开启 ASan/UBSan。`test_gpu_scanout_vmo.py` 的 7 项方法执行真实 snapshot helper、fbdev adapter、unlocked MM cap wrapper 和 virtio get/remove/release，覆盖错误输出引用、模块 ops/producer 销毁后存储存活、映射失败归还和 ceiling。locked VMO mapper、class admission、transport/IRQ 为模型，不将 stub reset 解释为停止证明。真实 class admission 与 DMA VMO 导入另有既有回归，新增 kind 4 导入方法。

双核 embedded hub 自检要求 `DRIVER_GPU_SCANOUT: PASS vmo=1 unbind=1 split=1 fork=1 pins=1`：真实 class/VMO/MM/PFA 链在 producer 解绑后只靠 VMA 存活，再 split、fork、部分 unmap、交还页引用；该合成映射尚未 fault 用户 PTE。双核 generic GPU 2D 则运行 `fbdev_map_test --retained`，要求旧 fixed/NOREPLACE/hint/ceiling/ioctl 标记加 `fork=1`，实际访问 resident framebuffer 共享页并 fork；保留 sysfs/videoplay 验证。程序默认模式继续支持旧 raw DONTFORK 路径。

RISC-V native IOMMU/EDU 门禁保留普通可变 VMO 的数据往返，另以连续 VMO 页 1–2 建立 IOVA，先销毁源对象再执行实际跨页 DMA；新增 `DRIVER_IOMMU_CONTIGUOUS_VMO: PASS canonical=1 offset=1 dma=1 lifetime=1`，原有 high-order buffer、不连续页、权限/故障/watch 标记继续保留。运行显式 embedded instance，仍为单核；停止/BME 清除后才解除映射。上述结果验证 RAM 所有权基础，不表示实体 GPU、厂商固件或完整 virtio 停止/恢复已经验证。

## VirtIO reset 与 GPU 失败退出回归

`test_virtio_gpu_stop.py` 运行生产 GPU 的 native queue/instance、四种格式、probe/stop/remove 和 reset；RAM 复用实际 VMO/PFA/Rust 回收夹具。覆盖 status 读回、超时/重试、错误 used id/index、暂存 OOM、活动 issuer 拒绝退出、每设备隔离和双设备失败停止。`test_driver_queue` 使用真实 fence，覆盖背压、多 active、乱序完成、依赖通知与撤销、取消、retire 失败和 owner 保留；`test_driver_protocol` 覆盖 hosted/MCU publication、失败执行停止先于 call drain 及 managed probe。测试数量以当前运行输出为准，不把旧阶段的“第二设备拒绝”当作 VirtIO GPU 的现状。

寄存器、时钟、IRQ 和 core probe admission 在这个 fixture 中为模型，未测试真实 IRQ 排空或硬件超时；native core 的失败 probe/remove 保留另有既有测试。`test_pc_module_boundary` 同时实际编译 virtio-gpu 包装器并核对 loader 导出，防止仅 host 提取函数通过。

`tools/a20 test smoke-driver-gpu-stop-x86_64` 显式选择 embedded、双 CPU 和真实 virtio-gpu PCI；要求 `DRIVER_GPU_STOP: PASS reset=1 unbind=1 retained=1 rebind=1`。真实 class 取得 VMO 并交给 VMA，交还 snapshot/class 调用后注销 driver，旧 publication 必须离线；旧 VMA 保留 RAM，再登记 driver 得到不同 scanout VMO，完成真实刷新并核对旧页 canary。该内核视图未 fault 用户 PTE，resident framebuffer fork 仍由 generic 2D 用户门禁覆盖。QEMU 不注入设备拒绝 reset，不表示实体 GPU 或 raw DRM 并发热拔插通过。

## 门禁失败时怎么办

| 失败门禁 | 先检查什么 | 下一步 |
|---|---|---|
| `git diff --check` 报错 | 行尾空格、tab 混用、文件尾空行 | 修复并重新 `git diff --check` |
| `make check-firmware-model` | driver core 头文件、ID 表、class ops 是否匹配规范 | 对照 [drivers/core-model.md](../guide/core-model.md) 和 `kernel/include/drivers/` 修正 |
| `make check-component-registry` | 文档与代码中注册表、生成清单与真实模块构建规则不一致 | 同步文档和实现，确保命令矩阵和真实 Makefile 目标一致 |
| `make smoke-driver-lifecycle` 失败 | 合成 bus/device 注册、probe 失败清理、unregister 路径 | 加 `CONFIG_DRIVER_LIFECYCLE_TEST=y` 日志，核对清理成功后的回收与未确认停止时的保留/重试 |
| `make smoke-platform-irq-fallback` 失败 | `platform_device_irq()` 的四种返回、`request_irq`/`free_irq` 往返、`a20.dw-sdio.poll=1` 解析 | 加 `CONFIG_PLATFORM_IRQ_TEST=y` 日志，看是哪个断言行打印了 FAIL |
| 构建矩阵中某一架构失败 | 是否用了 `#ifdef CONFIG_BOARD_` 或架构私有头 | 把板级常量移到 platform，把可跨架构代码改成通用 PCI/MMIO |
| 块/网络/input/display I/O 失败 | 是否用了唯一数据镜像、是否满足类接口语义 | 用可丢弃镜像复跑，按设备类规范逐个检查返回值 |
| 硬件验收失败 | 串口日志是否包含从 `[BUS] pci` 到类消费者的完整链路 | 不要只以“桌面黑了”或“shell 出来了”作结论 |

如果同一门禁在改动前后行为不同，先回退到已知通过的 commit，确认门禁本身没有环境变化。不要通过放大超时或放宽断言来“绕过”失败；失败点通常指向真实的资源泄漏或竞争。

## 提交清单

- [ ] ID、资源大小、DMA 地址宽度来自硬件手册并已验证。
- [ ] 驱动不依赖板级地址或架构私有 include。
- [ ] class 返回值符合 [device-classes.md](../guide/device-classes.md)。
- [ ] probe 已明确 managed 或局部回滚所有权；stop/retire/remove 失败保留资源可重试，成功后才逆序回收。
- [ ] IRQ、DMA、barrier、cache ownership 正确。
- [ ] 多实例状态不错误地放在单全局对象；若暂限单实例已记录。
- [ ] 新锁已加入 `lock-order.md`。
- [ ] 用户 ABI 由通用适配层实现并有边界检查。
- [ ] 文档、[实现状态](../../status/current/drivers-implementation-status.md)、构建和目标硬件日志一同更新。

提交前按上面的命令矩阵跑完相关门禁，并把日志贴到 PR 描述或提交说明里。

## Resident RAM 与预算验证

原生 resident 工厂修改运行 `python3 -m unittest tools.tests.test_vmo_resident tools.tests.test_vmo_contiguous tools.tests.test_driver_dma_vmo tools.tests.test_drm_binding`；resident 夹具执行生产工厂、cgroup charge/uncharge/pin 与 Rust 生命周期/析构，覆盖碎片页、大于最大 buddy 块的范围、padding 清零、任意部分失败、不可变 slot/extent、预算拒绝及 task RSS teardown。DRM 夹具的 factory/VMO 边界为模型，检查 caller cgroup 捕获、预算耗尽/原生 errno、输出失败、失败硬件清理期间保留 charge、VMA/PRIME 的原始 owner；不能把它单独当成真实物化证据。

另运行 `python3 tools/tests/test_shared_anon_host.py`、`python3 tools/tests/test_vmo_page_host.py` 与 vmo_lifecycle Rust 单测；共享匿名集成执行完整生产 C VMO/Rust 物化/resize/析构及真实 cgroup 记账，resident fault 不重复向映射任务 charge，失败 resize 不改变范围，页 pin 和预算寿命各按自身契约。

`tools/a20 test smoke-driver-iommu-native-riscv64` 增加 resident VMO，通过已物化页查询与拒绝 resize、源对象销毁后的实际 EDU 跨页 DMA 及数据/guard 比对，要求 `DRIVER_IOMMU_RESIDENT_VMO` 成功标记。该 fixture 使用无 cgroup charge 的内核 owner，不是实板 GPU 或真实 cgroup OOM 验证。`smoke-driver-gpu-stop-x86_64` 的 DRM 自检检查新 GEM 的 kind/已物化页与真实 VMA 唯一 backing owner；generic 显示及 RISC-V PRIME 仍回归。当前模块 ABI 12 与公开结构不变，新增工厂及测试不表示 GPUVM/job 或 BO 迁移已实现。

## 固定 RAM 的 owning DMA 验证

运行 `python3 -m unittest tools.tests.test_driver_resident_mapping tools.tests.test_driver_buffer_mapping tools.tests.test_driver_dma_vmo tools.tests.test_driver_dma_mapping tools.tests.test_riscv_iommu_requester tools.tests.test_riscv_iommu_queue`。新增 resident mapping 的 15 个方法执行生产 resident/contiguous 工厂、真实 cgroup charge/ref 原语、Rust 生命周期/析构与完整 DMA mapper；边界模型是 PFA/CPU alias、cache hook 和 provider。断言每次 provider 只收到选定页，覆盖整个 VMO/未映射页与 charge、多视图、kind 4 正例、其他 kind 拒绝、全部回滚/部分失败/非法 IOVA/失效重试、shutdown 与最终 owner 回调。buffer 夹具另覆盖分配设备不同于 mapping device，最后 release 在 domain 锁外重新进入 shutdown；正常映射与失败回滚保留的清理句柄均覆盖。不持有 VMO 的旧页/buffer/IOMMU 夹具使用禁止非空 VMO release 的断言 stub；实际 owning VMO 集成执行生产 C/Rust 析构。

16 个编译成功的负控分别破坏引用取得/交接、失败清理 owner、失败引用回收、VMO/buffer 最后 put、失效前提前退休、类型准入、多视图引用、first/count 视图、错误输出、锁外回调、临时 domain 引用顺序，以及旧快照不持 VMO 的契约，均须由运行时断言拒绝；负控改动还原后重新运行正例和广回归。检查 VMO owner 的 callback 与跨设备 buffer callback 是两条独立路径，不能只验证元数据字段。

RISC-V 原生 EDU 门禁现在对 resident 分支采用 owned 视图：交还 caller VMO 引用后，仍可通过句柄保留的对象核对未映射页，再执行实际跨页 DMA/数据和 guard 比对，要求 `DRIVER_IOMMU_RESIDENT_VMO: PASS canonical=1 fixed=1 dma=1 lifetime=1 owner=1`。匿名与连续分支仍回归旧页快照；连续 owning 正例由主机集成覆盖。guest 的 owner 为 NULL 内核预算，真实 cgroup 保留证据来自主机 C/Rust 集成，不是实板 GPU/cgroup OOM 或物理失效超时证据。其余必跑门禁为 x86_64 lifecycle、embedded 双核 GPU stop、generic 模块显示及 RISC-V PRIME；详见原生运行时的本轮验证记录。

## 原生 fence 通知验证

运行 `python3 -m unittest tools.tests.test_driver_fence tools.tests.test_driver_workqueue tools.tests.test_pc_module_boundary`。fence 夹具执行完整生产 fence 与 watch 代码，Park/Wake 和 spinlock 由 pthread 边界提供；17 个方法包含原来的等待/超时/80 waiters/竞争 producer 回归，以及 16 个通知契约方法。覆盖失败输出/OOM、已完成时无分配（含注入 OOM）及分配期间完成、pending/completed owner 引用、原错误传递、一次派发、128 登记的首尾/中间撤销、RUNNING 保留、terminal 后尚未选中通知的撤销、等待者先唤醒，以及各 200 次登记/撤销与 signal 竞争。模型的 IRQ 标记禁止 signal 中分配/free，callback 查询 status 验证锁外执行。RUNNING 与 waiter 进度的夹具故意暂停 callback 以暴露竞争窗口，真实 callback 不得这样等待。

17 个编译成功负控分别破坏初始 terminal 无分配、fence 引用取得/交还、登记回收、RUNNING 保护与 DONE 发布、锁外 callback、成功/错误 status、terminal 准入、取消链、输出清理、通知派发、首条登记可达性、IRQ 不回收和先唤醒 waiter；必须由运行时断言/ASan 拒绝，不能把编译失败或存活变体计为通过。还原后运行完整驱动回归；firmware 与 PC 门禁均已纳入 fence 测试。

`tools/a20 test smoke-driver-runtime-x86_64` 及 `tools/a20 test smoke-driver-runtime-smp2-x86_64` 要求 `DRIVER_FENCE_WATCH: PASS notify=1 cancel=1 error=1 work=1`：实际原生 fence 的成功/错误通知调度真实 kernel worker，并检查 pending 撤销和 terminal 拒绝。signal 在测试任务上下文执行，不将其称为真实硬件 IRQ 完成验证。继续运行 embedded 双核 GPU stop、generic 模块显示及 RISC-V PRIME；跨架构编译另行记录。该基础不表示已接入 GPU 请求队列、Linux pending sync-file 或实体 GPU。

## 通用协议与驱动架构验收

原生协议按完整使用流程验证：`python3 -m unittest tools.tests.test_driver_protocol tools.tests.test_class_binding tools.tests.test_driver_remove tools.tests.test_class_stop tools.tests.test_gpu_access tools.tests.test_drm_binding tools.tests.test_pc_module_boundary`。新集成夹具运行生产 driver core/publication/protocol，在完整与 MCU 两种 backend 下执行无 class 服务、primary+扩展互通、版本/大小/能力拒绝、重绑快照、在途调用排空、发布 OOM 与失败 probe/remove 保留。模型边界为 allocator、firmware、IRQ/Park 与系统发布，不能代替硬件停止验证。

`tools/a20 test smoke-driver-protocols-x86_64` 在 runtime 模块加载及 NIC attach 后，通过真实 VirtIO 网卡模块查询 primary 和 NET_STATS，调用发送并读取计数，要求 `DRIVER_PROTOCOLS: PASS primary=1 extension=1 send=1 stats=1`。不把发送计数解释为远端送达或 DMA 停止证明。VirtIO-net 与 RTL8139 包装器纳入实际编译/导入白名单检查，另回归 GPU stop/通用模块消费者与跨架构构建。仅在对应构建/运行证据存在时声明平台支持；无需为每个字段另造重复测试。

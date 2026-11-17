# 真机 GPU：原生驱动的实现边界与开发顺序

> 2026-11-16 范围：本页为后续原生 GPU/AMD-Vi 开发设计，功能深化不属于 1.0 目标。现有接口的内存/DMA 与停止失败缺陷仍按 [驱动台账](../status/gaps/drivers.md) 修复。

A20OS 能通过固件保留的 framebuffer 裸机显示，不等于能提交实体 GPU 命令。当前 `simplefb` 保留 GOP/DT scanout，DRM dumb buffer 路径可由 CPU 复制像素；virtio-gpu/virgl 则面向虚拟设备和宿主渲染器。两者都不是 AMD/NVIDIA 实体 GPU 驱动。

本页替换此前按驱动总行数推断“amdgpu 不可行”“不能裁剪”及“只能通过 Zink 做硬件 3D”的判断。代码规模只能提示复杂度；硬件范围、共享基础设施、可复用上游代码和验收目标才决定任务边界。

## 当前实现

- 设备核心已有总线匹配、引用、依赖、延迟 probe 和资源回收；新增 quiesce 可以在排空在途 class 调用前停止硬件并结束请求。
- DISPLAY 默认选择已有带引用的 publication/access，fbdev 单次调用与 sysfs GPU 内容生成捕获同一绑定；离线和重绑不会复用旧 publication。默认选择已使用显式原生角色，排除 headless、优先已实现渲染，同级优先原生输出而保留固件输出作为回退；不依赖 virgl 表。该查询尚不提供通用格式/队列/内存限制或提交协议。RAM framebuffer 已经 owning scanout → 连续 VMO → 普通 VMA 独立保留存储，支持 fork/split；GPU vtable 扩展后当前 ABI 12。DRM 文件、状态与调用已迁移到每 publication namespace/scoped access；失败对象保留至清理成功或旧绑定停止。每文件 handle/FB owner 与正常 close 已实现，显式 PRIME/GEM_OPEN 保留接收文件引用；完整 master/auth、共享访问权、旧 get_fb 物理视图与厂商 BO/GPUVM 仍需实现，class active call 本身不保留存储或证明硬件停止。
- CPU 物理视图已有共享 MM/Rust 安装事务，fbdev 与用户驱动复用 fixed/hint、PTE 回滚和 TLB 退休，保护 ceiling 与缓存属性保留已接入；这是借用物理存储的映射，不授予 GPU/BO 生命周期或 DMA 权限。连续 RAM scanout 已复用原生 VMO 引用；旧物理 aperture、厂商 BO/VRAM/GPUVM 与硬件停止/失败退役协议仍待实现，见 [RAM scanout](../drivers/guide/native-runtime.md#连续-ram-vmo-与-owning-scanout)。
- 原生 VMO 可通过 Rust 原子页保留和显式 map_vmo 接入 DMA domain，mapping 在源 resize/销毁后仍保留原帧；缺页不物化，物理 VMO 不作为普通 RAM 导入，不冻结 extent/转移 charge。真实 EDU 已验证源 VMO 销毁后的跨页 DMA；固定 resident RAM 工厂已补充完整物化、不可变 extent 与 VMO-owned cgroup charge，DRM backing 已采用，构造与任务归属捕获分别位于 store/GPU 锁外。BO 驻留/迁移策略、GPU 专属预算、访问权和请求同步仍待建立；独立页 pin/map_vmo 不保留 VMO 的预算 owner；新增 map_owned_vmo 可为固定 kind 4/5 保留整个 backing/原始 charge，成功及失败清理均持有，确认失效后在 domain 锁外交还。它只映射所选页，不建立 GPUVM、共享权限或硬件停止证明。
- virtio-gpu 已采用每绑定原生 DMA 实例，两个设备拥有独立 controlq/staging/IRQ/停止状态；独立逻辑注销、旧 VMA 保留、peer 持续刷新与重新登记已有双核 QEMU 验证。仍未实现多 GPU 用户节点、厂商执行队列或跨 GPU attach。
- 原生页级 DMA domain/mapping 提供设备地址转换、独立页面引用和失败回滚，direct domain 必须由平台显式确认恒等 DMA 后使用。
- 原生异步 fence 可以从 IRQ 发布完成，支持错误、等待和超时；新增可撤销的一次性 watch，可在 fence 锁外用 IRQ-safe 短通知调度 work，RUNNING 撤销保留 owner 重试。它只保留 fence，不保留 callback 代码/context，也不停止 DMA；尚未接入 BO/request 队列或 DRM syncobj/sync_file。
- 可取消工作队列、IRQ 排空和有界不可变固件读取已经实现；固件目录部署、厂商格式校验、上传和启动协议仍需完成。
- MMIO/port I/O 已有 owner 引用与受管占用；checked claim 核对已发布设备声明，并阻止后续冲突发布。独立物理存储对象已保护受管 RAM 和 ACPI 表页，阻止其作为 MMIO 使用。PCI BAR 在可能建立映射的 hook 前核对保护区，并采用全部规划通过后再提交地址与资源的事务。
- 原生 RISC-V IOMMU provider 已有 QEMU EDU 实际 DMA、fault 隔离及 worker 通知 fence 的验证；x86 AMD-Vi 已有 IVRS 策略解析、PCI/MMIO 只读探测、保留完整 aperture 的被动 PCI 绑定、PCI 桥路径/Range 补充查询、保留路由对象、完整固件声明 alias 集合的查询、逐成员检查与原子占用，以及覆盖各成员的 IVMD policy 和通用映射入口检查，尚无翻译后端。RISC-V 的结果不代表本机 AMD GPU 已有 DMA 隔离。
- 这些接口、限制与历史验证记录见 [原生驱动运行时](../drivers/guide/native-runtime.md)。它们是第一阶段基础，不代表已有 GPU 加速。
- 当前没有 Phoenix/Radeon 780M 或 AD107/RTX 4060 原生驱动、通用 AMD-Vi 后端、GPUVM 或硬件任务调度器。

## 本机首个目标

2026-11-11 的 Linux 主机只读探测得到：

| 设备 | PCI ID | Linux 驱动 | 当前内屏拓扑 |
|---|---|---|---|
| Ryzen 7 7840H Radeon 780M / Phoenix | `1002:15bf` | amdgpu | AMD eDP connected/enabled，boot_vga=1 |
| RTX 4060 Laptop / AD107M | `10de:28e0` | nvidia | NVIDIA eDP disconnected，boot_vga=0 |

复核命令：`lspci -nn -d ::0300`，读取 `/sys/class/drm/card*/device/{vendor,device,boot_vga}` 及各 connector 的 `status/enabled`。card 编号与连接状态可变，必须通过 device symlink 确认 GPU 身份。上述是 Linux 主机观测，不是 A20OS GPU 运行验证，也不保证切换 MUX 或固件设置后的拓扑相同。

优先限定为 x86_64 + Phoenix 单型号，可以减少双 GPU 呈现的工作。驱动创建 GPUVM、复位 GPU 或接管显示控制器时，固件留下的 GOP 画面可能丢失；调试需有独立日志输出。

## 分阶段验收

1. **运行时基础**：已有原生有界请求队列、class drain 前可失败 execution stop、DMA 地址/生命周期、异步完成、共享工作队列、固件读取和资源占用接口。AMD-Vi 被动控制器绑定与完整 aperture 占用已接入 PCI 生命周期，保留路由对象已组合 IVRS/requester/alias、PCI 路径和绑定快照，并可检查和原子占用完整固件声明集合、保留各成员身份/路径，构造包含 sibling alias 的独立 IVMD 约束用于通用映射预检。核心已在驱动报告 remove 失败时保留绑定/devres，并可在 probe 中显式选择部分初始化的受管失败清理；virtio-gpu 已接入原生失败清理、reset 读回和内部 staging 封闭，并有真实解绑/重绑验证；DRM 已保留失败清理对象并在旧绑定成功停止后退休，仍不替代厂商 GPU 的停止回调、硬件执行队列和请求/BO backing 寿命。真实 GPU 后端仍须实现安全停止回调。进入本机硬件初始化前，还需补齐其余 RAM/固件保留来源、通过各所属驱动完成全部 alias peer 的停止和归属变更协调，以及硬件权限/Unity/Exclusion、翻译与故障处理；具体后端须验证停止/失效失败时 DMA 页面与上下文不会提前释放，并补齐失败 probe 与关机/kexec 的 shutdown 硬件协议。
2. **硬件初始化**：固定上游版本及固件，识别实际 IP，完成所需 PSP/SMU、内存控制与中断初始化。共享电源/内存模块的依赖不能随意省略。
3. **GPU 确实执行任务**：提交 SDMA copy/fill，收到真实完成，CPU 回读逐字节比对。ioctl 成功或队列被接受不足以证明执行正确。
4. **硬件渲染**：建立 BO、GPUVM、context、GFX 提交与同步，接入现有 Mesa，离屏三角形并回读。禁止静默回落软件渲染。
5. **显示呈现**：先研究回读并复制到 GOP 的受限路径，再实现显示控制器/KMS、page flip、vblank、缓冲区共享与桌面合成。
6. **稳定性**：进程退出、多请求并发、任务超时、GPU 故障、复位及资源回收。视频编解码、休眠和双 GPU 切换分别立项。

## 参考与移植上游

Linux syscall ABI 兼容仅帮助运行 Mesa 等用户程序，不意味着能直接加载 Linux `.ko`。可以选择原生硬件实现并参考上游，也可以通过源码兼容层复用驱动；兼容层应映射到原生设备运行时，避免让 A20OS 原生驱动依赖 Linux 内部结构。

FreeBSD 的 [drm-kmod](https://github.com/freebsd/drm-kmod) 使用 LinuxKPI 移植 Linux DRM，是移植可行的先例，不表示 A20OS 已具备其内存/同步/电源基础设施。原始 amdgpu 的公共依赖和硬件模块可以从 [固定 Linux v6.12 的 amdgpu.h](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu.h) 与 [核心文档](https://docs.kernel.org/gpu/amdgpu/driver-core.html) 开始核对。

用户态复用已有 Mesa 编译器与驱动：AMD 可以使用 RadeonSI/OpenGL 和 RADV/Vulkan；Zink 是可选的 OpenGL-on-Vulkan 路线。[RADV 文档](https://docs.mesa3d.org/drivers/radv.html) 说明了用户态与内核态的职责边界。[NVK 文档](https://docs.mesa3d.org/drivers/nvk.html) 提供 NVIDIA 开源用户态路线；NVIDIA 官方开放内核模块则仍要求匹配的固件和用户态组件，参见其 [仓库说明](https://github.com/NVIDIA/open-gpu-kernel-modules)。

复用代码时逐文件核对来源、许可证及固件再分发条件；不能把所有 Linux GPU 代码一概标成同一种许可证。

## 排期口径

限定本机单 GPU、复用上游和 Mesa、由熟悉内核/DRM 的全职开发者实施时，首次可靠硬件任务通常应按月规划，基本加速桌面按多个季度规划。该估算不是测试结论，不由源码行数线性换算，也不覆盖多个 GPU 世代、视频、休眠和日常稳定性。

当前交付的是通用运行时与发现/资源管理基础，尚未达到本机 AMD-Vi 接管或 GPU 硬件初始化的验收条件。具体排期应在固定源码/固件版本、列出依赖并完成首个硬件初始化实验后修订；现有基础设施的实现与限制见[驱动状态](../status/current/drivers-implementation-status.md)，后续原生 GPU/AMD-Vi 开发按本页范围推进。

原生服务层已采用通用 endpoint/protocol：新 GPU 扩展可声明独立版本、实际表大小和能力，复用绑定的在线准入/排空，而不要求继续扩展所有类的表。既有 DISPLAY 自动提供 primary protocol；这不把 VirtIO/DRM 协议定义为通用 GPU 执行模型，也不等于 BO/GPUVM/job 已实现。通用原生 request/queue 已组合固定 VMO/原 charge、DMA buffer、fence 和 worker，VirtIO GPU 四种控制命令已复用同一执行路径；厂商 GPU 调度、主动 abort、BO/GPUVM 与恢复仍由上层/硬件协议实现，详见[请求与队列](../drivers/guide/request-queues.md)；详见[完整驱动架构](../drivers/guide/core-model.md#架构与开发入口)。

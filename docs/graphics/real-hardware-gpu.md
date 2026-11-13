# 真机 GPU：原生驱动的实现边界与开发顺序

A20OS 能通过固件保留的 framebuffer 裸机显示，不等于能提交实体 GPU 命令。当前 `simplefb` 保留 GOP/DT scanout，DRM dumb buffer 路径可由 CPU 复制像素；virtio-gpu/virgl 则面向虚拟设备和宿主渲染器。两者都不是 AMD/NVIDIA 实体 GPU 驱动。

本页替换此前按驱动总行数推断“amdgpu 不可行”“不能裁剪”及“只能通过 Zink 做硬件 3D”的判断。代码规模只能提示复杂度；硬件范围、共享基础设施、可复用上游代码和验收目标才决定任务边界。

## 当前实现

- 设备核心已有总线匹配、引用、依赖、延迟 probe 和资源回收；新增 quiesce 可以在排空在途 class 调用前停止硬件并结束请求。
- 原生页级 DMA domain/mapping 提供设备地址转换、独立页面引用和失败回滚，direct domain 必须由平台显式确认恒等 DMA 后使用。
- 原生异步 fence 可以从 IRQ 完成任务，支持错误、等待和超时；尚未接入 DRM syncobj/sync_file。
- 可取消工作队列、IRQ 排空和有界不可变固件读取已经实现；固件目录部署、厂商格式校验、上传和启动协议仍需完成。
- MMIO/port I/O 已有 owner 引用与受管占用；checked claim 核对已发布设备声明，并阻止后续冲突发布。PCI BAR 已采用全部规划通过后再提交地址与资源的事务。
- 原生 RISC-V IOMMU provider 已有 QEMU EDU 实际 DMA、fault 隔离及 worker 通知 fence 的验证；x86 AMD-Vi 目前只有 IVRS 策略解析和 PCI/MMIO 只读探测。RISC-V 的结果不代表本机 AMD GPU 已有 DMA 隔离。
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

1. **运行时基础**：已有 DMA 地址/生命周期、quiesce、异步完成、工作队列、固件读取和资源占用接口。进入本机硬件初始化前，还需建立 AMD-Vi 控制器所有权、完整 aperture 占用、硬件翻译与故障处理，并验证停止失败时仍保留 DMA 页面和上下文。
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

当前交付的是通用运行时与发现/资源管理基础，尚未达到本机 AMD-Vi 接管或 GPU 硬件初始化的验收条件。具体排期应在固定源码/固件版本、列出依赖并完成首个硬件初始化实验后修订；剩余基础设施的关闭条件见 [驱动缺口](../status/gaps/drivers.md#原生-gpu-与-amd-vi-基础设施)。

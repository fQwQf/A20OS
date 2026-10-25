# A20OS 文档首页

1.0 开发与范围决策以 [1.0 计划](releases/v1.0.md) 为入口：完成已承诺的子系统功能并验证稳定性；在共享状态、争用和复杂生命周期等确有收益的职责中使用 Rust，保留合理的 C 与硬件机制边界；LoongArch32/PPC64LE 退出支持。功能与性能验收要求见 [验收计划](releases/v1.0-validation.md)。
这些是发布要求，不能引用为当前已实现能力；早期阶段边界不豁免 1.0 验收。

第一次运行：按 [构建与运行指南](dev/build.md#首次运行linux--risc-v-64) 获取完整子模块、
安装单架构依赖与固定 Rust，运行 `python3 tools/check_quickstart.py`，再执行
`tools/a20 run qemu-riscv64 -- -j4`。进入 shell 后用 `poweroff` 退出；
`make -j4 smoke-quickstart` 自动验收 shell、外部命令和正常关机。
根目录 `make all` 是双架构发布构建。了解系统架构可继续阅读 [OS-Design.md](OS-Design.md)。

---

这里汇集了 A20OS 的设计与开发文档。A20OS 是一个**混合内核**：性能关键路径（调度、MM、VFS、页缓存）留在内核态，驱动既可按 generic profile 作为内核 `.a20drv` 包部署，也可按 embedded profile 静态链接；Native ABI 还提供用户态驱动服务与文件系统宿主机制（uxfs/ufsd）。Linux ABI兼容现有 musl 生态，Native ABI提供面向能力、句柄与事件的新接口；登记入口以各自 `syscall_table.def` 为准，数量不等同于语义完整性。混合内核的设计参考见 [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md)。

## 文档范围与权威性

- 当前事实文档：本页、[OS-Design.md](OS-Design.md)、[dev/build.md](dev/build.md)、[dev/instances.md](dev/instances.md)、[kernel/process-scheduler.md](kernel/process-scheduler.md) 和 [testing-gates.md](testing-gates.md) 以当前源码接口为目标；它们不单独证明运行结果。
- 目录分工：根目录只放入口文档（本页、贡献指南、架构总览、测试门禁与致谢/许可声明）；`dev/` 放构建、运行、调试与外部依赖；`kernel/` 放核心内核参考（调度、内存管理、Linux ABI 细节、架构特性）；其余按子系统分目录（`drivers/`、`fs/`、`net/`、`graphics/`、`native-abi/`、`cluster/`、`hypervisor/`、`platforms/` 等）。
- 设计与规划文档：`hybrid-kernel/`、`hypervisor/`、`roadmap/` 以及标题或正文明确标为 plan/design 的页面可以描述目标能力；未在源码和测试入口中落地的内容不能当作当前功能。
- 状态与进度文档：`status/` 是进度、已知问题、模块状态与测量协议的唯一存放处；`status/current/` 描述当前状态，`status/gaps/` 是**唯一的待办与缺口台账**（完成一项删一项，清空即表示承诺范围完成），`status/history/` 保留已被取代的结论性叙述，`status/measurements/` 放测量基线与协议。文档与门禁工具的当前行为及复验入口见 [docs-gates.md](status/current/docs-gates.md)。状态文档不定义接口契约，也不记录 commit SHA 或逐次运行结果。
- 研究笔记：`research/` 是 A20OS 研究方向的主张与设计材料（能力信封、预算能力、形式化议程）。其中标注"未开始/进行中"的内容是研究计划，不是系统功能。
- 历史档案：`archive/` 保留已完成里程碑的审计快照，冻结后不再随 HEAD 更新；其中的 PASS、测量数字和"已验证"只适用于各自标明的历史时点。
- 第三方材料：`kernel/external/`、`user/external/` 中的 vendor 文档解释上游背景，不定义 A20OS 当前接口。
- 冲突时的顺序：HEAD 源码和头文件优先，其次是根 `Makefile` 与 `tools/*.mk`/测试脚本，再其次是与当前提交匹配的运行日志，最后才是叙述性文档。

## 按读者选择参考页

用户操作路径只承载用途、前提、命令、可观察结果与失败语义。锁序、引用/所有权、
内部状态与回滚机制属于技术参考页或源码注释；操作页用链接引导需要实现细节的读者。
驱动开发教程中的寄存器与 DMA 契约、调试命令需要的参数布局属于其读者完成操作的前提。

- 运行系统：从 [构建](dev/build.md)、[实例](dev/instances.md)、[打包快速开始](packaging/quickstart.md)、[翻译器使用](exec-xlator/01-usage.md)、[虚拟机使用](hypervisor/03-usage.md)、[集群运维](cluster/10-operator-guide.md) 进入。
- 远程页数据共享：[可配置远程内存](cluster/63-configurable-remote-memory.md) 描述默认关闭、只读/可写模式、容量/流量限制、SDK 与部署；当前提供显式快照和事务，不提供普通线程自动跨机执行。
- 理解实现：阅读各子系统的技术契约；例如 [调度](kernel/process-scheduler.md)、[网络锁](net/network-lock-contract.md)、[翻译器内部](exec-xlator/03-internals.md)、[驱动运行时](drivers/guide/runtime-contracts.md)。
- 检查文档：`make check-doc-drift` 检查源码路径、候选内容与行号范围；`mkdocs build` 构建文档站并严格检查内部链接和锚点；`make docs` 生成参考手册 PDF。依赖与聚合门禁的边界见 [文档门禁现状](status/current/docs-gates.md)。

## 文档时效性约定

为避免每篇文档各自维护核对基线，统一约定如下：

- 文档描述的是**接口与设计契约**，以 HEAD 源码为最终依据；发现文档与源码不一致时，以源码为准，并欢迎直接修正文档。
- 单篇文档头部的"最后核实"日期表示内容在该时点与源码核对过；日期久远不等于内容失效。
- 运行类结论不写入文档，运行产物也不随仓库保存。任何结论在作为当前事实引用前，必须按 [testing-gates.md](testing-gates.md) 的入口在**当前提交**上实际重跑；已冻结的里程碑审计快照见 [archive/](archive/)。

刚接触项目的话，下面几条路径可能用得上。

## 快速开始（给新贡献者）

如果你准备第一次看代码、改 bug 或者提交补丁：

- [OS-Design.md](OS-Design.md)：总体架构、双重 ABI 与模块组织
- [dev/instances.md](dev/instances.md)：实例化构建/运行/冒烟声明（`instances/*.toml` 与 `tools/a20`）与组件注册表
- [kernel/process-scheduler.md](kernel/process-scheduler.md)：当前进程状态、CPU 所有权、Park/Wake、timeout、信号与 SMP 调度协议
- [testing-gates.md](testing-gates.md)：本地 smoke 测试与门禁检查
- [releases/v1.0.md](releases/v1.0.md)：稳定版范围、子系统完成清单、实现选择与接口冻结
- [releases/v1.0-validation.md](releases/v1.0-validation.md)：可重建参照、行为验收、性能测量与发布阻塞
- [releases/v1.0-comparison.md](releases/v1.0-comparison.md)：成对比较 runner 的 JSON 协议、统计方法和证据规则
- [status/](status/README.md)：状态、已知问题与测量协议的统一入口
- [releases/v0.17.md](releases/v0.17.md)：v0.17 候选版本范围、构建修正与发布检查
- [CONTRIBUTING.md](CONTRIBUTING.md)：贡献流程，以及代码与注释规范（语言、格式、何时该写注释、锁序与 ABI 注释约定）
- [drivers/guide/getting-started.md](drivers/guide/getting-started.md)：从第一个驱动开始理解内核接入方式
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)：当前公认需要改进的地方和切入方向
- [status/current/server-readiness.md](status/current/server-readiness.md)：把系统当服务器用之前，先读这篇（它直说了还差什么）

## 驱动开发

如果你要添加或维护某个设备驱动：

- [drivers/README.md](drivers/README.md)：驱动开发手册总入口
- [drivers/guide/core-model.md](drivers/guide/core-model.md)：device、driver 与 bus 的核心模型
- [drivers/guide/runtime-contracts.md](drivers/guide/runtime-contracts.md)：MMIO、IRQ、DMA 与锁的运行时契约
- [drivers/guide/pci-and-virtio.md](drivers/guide/pci-and-virtio.md)：PCI 与 VirtIO 设备的接入方法
- [drivers/classes/display.md](drivers/classes/display.md)：Framebuffer 与显示设备
- [graphics/3d-graphics.md](graphics/3d-graphics.md)：virtio-gpu 3D 图形加速栈的原理、内核接口与当前状态清单
- [graphics/gpu-3d-roadmap.md](graphics/gpu-3d-roadmap.md)：让 stock Mesa 挂上、把像素送上屏的路线图（VIRTGPU UAPI、ABI 门禁、排序论证、非目标）
- [graphics/host-tools.md](graphics/host-tools.md)：宿主侧三件套：`tools/check-drm-abi.sh`（DRM UAPI 门禁）、`tools/build-virglrenderer.sh`（需 root，**尚未运行**）、`tools/a20_resource.py`（启动前资源门禁）
- [graphics/real-hardware-gpu.md](graphics/real-hardware-gpu.md)：真机 GPU 的现实边界：scanout（2–4 周）与 3D 加速（不可行）是两件事，附实测驱动规模与逐 SoC 结论
- [drivers/classes/audio.md](drivers/classes/audio.md)：通用音频 UAPI、HDA、virtio-sound 与 PC Speaker

## 平台移植与运行

- [platforms/porting-guide.md](platforms/porting-guide.md)：架构与平台边界、SMP hooks 和 bring-up 验收
- [platforms/README.md](platforms/README.md)：硬件平台支持范围、默认实例与各平台部署文档
- [platforms/licheerv-nano.md](platforms/licheerv-nano.md)、[platforms/milk-v-duo.md](platforms/milk-v-duo.md)、[platforms/rk3328.md](platforms/rk3328.md)、[platforms/sun50i-h616.md](platforms/sun50i-h616.md)：RISC-V 与 ARM SBC 的平台配置、设备支持范围及部署方法
- [platforms/x86_64-pc.md](platforms/x86_64-pc.md)：x86_64 PC / 瘦客户机目标（PIRQ 与 INTx 已在 QEMU i440fx 实测；真机未验证）
- `tools/a20 boards`：列出树里全部板子及其架构、SMP、链接脚本情况
- [platforms/not-yet-ported.md](platforms/not-yet-ported.md)：Kendryte K230 与 Allwinner D1s/F133 的已核实事实与阻塞点（K230 的 PLIC 在 60 GiB，需要启动页表第三个 MMIO 槽；D1s 缺 `SOC_PERIPHERAL_IRQ` 偏移与 DDR 基址的可引用出处）
- [platforms/physical-boards.md](platforms/physical-boards.md)：VisionFive 2 与 LS2K1000 板级事实与驱动边界
- [platforms/visionfive2-boot.md](platforms/visionfive2-boot.md)：VisionFive 2 从源码构建启动链与 Flash 上板流程
- [platforms/stm32f103-port.md](platforms/stm32f103-port.md)：STM32F103 移植与硬件验证
- [platforms/virtualbox.md](platforms/virtualbox.md)：VirtualBox 平台与驱动栈

## 用户态、桌面与发行版

- [packaging/overview.md](packaging/overview.md)：包管理与镜像分发体系（apk 打包、world 组装、仓库签名、CI/CD）
- [distro/README.md](distro/README.md)：Alpine rootfs 发行版路径（A20OS 作内核、原生发行版作用户态）
- [distro/source-software-porting.md](distro/source-software-porting.md)：把新软件接进 A20OS（Alpine 上游包写进 world 清单；仍需从源码构建的只有 `user/cmds/` 与 fastfetch）与运行时验证

## Native ABI 与子系统细节

如果你想深入 Native ABI 或具体内核子系统：

- [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md)：混合内核设计参考（架构形态与分层原则）
- [hybrid-kernel/01-mechanisms.md](hybrid-kernel/01-mechanisms.md)：混合内核核心机制语义与契约（channel_call、svcmgr、共享环、用户态驱动、文件系统协议、vDSO）
- [hybrid-kernel/02-mainstream-plan.md](hybrid-kernel/02-mainstream-plan.md)：与主流混合内核的差距与演进方向
- [hybrid-kernel/06-user-fs.md](hybrid-kernel/06-user-fs.md)：用户态文件系统宿主（uxfs 代理 + ufsd 多后端：fat/ext4/iso9660/ntfs-ro）
- [status/current/hybrid-kernel-status.md](status/current/hybrid-kernel-status.md)：能力与边界清单
- [kernel/process-scheduler.md](kernel/process-scheduler.md)：进程生命周期、per-CPU runqueue、持久抢占和阻塞协议
- [native-abi/00-overview.md](native-abi/00-overview.md)：Native ABI 设计概览
- [native-abi/01-types.md](native-abi/01-types.md)：基础类型与 syscall 参数结构
- [native-abi/03-handle.md](native-abi/03-handle.md)：句柄模型与对象类型
- [native-abi/04-memory.md](native-abi/04-memory.md)：VMO/VMAR 内存模型
- [native-abi/05-ipc.md](native-abi/05-ipc.md)：Channel 与 Event Queue 机制
- [fs/vfs-edge-semantics.md](fs/vfs-edge-semantics.md)：VFS 边界语义
- [net/network-lock-contract.md](net/network-lock-contract.md)：网络栈锁契约
- [net/network-config-design.md](net/network-config-design.md)：网络配置设计（`a20.*` 键、`a20.tcpmode` 两档模式、运行时控制面）
- [net/net-lanes.md](net/net-lanes.md)：网络 lane 设计与阶段顺序（阶段 A/B 已落地；含缓冲改造被锁契约挡住的原因）

### 外来架构翻译通道（exec-xlator）

`execve` 一个外来架构（x86_64 / aarch64）的 Linux 可执行文件，内核把它改写成一次对用户态翻译器的就地 re-exec。不了解本系统的人从索引点进来可以按角色直接选入口：

- [exec-xlator/README.md](exec-xlator/README.md)：机制说明与三分钟快速上手（入口页）
- [exec-xlator/01-usage.md](exec-xlator/01-usage.md)：使用手册——三层开关、启动键与 argv 模板语法、`/proc/a20/xlator`、无交叉编译器的验证路径、排查表
- [exec-xlator/02-integration.md](exec-xlator/02-integration.md)：接入指南——翻译器需要满足什么契约、加一个 guest 架构要改几处、Rosetta 型包装器与 Prism 型 AOT 的可行性
- [exec-xlator/03-internals.md](exec-xlator/03-internals.md)：内核内部——形态取舍、代码地图、`xlator_*` API 契约、并发不变量、已知代价

### Linux ABI 兼容层

- `kernel/abi/linux/syscall_coverage.md`：Linux syscall 的逐项兼容等级与证据（含四主线架构编号覆盖说明）
- `kernel/abi/linux/compat_notes.md`：Linux ABI 兼容性说明（高风险 partial 区域、文件化接口、占位符决策记录）
- [OS-Design.md](OS-Design.md)：ABI 分层原则（核心实现、ABI 薄包装）与两套 ABI 对比

### 虚拟化（hypervisor）

共享 VM/vCPU 核心现有 RV64 H、RV32 H、AArch64 EL2、ARM32 HYP、AMD SVM 与
Intel VMX 后端，多架构小载荷的双 vCPU 并行运行已验证。Intel VMX 使用 Bochs
软件 CPU，尚无 Intel 实机验证。LoongArch LVZ 是默认关闭的实验实现，仅通过
编译与无 LVZ 负控，没有实际 LVZ guest 执行证据。完整 OS guest 的 ELF/FDT
装载和管理器工作流仍面向 RV64，完整 OS SMP、虚拟 GIC/APIC 和长稳尚未验收。

- [hypervisor/README.md](hypervisor/README.md)：文档导航、当前后端与验收边界。
- [hypervisor/03-usage.md](hypervisor/03-usage.md)：依赖、构建、QEMU / Bochs、
  bootargs、日志判据、用户态最小 guest、VMM/SMP 调用顺序和故障排查。
- [hypervisor/08-multiarch-backends.md](hypervisor/08-multiarch-backends.md)：
  各后端 guest 模式、页表、寄存器与设备边界。
- [hypervisor/09-smp-validation.md](hypervisor/09-smp-validation.md)：实际运行
  矩阵、复现命令、SMP 方法及镜像/日志哈希清单。
- [status/current/hypervisor-implementation-status.md](status/current/hypervisor-implementation-status.md)：
  当前实现、完整 guest 场景和未完成项。
- [hypervisor/06-host-runtime.md](hypervisor/06-host-runtime.md)、
  [hypervisor/07-guest-supervisor.md](hypervisor/07-guest-supervisor.md)：RV64
  命名实例、磁盘与控制台隔离、双 guest 管理及控制协议。
- [hypervisor/00-design.md](hypervisor/00-design.md)、
  [hypervisor/01-a20os-guest.md](hypervisor/01-a20os-guest.md)、
  [hypervisor/02-roadmap.md](hypervisor/02-roadmap.md)、
  [hypervisor/04-platform-design.md](hypervisor/04-platform-design.md)：基础设计、
  历史阶段与后续平台目标，以当前使用指南和运行记录为状态依据。

多架构运行入口为 `python3 tools/check_hyp_multiarch.py --arch ARCH --cpus 2
--build`，VMX 另用 Bochs runner。旧 `smoke-hyp-*` 门禁保留 RV64 完整 guest
场景覆盖；不能将多架构小载荷通过等同于这些完整 guest 场景全部通过。
Native 与 Linux 扩展 ABI 共享内核核心，但编号、handle 和权限包装各自独立；
当前没有 A20 `/dev/kvm` 兼容层。

## 当前契约与边界记录

- [kernel/mm/fork-mprotect-cow.md](kernel/mm/fork-mprotect-cow.md)：fork、COW 与 `mprotect` 的当前锁和帧引用契约，以及隔离回归范围。
- [kernel/arch/aarch64-fex-preconditions.md](kernel/arch/aarch64-fex-preconditions.md)：AArch64 auxv 与保护提示的接口审计范围；明确哪些证据尚不能推出 FEX 支持。

## 研究与项目背景

如果你关注研究思路或工程路线图：

- [research/00-index.md](research/00-index.md)：A20OS 研究笔记的索引与论点（能力信封为核心贡献）
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)：当前改进清单与开发路线图
- [status/README.md](status/README.md)：状态、缺口台账、历史记录与测量协议的收录规则与索引
- [status/current/server-readiness.md](status/current/server-readiness.md)：面向服务器部署的当前能力边界、结构性限制与阻塞项排序
- [security/hardening.md](security/hardening.md)：安全加固状态，含「诚实性原则（fail-closed）」
- [dev/external-dependencies.md](dev/external-dependencies.md)：外部依赖与协议说明
- [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md)：第三方项目致谢与出处说明
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)：第三方组件许可证集中声明
- [archive/](archive/)：历史里程碑的审计快照（冻结档案）

---

这些文档既包含当前接口说明，也包含研究设计与历史记录。引用能力或测试结论时，先按上面的范围和时效性约定判断其性质。

# A20OS 文档首页

五分钟上手：先读 [OS-Design.md](OS-Design.md) 把握整体架构，再跑 `make ARCH=riscv64 BOARD=qemu-virt-riscv64 run` 把开发镜像在 QEMU 里启动起来。根目录 `make all` 是双架构提交构建，不是这个开发流程的缩写。

---

这里汇集了 A20OS 的设计与开发文档。A20OS 是一个**混合内核**：性能关键路径（调度、MM、VFS、页缓存）留在内核态，驱动既可按 generic profile 作为内核 `.a20drv` 包部署，也可按 embedded profile 静态链接；Native ABI 还提供用户态驱动服务机制。Linux ABI（`syscall_table.def` 登记 343 个 syscall）兼容现有 musl 生态，Native ABI（登记 126 个 syscall）探索面向能力、句柄与事件的新接口。混合内核的设计参考见 [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md)。

## 文档范围与权威性

- **当前事实文档**：本页、[OS-Design.md](OS-Design.md)、[build.md](build.md)、[process-scheduler.md](process-scheduler.md) 和 [testing/testing-gates.md](testing/testing-gates.md) 以当前源码接口为目标；它们不单独证明运行结果。
- **设计与规划文档**：`hybrid-kernel/`、`roadmap/` 以及标题或正文明确标为 plan/design 的页面可以描述目标能力；未在源码和测试入口中落地的内容不能当作当前功能。
- **审计与展示快照**：`testing/*-audit.md`、带日期的平台进展、硬件观察和 `slides.tex` 等展示材料保留历史证据。其数字、PASS 或“已验证”只适用于页面标明的快照，不自动外推到 HEAD。
- **第三方与研究材料**：`kernel/external/`、`user/external/` 中的 vendor 文档及 `research/` 笔记解释上游或调研背景，不定义 A20OS 当前接口。
- **冲突时的顺序**：HEAD 源码和头文件优先，其次是根 `Makefile` 与 `tools/*.mk`/测试脚本，再其次是与同一提交匹配的干净运行证据，最后才是叙述性文档。本轮源码审计基线 `e33c3219` 尚无一套与之完全匹配的正式全流程测试；最新完整、干净的正式证据是 `f9732348`，后续提交的结果不得由该快照推定。

如果你刚接触项目，不知道自己该看什么，下面四条路径应该能帮到你。

## 快速开始（给新贡献者）

如果你准备第一次看代码、改 bug 或者提交补丁：

- [OS-Design.md](OS-Design.md)：总体架构、双重 ABI 与模块组织
- [process-scheduler.md](process-scheduler.md)：当前进程状态、CPU 所有权、 Park/Wake、timeout、信号与 SMP 调度协议
- [testing/testing-gates.md](testing/testing-gates.md)：本地 smoke 测试与门禁检查
- [drivers/guide/getting-started.md](drivers/guide/getting-started.md)：从第一个驱动开始理解内核接入方式
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)：当前公认需要改进的地方和切入方向

## 驱动开发

如果你要添加或维护某个设备驱动：

- [drivers/README.md](drivers/README.md)：驱动开发手册总入口
- [drivers/guide/core-model.md](drivers/guide/core-model.md)：device、driver 与 bus 的核心模型
- [drivers/guide/runtime-contracts.md](drivers/guide/runtime-contracts.md)：MMIO、IRQ、DMA 与锁的运行时契约
- [drivers/guide/pci-and-virtio.md](drivers/guide/pci-and-virtio.md)：PCI 与 VirtIO 设备的接入方法
- [drivers/classes/display.md](drivers/classes/display.md)：Framebuffer 与显示设备
- [gpu/3d-graphics.md](gpu/3d-graphics.md)：virtio-gpu virgl 3D 图形加速栈
- [drivers/classes/audio.md](drivers/classes/audio.md)：通用音频 UAPI、HDA、virtio-sound 与 PC Speaker

## 平台移植与运行

- [platforms/porting-guide.md](platforms/porting-guide.md)：架构与平台边界、SMP hooks 和 bring-up 验收
- [platforms/stm32f103-port.md](platforms/stm32f103-port.md)：STM32F103 移植与硬件验证
- [platforms/virtualbox.md](platforms/virtualbox.md)：VirtualBox 平台与驱动栈

## Native ABI 与子系统细节

如果你想深入 Native ABI 或具体内核子系统：

- [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md)：混合内核设计参考（架构形态与分层原则）
- [hybrid-kernel/01-mechanisms.md](hybrid-kernel/01-mechanisms.md)：混合内核核心机制语义与契约（channel_call、svcmgr、共享环、用户态驱动、vDSO）
- [hybrid-kernel/02-mainstream-plan.md](hybrid-kernel/02-mainstream-plan.md)：与主流混合内核的差距与演进方向
- [hybrid-kernel/STATUS.md](hybrid-kernel/STATUS.md)：能力与边界清单
- [process-scheduler.md](process-scheduler.md)：进程生命周期、per-CPU runqueue、持久抢占和阻塞协议
- [native-abi/00-overview.md](native-abi/00-overview.md)：Native ABI 设计概览
- [native-abi/01-types.md](native-abi/01-types.md)：基础类型与 syscall 参数结构
- [native-abi/03-handle.md](native-abi/03-handle.md)：句柄模型与对象类型
- [native-abi/04-memory.md](native-abi/04-memory.md)：VMO/VMAR 内存模型
- [native-abi/05-ipc.md](native-abi/05-ipc.md)：Channel 与 Event Queue 机制
- [fs/vfs-edge-semantics.md](fs/vfs-edge-semantics.md)：VFS 边界语义
- [net/network-lock-contract.md](net/network-lock-contract.md)：网络栈锁契约
- [net/network-config-design.md](net/network-config-design.md)：网络配置设计

### Linux ABI 兼容层

- `kernel/abi/linux/syscall_coverage.md`：343 个 syscall 的逐项兼容等级（全部登记、保守 `partial`，含四主线架构编号覆盖说明）
- `kernel/abi/linux/compat_notes.md`：Linux ABI 兼容性说明（高风险 partial 区域、文件化接口、占位符决策记录）
- [OS-Design.md](OS-Design.md)：ABI 分层原则（核心实现、ABI 薄包装）与两套 ABI 对比

## 活动、研究与项目背景

如果你关注活动设计、研究思路或工程路线图：

- [research/00-index.md](research/00-index.md)：Native ABI 研究笔记的阅读索引
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)：当前改进清单与开发路线图
- [project/external-dependencies.md](project/external-dependencies.md)：外部依赖与协议说明
- [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md)：第三方项目致谢与出处说明
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)：第三方组件许可证集中声明

---

这些文档既包含当前接口说明，也保留工程过程记录。引用能力或测试结论时，请先按上面的范围和证据边界判断其性质。

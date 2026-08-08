# A20OS 文档首页

五分钟上手：先读 [OS-Design.md](OS-Design.md) 把握整体架构，再跑 `make ARCH=riscv64 run` 把内核在 QEMU 里启动起来。

---

这里汇集了 A20OS 的设计与开发文档。A20OS 是武汉大学 A20 战队开发的**混合内核**：性能关键路径（调度、MM、VFS、页缓存）留在内核态，服务监管与低速设备驱动以可崩溃、可重启的用户态服务运行；Linux ABI（`syscall_table.def` 登记 258 个 syscall）兼容现有 musl 生态，Native ABI（登记 126 个 syscall）探索面向能力、句柄与事件的新接口。混合内核的设计参考见 [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md)。

如果你刚接触项目，不知道自己该看什么，下面四条路径应该能帮到你。

## 快速开始（给新贡献者）

如果你准备第一次看代码、改 bug 或者提交补丁：

- [OS-Design.md](OS-Design.md)：总体架构、双重 ABI 与模块组织
- [process-scheduler.md](process-scheduler.md)：当前进程状态、CPU 所有权、 Park/Wake、timeout、信号与 SMP 调度协议
- [testing/testing-gates.md](testing/testing-gates.md)：本地 smoke 测试与门禁检查
- [drivers/getting-started.md](drivers/guide/getting-started.md)：从第一个驱动开始理解内核接入方式
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)：当前公认需要改进的地方和切入方向

## 驱动开发

如果你要添加或维护某个设备驱动：

- [drivers/README.md](drivers/README.md)：驱动开发手册总入口
- [drivers/core-model.md](drivers/guide/core-model.md)：device、driver 与 bus 的核心模型
- [drivers/runtime-contracts.md](drivers/guide/runtime-contracts.md)：MMIO、IRQ、DMA 与锁的运行时契约
- [drivers/pci-and-virtio.md](drivers/guide/pci-and-virtio.md)：PCI 与 VirtIO 设备的接入方法
- [drivers/display.md](drivers/classes/display.md)：Framebuffer 与显示设备
- [drivers/audio.md](drivers/classes/audio.md)：通用音频 UAPI、HDA、virtio-sound 与 PC Speaker

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

## 赛事、研究与项目背景

如果你关注赛事设计、研究思路或工程路线图：

- [research/00-index.md](research/00-index.md)：Native ABI 研究笔记的阅读索引
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md)：当前改进清单与开发路线图
- [project/external-dependencies.md](project/external-dependencies.md)：外部依赖与协议说明
- [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md)：第三方项目致谢与出处说明
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)：第三方组件许可证集中声明

---

这些文档是工程过程中的真实记录，有的已经比较完整，有的还在随着代码一起更新。如果你发现描述与代码不一致，请优先以源码和头文件为准，也欢迎告诉我们。

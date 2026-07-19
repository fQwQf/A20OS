# A20OS 文档首页

五分钟上手：先读 [OS-Design.md](OS-Design.md) 把握整体架构，再跑 `make ARCH=riscv64 run` 把内核在 QEMU 里启动起来。

---

这里汇集了 A20OS 的设计与开发文档。我们是武汉大学 A20 战队的参赛内核，正在尝试把宏内核的执行效率和微内核的抽象能力放在同一个内核里：用 Linux ABI 兼容现有生态，同时用 Native ABI 探索面向能力、句柄与事件的新接口。

如果你刚接触项目，不知道自己该看什么，下面四条路径应该能帮到你。

## 快速开始（给新贡献者）

如果你准备第一次看代码、改 bug 或者提交补丁：

- [OS-Design.md](OS-Design.md) —— 总体架构、双重 ABI 与模块组织
- [testing/testing-gates.md](testing/testing-gates.md) —— 本地 smoke 测试与门禁检查
- [drivers/getting-started.md](drivers/getting-started.md) —— 从第一个驱动开始理解内核接入方式
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md) —— 当前公认需要改进的地方和切入方向

## 驱动开发

如果你要添加或维护某个设备驱动：

- [drivers/README.md](drivers/README.md) —— 驱动开发手册总入口
- [drivers/core-model.md](drivers/core-model.md) —— device、driver 与 bus 的核心模型
- [drivers/runtime-contracts.md](drivers/runtime-contracts.md) —— MMIO、IRQ、DMA 与锁的运行时契约
- [drivers/pci-and-virtio.md](drivers/pci-and-virtio.md) —— PCI 与 VirtIO 设备的接入方法
- [drivers/display.md](drivers/display.md) —— Framebuffer 与显示设备

## Native ABI 与子系统细节

如果你想深入 Native ABI 或具体内核子系统：

- [native-abi/00-overview.md](native-abi/00-overview.md) —— Native ABI 设计概览
- [native-abi/01-types.md](native-abi/01-types.md) —— 基础类型与 syscall 参数结构
- [native-abi/03-handle.md](native-abi/03-handle.md) —— 句柄模型与对象类型
- [native-abi/04-memory.md](native-abi/04-memory.md) —— VMO/VMAR 内存模型
- [native-abi/05-ipc.md](native-abi/05-ipc.md) —— Channel 与 Event Queue 机制
- [fs/vfs-edge-semantics.md](fs/vfs-edge-semantics.md) —— VFS 边界语义
- [net/network-lock-contract.md](net/network-lock-contract.md) —— 网络栈锁契约
- [net/network-config-design.md](net/network-config-design.md) —— 网络配置设计

## 赛事、研究与项目背景

如果你关注赛事设计、研究思路或工程路线图：

- [research/00-index.md](research/00-index.md) —— Native ABI 研究笔记的阅读索引
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md) —— 当前改进清单与开发路线图
- [project/external-dependencies.md](project/external-dependencies.md) —— 外部依赖与协议说明
- [platforms/stm32f103-port.md](platforms/stm32f103-port.md) —— STM32F103 移植与 bring-up 记录
- [platforms/virtualbox.md](platforms/virtualbox.md) —— VirtualBox 驱动栈与运行说明

---

这些文档是工程过程中的真实记录，有的已经比较完整，有的还在随着代码一起更新。如果你发现描述与代码不一致，请优先以源码和头文件为准，也欢迎告诉我们。

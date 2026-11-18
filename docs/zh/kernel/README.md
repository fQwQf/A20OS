# 内核核心子系统文档索引

本目录汇集 A20OS 内核核心机制的技术规范与实现参考，涵盖进程调度、内存管理、Linux ABI 兼容细节、命名空间与体系架构扩展。

---

## 1. 进程管理与调度

| 文档 | 说明 |
|---|---|
| [process-scheduler.md](process-scheduler.md) | 进程生命周期、per-CPU runqueue、Park/Wake 协议、超时处理与信号分发 |
| [eevdf-scheduler.md](eevdf-scheduler.md) | EEVDF（Earliest Eligible Virtual Deadline First）调度算法设计与实现 |

## 2. 内存管理（MM）

| 文档 | 说明 |
|---|---|
| [mm/fork-mprotect-cow.md](mm/fork-mprotect-cow.md) | Fork、mprotect 与 Copy-On-Write 并发约束与锁模型 |
| [mm/slab-cpu-cache.md](mm/slab-cpu-cache.md) | Slab 分配器 CPU 本地缓存与归属模型 |

## 3. Linux ABI 兼容细节

- [控制终端与作业控制](linux-abi/tty-jobcontrol.md)：会话归属、前后台访问、挂断撤销与重复冷启动门禁。

| 文档 | 说明 |
|---|---|
| [linux-abi/cpuinfo.md](linux-abi/cpuinfo.md) | `/proc/cpuinfo` 与在线 CPU 状态枚举语义 |
| [linux-abi/interface-enumeration.md](linux-abi/interface-enumeration.md) | 网络接口与 sysfs/procfs 设备枚举兼容 |
| [linux-abi/unix-ancillary.md](linux-abi/unix-ancillary.md) | Unix 域套接字 `SCM_RIGHTS` 与附属控制数据传递 |
| [linux-abi/child-exit-notification.md](linux-abi/child-exit-notification.md) | 子进程退出通知、`SIGCHLD` 与 waitid 语义 |
| [linux-abi/keyring.md](linux-abi/keyring.md) | Linux Keyring 密钥环子系统与 Rust 迁移契约 |

## 4. 命名空间与隔离

| 文档 | 说明 |
|---|---|
| [resource-namespaces.md](resource-namespaces.md) | 资源命名空间（PID / mount / net）分层模型与继承 |

## 5. 调试与内核扩展

| 文档 | 说明 |
|---|---|
| [debugging-internals.md](debugging-internals.md) | 内核调试机制内部实现（GDB stub、断点与寄存器状态机） |
| [日志与诊断](logging/README.md) | 统一入口：日常使用、持久日志、Boot Report、移植与性能分析、验证记录 |
| [extensions.md](extensions.md) | 内核可扩展机制与动态模块注册 |
| [kexec-hot-update.md](kexec-hot-update.md) | kexec 热更新与内核平滑重启机制 |

## 6. 架构特定规范

| 文档 | 说明 |
|---|---|
| [arch/aarch64-fex-preconditions.md](arch/aarch64-fex-preconditions.md) | AArch64 架构运行 FEX-Emu 模拟器的内核前置条件 |
| [arch/x86_64-cpu-id.md](arch/x86_64-cpu-id.md) | x86_64 CPUID 特性探测与上报契约 |

## 7. 外来架构二进制翻译（exec-xlator）

| 文档 | 说明 |
|---|---|
| [exec-xlator/README.md](exec-xlator/README.md) | 机制说明与三分钟快速上手 |
| [exec-xlator/01-usage.md](exec-xlator/01-usage.md) | 使用手册：三层开关、启动键与 argv 模板语法 |
| [exec-xlator/02-integration.md](exec-xlator/02-integration.md) | 接入指南：翻译器契约与 guest 架构扩展 |
| [exec-xlator/03-internals.md](exec-xlator/03-internals.md) | 内核内部：形态取舍、代码地图与 `xlator_*` API 契约 |
| [exec-xlator/04-extending.md](exec-xlator/04-extending.md) | 扩展手册：添加新架构与 native-ABI 翻译器演进 |

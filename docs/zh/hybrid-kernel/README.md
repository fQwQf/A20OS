# 混合内核架构文档索引

A20OS 采用高级混合内核（Hybrid Kernel）架构：将调度、内存管理、页缓存与 TCP/IP 核心放在内核态以保障极速调用性能；同时将设备驱动与文件系统后端设计为既可静态/内核链接，也可作为用户态独立服务运行的形态。

---

## 规范与演进设计

| 文档 | 说明 |
|---|---|
| [00-design.md](00-design.md) | 混合内核总体架构设计、边界划分与演进原则 |
| [01-mechanisms.md](01-mechanisms.md) | 核心机制语义：Channel Call、服务管理器（svcmgr）、共享环与 vDSO |
| [02-mainstream-plan.md](02-mainstream-plan.md) | 与主流混合内核的对比分析与演进规划 |
| [03-refactor-plan.md](03-refactor-plan.md) | 模块化重构规划与解耦路线 |
| [04-dual-placement.md](04-dual-placement.md) | 双重放置策略（内核态驱动与用户态驱动的灵活部署） |
| [05-idl-and-personality.md](05-idl-and-personality.md) | IDL 接口描述语言与操作系统人格层（Personality）设计 |
| [06-user-fs.md](06-user-fs.md) | 用户态文件系统宿主（uxfs 代理与 ufsd 多后端架构） |

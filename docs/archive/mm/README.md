# 内存管理历史演进归档 (Memory Management Archive)

本目录归档 A20OS 内存管理子系统从早期两级模型（软件 VMA 区间树 + 硬件页表）演进迁移至 CortenMM 式单级页表模型过程中的历史工作记录、调试日志与实验草稿。

---

## 归档文档清单

| 归档文档 | 原始阶段与内容 | 对应当前权威规范 |
| :--- | :--- | :--- |
| [mm-single-level-migration-log.md](mm-single-level-migration-log.md) | **单级模型迁移实施日志（2026-09 至 2026-10）**：共 4500+ 行，完整记录了 CortenMM 论文核对、Radix 树状态字节设计、中间阶段缺陷排查与段表最终合入的原始实验数据。 | [docs/kernel/mm/single-level-page-table.md](../../kernel/mm/single-level-page-table.md) |

---

> [!NOTE]
> 归档文档保留用于工程演进追溯与历史缺陷复盘，不代表内核当前最新接口规范。阅读系统内存架构请以 [`docs/kernel/mm/`](../../kernel/mm/) 中的标准文档为准。

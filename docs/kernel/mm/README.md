# 内存管理子系统 (Memory Management)

A20OS 内存管理子系统负责物理内存分配、虚拟地址空间管理以及多核环境下的并发控制。内核采用现代化单级页表模型（基于 SOSP '25 CortenMM 思想），将硬件页表与附加状态作为虚存状态的唯一权威，消除传统双级抽象的同步复杂性。

---

## 模块文档目录

| 文档 | 内容概要与契约范围 |
| :--- | :--- |
| [single-level-page-table.md](single-level-page-table.md) | **架构总览**：单级页表模型（`MM_AS_MODEL`）、事务式游标（`mm_cursor_t`）、元数据状态字节与单一段表记录（`struct mm_seg`） |
| [fork-mprotect-cow.md](fork-mprotect-cow.md) | **写时复制与保护控制**：`fork` 地址空间派生、叶节点锁保护下的 PTE 重写与写时复制状态同步 |
| [slab-cpu-cache.md](slab-cpu-cache.md) | **对象分配缓存**：每 CPU 本地 Slab 缓存归属、无锁快速路径与多核伸缩性 |

---

## 核心架构分层

```
+-------------------------------------------------------+
|                    用户虚拟地址空间                     |
|           mm_struct_t / mm_seg_t (映射段表记录)          |
+-------------------------------------------------------+
                            |
           事务式游标接口 (mm_cursor_t)
                            |
                            v
+-------------------------------------------------------+
|                 硬件多级基数页表 (Radix Tree)            |
|       每页表页绑定 pt_meta_t (MCS 队列锁 + cls[] 状态字节) |
+-------------------------------------------------------+
                            |
           物理页帧分配器 (Buddy System + Slab Cache)
                            |
                            v
+-------------------------------------------------------+
|                     底层物理内存                       |
+-------------------------------------------------------+
```

---

## 关键代码与头文件

- **核心虚拟内存定义**：[`kernel/include/mm/vm.h`](../../../kernel/include/mm/vm.h)（`mm_struct_t`、`mm_seg_t`、锁模型规则）
- **页表与游标契约**：[`kernel/include/mm/pt.h`](../../../kernel/include/mm/pt.h)（`pt_meta_t`、`mm_cursor_t` 接口与不变量）
- **缺页异常处理**：[`kernel/mm/fault.c`](../../../kernel/mm/fault.c)（状态驱动快速分派与 COW）
- **物理分配器**：[`kernel/mm/frame.c`](../../../kernel/mm/frame.c)、[`kernel/mm/slab.c`](../../../kernel/mm/slab.c)

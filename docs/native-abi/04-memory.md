# A20OS Native ABI：内存子系统设计

> 本文档记录 Native ABI 当前内存实现及目标抽象，已按 2026-08 的 `sys_core.c`、`sys_native_mm.c`、`kernel/mm/vmo.c` 和 `abi/native/vmar.c` 核对。当前 VMAR 不是独立层级对象；部分常量和 flag 仍只是保留接口。

---

## 1. 内存模型概述

Native ABI 在核心 VMA 之上增加可传递的 VMO 抽象，但当前尚未把所有映射统一为 VMO：

- **VMO (Virtual Memory Object)**：可由 MEMORY handle 引用的物理页容器，独立于地址空间；`vm_map(source=NULL)` 也会创建一个无返回 handle 的匿名 VMO。
- **VMAR (Virtual Memory Address Region)**：当前是 Native syscall 对核心 `vm_area_t`/`mm_mmap_*` 的称呼和薄包装，不存在独立 VMAR handle 或层级树。VMO 通过 `vm_map` 建立 `VM_VMO` VMA。
- **普通 VMA**：`vm_alloc` 建立普通匿名 VMA，FILE/DEVICE source 建立 `VM_FILE` VMA；二者都不是 MEMORY-handle-backed VMO。

```text
进程地址空间 (VMAR)
┌──────────────────────────────────────────────────┐
│ 0x0000_0000_0000                                 │
│  ... (不可映射)                                   │
│ 0x0001_0000_0000  ┌──────────────┐               │
│                    │ VMA: code    │ ← ELF/file    │
│ 0x0001_0001_0000  └──────────────┘               │
│                    ┌──────────────┐               │
│                    │ VMA: heap    │ ← anonymous   │
│                    └──────────────┘               │
│  ...                                              │
│ 0x7fff_0000_0000  ┌──────────────┐               │
│                    │ VMA: shared  │ ← VMO handle  │
│ 0x8000_0000_0000  └──────────────┘               │
│  ... (内核空间)                                   │
└──────────────────────────────────────────────────┘
```

### 1.1 与 POSIX mmap 的区别

| POSIX mmap | Native ABI |
|-----------|-----------|
| 匿名映射没有独立可传递对象 | `vm_alloc` 同样建立普通匿名 VMA；需要共享时显式创建 VMO handle |
| 共享内存通常通过 SysV/POSIX shm 或共享文件 | Native 共享路径是传递 MEMORY handle，再由接收方 `vm_map` |
| 保护位与映射绑定 | 当前保护位也保存在 VMA；source handle rights 只收紧首次 `vm_map` |
| `mremap`、`madvise`、`msync` 分别操作映射 | Native 提供对应操作，但 `vm_remap`/`vm_advise`/`vm_flush` 都只有受限语义 |

---

## 2. VMO (Virtual Memory Object)

### 2.1 定义

VMO 是物理页面的容器：

```c
typedef struct a20_vmo {
    refcount_t       refcount;
    uint64_t         size;              /* 逻辑大小（字节） */
    uint64_t         phys_size;         /* 已分配物理内存 */
    uint32_t         type;              /* VMO 类型 */
    uint32_t         options;           /* 创建选项 */
    spinlock_t       lock;              /* 保护 pages 数组 */
    pfn_t           *pages;             /* canonical frame 数组 */
    uint32_t         page_count;
    struct cg_node  *charge_cg;
    uint64_t         charged_pages;
} a20_vmo_t;
```

### 2.2 VMO 类型

| 类型 | 说明 | 来源 |
|------|------|------|
| `A20_VMO_ANONYMOUS` | 匿名 VMO，按需分配 canonical frame | `vm_create_object` 或 `vm_map(source=NULL)` |
| `A20_VMO_PHYSICAL` | 预留的 physical VMO 类型 | 当前没有 `vmo_create(VMO_PHYSICAL, ...)` 调用者；`device_alloc_dma` 创建 ANONYMOUS VMO 后手工安装连续页 |
| `A20_VMO_PAGED` | 预留的 paged VMO 类型 | 当前 file/device `vm_map` 直接走 `mm_mmap_file`，不会创建此类 VMO |

### 2.3 VMO 操作

- **创建**：`vm_create_object` 或 `vm_map(source=NULL)` 创建匿名 VMO；`vm_alloc` 当前直接调用核心匿名 `proc_mmap`，不返回也不创建可分享的 VMO handle
- **映射**：`vm_map` 将 VMO 映射到进程地址空间
- **共享**：`vm_share` 只把已有 MEMORY handle 安装到当前或目标 Native task；不按地址区间导出
- **调整大小**：核心有 `vmo_resize()`，但当前没有 `handle_control(VMO_RESIZE)` 用户入口
- **读取/写入**：当前 `handle_read`/`handle_write` 只走 vfile/global-fd 路径，不支持 MEMORY handle；VMO 内容需先映射

### 2.4 VMO 四元组

VMO 的核心属性可以形式化为四元组：

概念上可关注 `(pages, size, type, physical layout)`；当前 `struct vmo` 没有独立 `phys_contiguous` 字段：

- `pages`：物理页面数组（按需分配）
- `size`：逻辑大小
- `type`：ANONYMOUS / PHYSICAL / PAGED
- physical layout：由实际 `pages[]` 决定；`device_alloc_dma` 的匿名 VMO 可连续，不能只按 `type` 推断

---

## 3. VMAR (Virtual Memory Address Region)

### 3.1 定义

当前内核不分配 `a20_vmar_t`。Native VMAR 操作直接作用于核心 `vm_area_t`，与后端相关的字段可简化为：

```c
typedef struct vm_area {
    vaddr_t          start;
    vaddr_t          end;
    uint64_t         vm_flags;
    pte_t            pte_flags;
    int              file_fd;           /* VM_FILE 后端 */
    uint64_t         file_offset;
    struct vmo      *vmo;               /* 仅 VM_VMO 后端非 NULL */
    uint64_t         vmo_offset;
    /* 另有 vnode、链表和 NOMMU 字段 */
} vm_area_t;
```

### 3.2 VMAR 三元组

概念上可写为：

$$VMAR = ([base, base+len), prot, backing)$$

- `[base, base+len)`：虚拟地址范围
- `prot`：保护位（R/W/X 组合）
- `backing`：普通匿名页、FILE/DEVICE 的 file/page-cache 后端，或 MEMORY source 的 VMO

### 3.3 VMAR 标志（保留接口）

| 标志 | 说明 |
|------|------|
| `A20_VMAR_CAN_MAP_READ` | 允许映射为可读 |
| `A20_VMAR_CAN_MAP_WRITE` | 允许映射为可写 |
| `A20_VMAR_CAN_MAP_EXEC` | 允许映射为可执行 |
| `A20_VMAR_CAN_MAP_SPECIFIC` | 允许在指定地址映射 |

这些常量存在于用户 ABI 头中，但当前 `a20_vmar_map()` 忽略传入 `flags`，也不保存可用于后续 `vm_protect` 的 VMAR capability mask。

---

## 4. 内存操作语义

### 4.1 vm_alloc — 匿名内存分配

```c
int64_t vm_alloc(a20_vm_alloc_args_t *args);
```

语义：
1. 校验版本化参数与非零长度
2. 调用 `proc_mmap(..., MAP_ANONYMOUS, -1, 0)` 建立普通匿名 VMA
3. 返回映射地址；不会返回 VMO handle

错误条件：
- `NO_MEMORY`：核心匿名 mmap 失败
- `INVALID_ARGUMENT`：版本化结构校验失败或 `length == 0`
- `FAULT`：参数结构不可访问，或结果复制回用户态失败

当前 syscall 不单独校验未知 `prot` 位，地址空间无空洞等核心 mmap 失败也统一映射为 `NO_MEMORY`，不会返回 `NO_SPACE`。

### 4.2 vm_map — 对象映射

```c
int64_t vm_map(a20_vm_map_args_t *args);
```

当前实现语义：
1. `source == A20_HANDLE_NULL`：创建匿名 VMO 并映射
2. 否则验证 source handle 有效、检查 `MAP` 权限，类型必须为 `MEMORY`、`FILE` 或 `DEVICE`
3. source 是 `MEMORY`：复用已有 VMO，验证 `[offset, offset+length)` 不越界；`offset` 需页对齐
4. source 是 `FILE`/`DEVICE`：走核心 `mm_mmap_file`，经 page cache **按需分页**填充（不再 eager-load 到匿名 VMO）；`offset` 需页对齐
5. 计算 READ/WRITE 的 `prot_eff` 与 handle rights 交集；当前 EXEC 位直接透传，没有检查 source handle 的 `A20_RIGHT_EXEC`
6. MEMORY source 创建 `VM_VMO` VMA并持 VMO 引用；FILE/DEVICE source 创建 `VM_FILE` 私有 VMA并持 fd 引用

**与 POSIX mmap 的关键区别**：非匿名映射的 source 是 handle。READ/WRITE rights 会收紧对应保护位；EXEC rights 当前未在该路径强制，属于实现与目标 rights 模型之间的已知缺口。

**实现分层（2026-08 更新）**：VMO 位于核心 MM（`kernel/mm/vmo.c`、`mm/vmo.h`），VMAR 是核心 `mm_mmap_vmo`/`mm_munmap`/`mm_mprotect` 的薄包装（`kernel/abi/native/vmar.c`）。VMO 帧由 VMO 自持，映射按需调页，fork 共享同一批帧。

### 4.3 vm_unmap — 解除映射

```c
int64_t vm_unmap(uint64_t addr, uint64_t length);
```

语义：
1. 直接调用核心 `proc_munmap` 解除指定范围内的普通匿名、文件或 VMO VMA
2. 核心 MM 拆分/移除 VMA，并执行所需 TLB invalidation
3. 仅当移除的是 `VM_VMO` VMA 时才释放该 VMA 持有的 VMO 引用；最后一个 VMO 引用释放 canonical frames

### 4.4 vm_protect — 修改保护

```c
int64_t vm_protect(uint64_t addr, uint64_t length, uint32_t prot);
```

语义：
1. 查找目标 VMAR
2. 直接调用核心 `mm_mprotect`
3. 由核心路径更新页表项并执行所需 TLB invalidation

当前没有保存/检查 `CAN_MAP_*` 或原 source handle rights，因此 Native 层本身不保证“只能收紧不能放宽”。

### 4.5 vm_share — 内存共享

```c
int64_t vm_share(a20_handle_t vmo, a20_handle_t target_task,
                 a20_rights_t rights);
```

当前 syscall 形式为 `vm_share(vmo_handle, target_task, rights)`：
1. source 必须是 `A20_OBJ_MEMORY`，需要 `READ | TRANSFER` right
2. `target_task == A20_HANDLE_NULL` 时安装到当前进程 HT；否则 task handle 需要 `CONTROL`，内核找到目标进程 HT 并安装
3. 接收权限为 `rights ∩ source.rights`，空集返回 `ACCESS`
4. 新 handle 继承源 VMO handle 的 expiry、remaining_ops、temporal flags 与安全标签，并增加 VMO 引用
5. 对目标进程执行 Bell-LaPadula No Read Up 检查

接收方通过返回的目标 HT handle 编号调用 `vm_map`。当前仍未提供“按地址区间反查 VMO 并导出”的 `a20_vm_share_args_t` 形式；用户应先使用 `vm_create_object` 创建 VMO，再映射和分享。

### 4.6 vm_flush — 刷新

```c
int64_t vm_flush(uint64_t addr, uint64_t length, uint32_t flags);
```

| Flag | 说明 |
|------|------|
| `A20_FLUSH_CLEAN` | 写回脏页 |
| `A20_FLUSH_INVALIDATE` | 使缓存无效 |
| `A20_FLUSH_SYNC` | 等待写回完成 |

当前实现先验证地址范围均有 VMA；`SYNC` 调用全局 `vfs_sync()`，`INVALIDATE` 只执行本地 `arch_tlb_flush()`，`CLEAN` 单独使用时是 no-op。它没有按给定 VMA 范围执行脏页写回或 page-cache invalidation，多个 flag 组合也因顺序返回而不是完整组合语义。

---

## 5. 共享内存流程

```text
进程 A                                      进程 B
vm_create_object → vmo_A
vm_map(vmo_A) → addr_A
vm_share(vmo_A, task_B, rights) ─────────→ vmo_B
                                             vm_map(vmo_B) → addr_B
[读写 addr_A]       ← canonical VMO frames → [读写 addr_B]
```

权限传递：`vm_share` 的 `rights` 参数限制接收方 handle 权限。只有 READ 时，首次 `vm_map` 的 WRITE 会被清除；但当前 `vm_protect` 不重新检查原 handle rights，仍可能把该 VMA 放宽为可写，这是尚未收口的权限缺口。

---

## 6. 与内核 MM 子系统的集成

### 6.1 映射到现有 mm_struct

```c
// Native VMAR syscall 是 mm_struct/vm_area 操作的薄包装。
// 普通匿名映射: vm_area -> anonymous frames
// FILE/DEVICE source: vm_area -> vm_file/page cache
// MEMORY source: vm_area -> vmo (VM_VMO)
// 三者共享核心页表、fault 和 TLB invalidation 机制。
```

### 6.2 缺页处理

1. CPU 触发 page fault，内核查找对应 `vm_area_t`
2. `VM_VMO` VMA 从 VMO 的 `pages[]` 获取或首次物化 canonical frame
3. `VM_FILE` VMA 通过 vnode/page cache 填充；当前不会创建 `VMO_PAGED`
4. 普通匿名 VMA 走核心匿名 fault/COW 路径
5. 更新页表项并返回用户态

### 6.3 fork 与 VMO 共享

`handle_dup(VMO_handle)` 只创建指向同一 VMO 的新 handle，不提供 COW 选项。`VM_VMO` VMA 在 fork 后继续映射 VMO 持有的 canonical frames，并按共享映射处理；普通匿名私有 VMA 才走核心 fork COW 路径。

---

## 7. 保护位语义

```c
#define A20_PROT_READ    (1 << 0)   /* 可读 */
#define A20_PROT_WRITE   (1 << 1)   /* 可写 */
#define A20_PROT_EXEC    (1 << 2)   /* 可执行 */
#define A20_PROT_NONE    0          /* 无访问权限（用于 guard page） */
```

保护位在映射时由以下因素共同决定：

目标模型是请求、handle rights 和 VMAR capability 的交集。当前实现只对 `vm_map` 的 READ/WRITE 做 handle-rights 交集；VMAR flags 未执行，EXEC 直接透传，后续 `vm_protect` 也不重新检查原 handle rights。因此该公式是待收口契约，不是当前完整保证。

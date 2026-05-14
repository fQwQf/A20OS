# A20OS Native ABI：内存子系统设计

> 本文档定义 Native ABI 的内存管理模型，包括 VMO（Virtual Memory Object）和 VMAR（Virtual Memory Address Region）抽象、内存操作语义和共享内存机制。

---

## 1. 内存模型概述

Native ABI 将匿名内存、文件映射和共享内存统一到两个核心抽象：

- **VMO (Virtual Memory Object)**：物理内存的后端存储。代表一组物理页面，独立于地址空间。
- **VMAR (Virtual Memory Address Region)**：进程地址空间中的一段连续虚拟地址范围。VMO 通过 vm_map 操作映射到 VMAR 中。

```text
进程地址空间 (VMAR)
┌──────────────────────────────────────────────────┐
│ 0x0000_0000_0000                                 │
│  ... (不可映射)                                   │
│ 0x0001_0000_0000  ┌──────────────┐               │
│                    │ VMAR: code   │ ← VMO: ELF    │
│ 0x0001_0001_0000  └──────────────┘               │
│                    ┌──────────────┐               │
│                    │ VMAR: heap   │ ← VMO: anon   │
│                    └──────────────┘               │
│  ...                                              │
│ 0x7fff_0000_0000  ┌──────────────┐               │
│                    │ VMAR: stack  │ ← VMO: anon   │
│ 0x8000_0000_0000  └──────────────┘               │
│  ... (内核空间)                                   │
└──────────────────────────────────────────────────┘
```

### 1.1 与 POSIX mmap 的区别

| POSIX mmap | Native ABI |
|-----------|-----------|
| 映射是匿名的，没有独立对象 | 映射的目标是 VMO handle |
| 共享内存通过 SysV/POSIX shm | shm 就是 VMO，通过 handle 传递 |
| 保护位与映射绑定 | 保护位在 VMO 和 VMAR 上各自独立 |
| `mremap`/`madvise` 碎片化 | vm_protect, vm_flush 语义清晰 |

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
    struct page    **pages;             /* 按页索引的物理页面数组 */
} a20_vmo_t;
```

### 2.2 VMO 类型

| 类型 | 说明 | 来源 |
|------|------|------|
| `A20_VMO_ANONYMOUS` | 匿名内存，按需分配物理页 | vm_alloc |
| `A20_VMO_PHYSICAL` | 连续物理内存（设备 DMA 等） | vm_share (device) |
| `A20_VMO_PAGED` | 文件后备内存，按需填充 | vm_map (file) |

### 2.3 VMO 操作

- **创建**：`vm_alloc` 创建匿名 VMO，`vm_map` 从 file handle 创建 paged VMO
- **映射**：`vm_map` 将 VMO 映射到进程地址空间
- **共享**：`vm_share` 将地址空间的一段导出为 VMO handle，可传递给其他进程
- **调整大小**：通过 `handle_control(VMO_RESIZE)` 扩展或缩小 VMO
- **读取/写入**：通过 `handle_read`/`handle_write` 直接读写 VMO 内容（不映射）

### 2.4 VMO 四元组

VMO 的核心属性可以形式化为四元组：

$$VMO = (pages, size, type, phys\_contiguous)$$

- `pages`：物理页面数组（按需分配）
- `size`：逻辑大小
- `type`：ANONYMOUS / PHYSICAL / PAGED
- `phys_contiguous`：是否物理连续（仅 PHYSICAL 类型为 true）

---

## 3. VMAR (Virtual Memory Address Region)

### 3.1 定义

VMAR 是进程地址空间中的连续区域：

```c
typedef struct a20_vmar {
    uint64_t         base;              /* 起始虚拟地址 */
    uint64_t         length;            /* 大小（字节） */
    uint32_t         prot;              /* 保护位（R/W/X） */
    uint32_t         flags;             /* VMAR 标志 */
    a20_vmo_t       *vmo;               /* 映射的 VMO（NULL = 未映射） */
    uint64_t         vmo_offset;        /* VMO 内偏移 */
} a20_vmar_t;
```

### 3.2 VMAR 三元组

$$VMAR = ([base, base+len), prot, mappings)$$

- `[base, base+len)`：虚拟地址范围
- `prot`：保护位（R/W/X 组合）
- `mappings`：映射到 VMO 的信息

### 3.3 VMAR 标志

| 标志 | 说明 |
|------|------|
| `A20_VMAR_CAN_MAP_READ` | 允许映射为可读 |
| `A20_VMAR_CAN_MAP_WRITE` | 允许映射为可写 |
| `A20_VMAR_CAN_MAP_EXEC` | 允许映射为可执行 |
| `A20_VMAR_CAN_MAP_SPECIFIC` | 允许在指定地址映射 |

---

## 4. 内存操作语义

### 4.1 vm_alloc — 匿名内存分配

```c
int64_t vm_alloc(a20_vm_alloc_args_t *args);
```

语义：
1. 创建匿名 VMO（size = args.length, type = ANONYMOUS）
2. 在进程地址空间中寻找空闲区域（从 addr_hint 或自动选择）
3. 创建 VMAR，映射 VMO
4. 返回映射地址

错误条件：
- `NO_MEMORY`：无法分配 VMO 结构或 VMAR
- `NO_SPACE`：地址空间无足够空闲区域
- `INVALID_ARGUMENT`：length = 0 或 prot 无效

### 4.2 vm_map — 对象映射

```c
int64_t vm_map(a20_vm_map_args_t *args);
```

语义：
1. 验证 source handle 有效且类型为 file/shm/device
2. 检查 `MAP` 权限
3. 如果 source 是 file：创建 paged VMO，后备存储为文件内容
4. 如果 source 是 shm：复用已有的 shm VMO
5. 在地址空间中分配 VMAR，映射 VMO
6. `refcount_inc(vmo)`

**与 POSIX mmap 的关键区别**：映射的目标是 handle，不是 fd。handle 的 rights 决定映射的保护位——如果 handle 只有 READ 权限，映射不能是 WRITABLE，即使 prot 参数请求了 W。

### 4.3 vm_unmap — 解除映射

```c
int64_t vm_unmap(uint64_t addr, uint64_t length);
```

语义：
1. 查找 [addr, addr+length) 范围内的 VMAR
2. 解除 VMO 映射
3. 刷新 TLB
4. `refcount_dec(vmo)`：如果最后引用，释放 VMO

### 4.4 vm_protect — 修改保护

```c
int64_t vm_protect(uint64_t addr, uint64_t length, uint32_t prot);
```

语义：
1. 查找目标 VMAR
2. 新保护位必须是 VMAR 允许的子集（`CAN_MAP_*` 标志限制）
3. 更新页表项
4. 刷新 TLB

**只能收紧不能放宽**：如果 VMAR 创建时没有 `CAN_MAP_EXEC`，后续不能通过 vm_protect 添加执行权限。

### 4.5 vm_share — 内存共享

```c
int64_t vm_share(a20_vm_share_args_t *args);
```

语义：
1. 查找 [addr, addr+length) 对应的 VMO
2. 创建新的 shm 对象（类型 A20_OBJ_MEMORY），引用同一 VMO
3. 设置导出权限（rights 参数限制接收方权限）
4. 返回 shm handle

接收方通过 `vm_map(shm_handle)` 映射到自己的地址空间。

### 4.6 vm_flush — 刷新

```c
int64_t vm_flush(uint64_t addr, uint64_t length, uint32_t flags);
```

| Flag | 说明 |
|------|------|
| `A20_FLUSH_CLEAN` | 写回脏页 |
| `A20_FLUSH_INVALIDATE` | 使缓存无效 |
| `A20_FLUSH_SYNC` | 等待写回完成 |

---

## 5. 共享内存流程

```text
进程 A                         进程 B
  │                              │
  vm_alloc → addr_A              │
  │                              │
  vm_share(addr_A) → shm_handle │
  │                              │
  channel_send(shm_handle) ──────→ channel_recv → shm_handle_B
  │                              │
  │                              │ vm_map(shm_handle_B) → addr_B
  │                              │
  [读写 addr_A] ←── 共享内存 ──→ [读写 addr_B]
```

权限传递：`vm_share` 的 `rights` 参数限制接收方权限。如果 rights 只有 READ，接收方映射后只能读取。

---

## 6. 与内核 MM 子系统的集成

### 6.1 映射到现有 mm_struct

```c
// VMAR 对应 mm_struct 中的 vm_area
// VMO 对应 vm_area 的后端存储
//
// Linux ABI: vm_area → vfile (通过 vm_file)
// Native ABI: VMAR → VMO (通过 a20_vmar.vmo)
//
// 两者共享同一套页表和 TLB 刷新机制
```

### 6.2 缺页处理

1. CPU 触发 page fault
2. 内核查找 fault 地址对应的 VMAR
3. 如果 VMAR 映射了 VMO：
   - ANONYMOUS：分配新物理页，填入 VMO pages 数组
   - PAGED：从文件读取对应页
4. 更新页表项
5. 返回用户态

### 6.3 写时复制 (Copy-on-Write)

`handle_dup(VMO_handle)` 可以选择创建 COW 映射。两个 handle 指向同一 VMO，但写操作触发 COW：

1. 写操作触发 page fault (write to read-only page)
2. 内核分配新物理页，复制原页内容
3. 更新 fault 进程的页表项指向新页
4. 原物理页的引用计数减 1

---

## 7. 保护位语义

```c
#define A20_PROT_READ    (1 << 0)   /* 可读 */
#define A20_PROT_WRITE   (1 << 1)   /* 可写 */
#define A20_PROT_EXEC    (1 << 2)   /* 可执行 */
#define A20_PROT_NONE    0          /* 无访问权限（用于 guard page） */
```

保护位在映射时由以下因素共同决定：

$$prot_{effective} = prot_{requested} \cap prot_{handle\_rights} \cap prot_{vmar\_flags}$$

三者取交集。即使进程请求了 WRITE，如果 handle 只有 READ 权限或 VMAR 没有 `CAN_MAP_WRITE`，最终映射仍然是只读的。

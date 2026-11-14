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

## 驱动物理视图

`mm_mmap_physical()` 由同一 Rust mmap policy 拥有 eager 物理映射的放置、准入和回滚，C 暴露字段/架构/锁/TLB 原语；fbdev 与用户驱动不再自行维护安装循环。记录在锁外分配，全部叶安装成功才发布 PFNMAP 记录并记账，失败连同当前页的空路径和已映射前缀一起撤销，在 TLB 事务结束后退休元数据。权限 ceiling 复用 VM_NATIVE_CAP，mprotect 保留原 CPU 缓存类型。

此视图不拥有 frame/backing 引用，不改变 PFA、VMO 或 DMA domain 的存储所有权。调用者须持有存储，或在释放前完成全部映射撤销；旧 get_fb framebuffer 持久物理映射的退役仍待实现；拥有 RAM 的路径见下文。固定覆盖、允许属性及 hosted/NOMMU 边界见 [驱动物理映射契约](../../drivers/guide/native-runtime.md#驱动-cpu-物理映射事务)。

## 驱动 RAM 使用普通 VMO

`vmo_create_contiguous()` 在发布前把独占高阶 PFA 块转换为 canonical order-0 引用，保留页取整逻辑前缀、交还多余尾部并清零全部暴露 padding。内部 CONTIGUOUS kind 固定 extent 和 slot，拒绝 resize/补洞，内容仍可写；`vmo_contiguous_info()` 只查询稳定 CPU PA/extent，不授予设备 DMA 权限。canonical pin 与 Rust destroy 继续使用原有引用和 TLB 后 frame_put，没有新增 VMO/VMA 布局或 backing 注册表。

GPU owning scanout 向消费者转移一份 VMO 引用。`mm_mmap_vmo_cap()` 是既有 locked VMO mapper 的 unlocked TLB/MM 事务入口，先校验初始权限 ceiling，成功 VMA 自持 VMO。virtio-gpu/fbdev 复用普通 VMO fault、共享 fork、split、mprotect 与销毁，release 不调用设备代码；旧 raw 物理路径仍不拥有存储。详见 [RAM scanout 契约](../../drivers/guide/native-runtime.md#连续-ram-vmo-与-owning-scanout)。

DRM 与 PRIME dma-buf 的 RAM mmap 同样复用 `mm_mmap_vmo_cap()`，校验共享读写、范围/溢出和 fake handle offset，拒绝执行与 private，保留权限 ceiling 和原生错误。普通 VMA 自持 VMO；旧 GPU 绑定停止后，DRM worker 可退休硬件身份和 GEM，已有 CPU 映射仍保留存储，不需要模块最终回调。dma-buf fd 另持原 GEM pin，跨 publication 导入不隐式建立 DMA attach。见 [DRM 文件与退休](../../drivers/guide/native-runtime.md#drm-文件命名空间与失败退休)。

GPU 原生能力快照是内部元数据，RAM_SCANOUT 标志不取得 VMO/page 引用，也不授予 MM/DMA 权限。ABI 12 get_capabilities 与 ABI 11 引入的 owning get_scanout 分工独立；实际 snapshot 仍须验证 extent/slot，VMA 仍自行保留 VMO。见 [能力与所有权](../../drivers/guide/native-runtime.md#原生-gpu-能力与默认选择)。

## 关键代码与头文件

- **核心虚拟内存定义**：[`kernel/include/mm/vm.h`](../../../kernel/include/mm/vm.h)（`mm_struct_t`、`mm_seg_t`、锁模型规则）
- **页表与游标契约**：[`kernel/include/mm/pt.h`](../../../kernel/include/mm/pt.h)（`pt_meta_t`、`mm_cursor_t` 接口与不变量）
- **缺页异常处理**：[`kernel/mm/fault.c`](../../../kernel/mm/fault.c)（状态驱动快速分派与 COW）
- **物理分配器**：[`kernel/mm/frame.c`](../../../kernel/mm/frame.c)、[`kernel/mm/slab.c`](../../../kernel/mm/slab.c)

## 固定 resident RAM backing

内核 MM 提供 `vmo_create_resident(bytes, owner, &out)`：页取整范围在发布前全部获得 canonical order-0 RAM 并清零，kind 5 固定 extent/slot、内容可写，无物理连续性保证。它复用 Rust VMO ref/pin/destroy 与普通 VMA；不创建 GPU 专用页表或对象注册表。公开 vmo_create 不接受 kind 5，resize 与空 slot 的重新物化被拒绝，连续地址查询也拒绝这种对象。

owner cgroup 的整个 backing charge 和 namespace pin 随 VMO 存活，采用既有共享匿名 backing 记账，避免进程 RSS teardown 提前扣除共享存储；NULL 明确选择内核无 charge 存储。预检、预算和任意部分分配失败交还全部资源。DRM 已在 store 锁外采用该工厂，预算 owner 在 GPU mutex 前捕获，映射/导出方不转移原始 charge。独立 page pin/DMA mapping 只保留帧；需要预算 owner 的请求须另持 VMO 引用。

这是固定 RAM 分配契约，不是 DMA 授权、VRAM/BO 迁移、GPUVM 或任务调度；owning scanout 目前仍限 kind 4。接口、错误与验证见[原生 resident RAM](../../drivers/guide/native-runtime.md#固定-resident-ram-vmo-与预算-owner)。

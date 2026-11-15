# Native virtgpu 资源分配

`DRM_IOCTL_VIRTGPU_RESOURCE_CREATE` 现在分配一个全新的 GEM 与 host 资源，并把 `bo_handle` 与 `res_handle` 作为输出返回。它忽略这两个字段的输入值，把后端存储向上取整到页，并把 `size=0` 当作一页。创建、挂载后端或拷贝输出任一步失败都会撤销未交付 handle，并尝试清理 host 对象；未知完成或清理失败时保留原 ID 和 backing，等待重试成功或旧绑定停止，不能把失败当作硬件未创建。实现遵循 [Linux virtgpu_ioctl.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/virtio/virtgpu_ioctl.c) 的分配契约。

GEM backing 已改用原生 `vmo_create_resident`：在 store mutex 外完整预分配并清零 canonical order-0 RAM，固定 extent/slot，物理连续性不保证。原始分配任务的 cgroup 在 GPU mutex 前捕获，backing charge 随 VMO 保留；失败传播原生 errno，不能降级成无 charge 分配。attach_backing 只查询已物化页，不再延迟补页。不是 VRAM/GPUVM 或 BO 迁移策略。

GEM store 改用哈希桶中的动态分配对象，取代原先 64 项的固定数组。既有的 pin 与 framebuffer 引用会推迟销毁；对象与后端存储在 store 互斥量之外释放。单对象 256 MiB 上限不变。对象按 publication 分区，handle/FB owner 已按 open file 记账；正常最后 close 自动交还，跨文件通过 PRIME/GEM_OPEN 显式共享。仍未实现完整 master/auth、共享访问权和厂商 BO/GPUVM 的迁移/驻留预算策略；resident backing 已保留原分配 owner 的 cgroup charge。

旧测试会预分配一个私有的 A20 GEM，从而掩盖了不兼容的资源创建契约。`gpu3d_test` 现在只用 native 分配，并检查：

- 真实由 host 渲染的红、蓝像素读回；
- 96 个并发资源、互不相同的输出句柄，`size=0` 的后端为页大小，随后逐个 close 并在已退休句柄上得到 ENOENT。

本改动确立的是 native 分配与命令流直接渲染；Minecraft 加速还需要额外的传输与 scanout 工作。

固定 RAM backing 可显式交给原生 `driver_dma_map_owned_vmo`：成功或失败清理句柄保留整个 VMO 和原始预算归属，硬件只获得选定页视图；确认失效后才在 domain 锁外交还 owner。现有 map_vmo 继续只保留帧。该入口不自动为 VirtIO attach_backing 建立 domain，也不提供厂商 BO/GPUVM。通用请求、依赖和安全完成由 [native queue](../drivers/guide/request-queues.md)组合这些 owner，VirtIO GPU 已接入；映射入口本身不自动替所有硬件建立执行/停止协议。见 [VMO owner](../drivers/guide/native-runtime.md#持有-vmo-的-dma-视图与锁外-owner-退休)。

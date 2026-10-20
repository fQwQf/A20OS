# Native virtgpu 资源分配

`DRM_IOCTL_VIRTGPU_RESOURCE_CREATE` 现在分配一个全新的 GEM 与 host 资源，并把 `bo_handle` 与 `res_handle` 作为输出返回。它忽略这两个字段的输入值，把后端存储向上取整到页，并把 `size=0` 当作一页。创建、挂载后端或拷贝输出任一步失败都会回滚 GEM 与 host 对象。实现遵循 [Linux virtgpu_ioctl.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/virtio/virtgpu_ioctl.c) 的分配契约。

GEM store 改用哈希桶中的动态分配对象，取代原先 64 项的固定数组。既有的 pin 与 framebuffer 引用会推迟销毁；对象与后端存储在 store 互斥量之外释放。单对象 256 MiB 上限不变。句柄归属在 A20OS 中仍是设备级：本改动不宣称 Linux 的 per-open GEM 句柄记账。

旧测试会预分配一个私有的 A20 GEM，从而掩盖了不兼容的资源创建契约。`gpu3d_test` 现在只用 native 分配，并检查：

- 真实由 host 渲染的红、蓝像素读回；
- 96 个并发资源、互不相同的输出句柄，`size=0` 的后端为页大小，随后逐个 close 并在已退休句柄上得到 ENOENT。

本改动确立的是 native 分配与命令流直接渲染；Minecraft 加速还需要额外的传输与 scanout 工作。

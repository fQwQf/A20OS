# 图形与 GPU 子系统文档索引

A20OS 图形子系统支持从固件线性帧缓冲（EFIFB/SimpleFB）、DRM/KMS 现代显卡接口到 VirtIO-GPU 3D 硬件加速的完整图形栈。

---

## 规范与演进设计

| 文档 | 说明 |
|---|---|
| [3d-graphics.md](3d-graphics.md) | 3D 图形栈设计：VirtIO-GPU、DRM UAPI 与 Mesa 3D 加速原理 |
| [gpu-3d-roadmap.md](gpu-3d-roadmap.md) | 3D GPU 演进路线图（VIRTGPU 驱动、ABI 门禁与渲染管线） |
| [virtgpu-native-allocation.md](virtgpu-native-allocation.md) | VirtIO-GPU 原生显存分配与 VMO 映射机制 |
| [virtgpu-sharing-and-sync.md](virtgpu-sharing-and-sync.md) | 跨进程显存共享（dma-buf）与同态栅栏同步 |
| [drm-sysfs-discovery.md](drm-sysfs-discovery.md) | DRM 节点探测、sysfs 枚举与用户态服务发现 |
| [firmware-video.md](firmware-video.md) | 引导期固件视频（UEFI GOP、Simple Framebuffer）接入 |
| [host-tools.md](host-tools.md) | 宿主侧图形工具链（`virglrenderer` 桥接与 DRM 校验） |
| [real-hardware-gpu.md](real-hardware-gpu.md) | 实体硬件 GPU 现实边界与移植可行性分析 |

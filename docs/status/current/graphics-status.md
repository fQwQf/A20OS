# 图形栈当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

图形（DRM、virtgpu、3D）当前生效的范围与限制。

<!-- 以下内容从 docs/graphics/host-tools.md 迁入；只保留当前状态，历史运行记录已删除。 -->

### 当前状态

实测 48 个号与 Linux UAPI 一致，1 个无上游对应（`MODE_DPMS`，上游同样
没有定义）。**双向可证伪已验证**：把 `MODE_RMFB` 扰动一下、或从
`drm_mode_fb_cmd` 删掉一个字段，都会被抓出来。

<!-- 以下内容从 docs/graphics/host-tools.md 迁入。 -->

### 当前状态：**已运行；renderer 已升级，但 Mesa 仍未 attach**

已实测（`fca72f5f`）：

- 依赖 `libgbm-dev`、`libdrm-dev`、`libudev-dev`、`python3-mako`、
  `python3-yaml`（1.3.0 的 gallium 会 import，漏掉它 `check` 会过、构建两分钟后才炸）
  已装齐；
- 脚本从源码构建出 virglrenderer 1.3.0（发行版那份是 2020 年的 1.1.0-2），
  装在 `tools/virgl/install`；
- `LD_DEBUG=libs` 确认 **QEMU 加载的确实是这份新库**。

但 **stock Mesa 仍未能 attach**。QEMU 依旧不向 guest 提供
`VIRTIO_GPU_F_VIRGL`：NVIDIA EGL 下它静默降级为 2D，强制 Mesa EGL 则显式报
`eglInitialize failed: EGL_NOT_INITIALIZED` / `render node init failed`。
强制 Mesa 时 `eglinfo -p surfaceless` 是成功的（radeonsi / OpenGL 4.6），
而 `eglinfo -p gbm` 失败。卡点落在 GBM/设备平台这条路上。

所以可以说宿主 renderer 已升级，但**不可以说 stock Mesa 已能 attach**。
完整判据表见 [../../graphics/gpu-3d-roadmap.md §5.1](../../graphics/gpu-3d-roadmap.md)。

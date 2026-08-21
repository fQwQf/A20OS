# A20OS 3D 图形加速栈（virtio-gpu / virgl）

本文档描述 A20OS 的 3D 图形加速实现：原理、内核接口、驱动实现、用户态对接方式和开发指南。阅读前提是理解 [Display/Framebuffer 驱动](../drivers/classes/display.md) 与 [PCI 与 VirtIO](../drivers/guide/pci-and-virtio.md)。

> 当前状态：**内核侧 3D 命令路径已可用**（feature 协商、capset 查询、context/resource/submit 透传、DRM 入口），用户态 virgl 客户端（Mesa/GBM/EGL）为后续阶段。

---

## 1. 背景与原理

### 1.1 为什么需要 3D

- 现代桌面（GTK/Wayland 合成器、浏览器、媒体播放）依赖 GPU 合成与渲染。纯软件（pixman/cairo）能点亮屏幕，但把合成和几何变换压回 CPU，性能与功耗都不足。
- A20OS 运行在 QEMU 上，没有真实 GPU；可用的"硬件加速"是 **virtio-gpu 的 3D/Context 模式**：guest 通过 `VIRTIO_GPU_CMD_SUBMIT_3D` 提交 OpenGL 命令流，QEMU 侧 virglrenderer 在宿主机 GPU/CPU 上执行并回传结果。**渲染计算发生在宿主机，guest 内核只负责搬运命令与资源。**

### 1.2 virtio-gpu 两种模式

| 模式 | 命令 | 作用 | A20 状态 |
|---|---|---|---|
| 2D | `RESOURCE_CREATE_2D` / `TRANSFER_TO_HOST_2D` / `FLUSH` | 把像素块 blit 到 scanout | ✅ 现有 fbdev 路径 |
| 3D (virgl) | `CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` / `TRANSFER_TO_HOST_3D` / BLOB | host 端 GL 上下文 + 命令流 | ✅ 内核透传已实现 |

设备 feature 位：`VIRTIO_GPU_F_VIRGL (bit 0)` 表示 host 支持 3D；`VIRTIO_GPU_F_CONTEXT_INIT (bit 4)` 表示 context 初始化协议。

### 1.3 virgl 协议模型

- **Context**：一个 host 端 GL 状态机（类似 EGL 上下文），用 `ctx_id` 标识。
- **Resource**：host 端 GL 对象（纹理、帧缓冲、缓冲对象），用 `resource_id` 标识，带 `target/format/bind` 等 GL 参数。
- **Submit**：把一段 virgl 命令流（用户态 Mesa virgl 驱动打包的 GL 调用）经 controlq 交给 host。
- **Capset**：host 能力描述块（如 `VIRTIO_GPU_CAPSET_VIRGL`），描述协议版本与能力。

用户态 Mesa 的 `virtio_gpu` 驱动把 EGL/GLES 调用序列化进 command buffer，经 DRM ioctl 交给内核，内核原样转发给 virtio-gpu。命令流格式由 Mesa `virglrenderer` 的协议定义（`VIRGL_*` 编码）。

---

## 2. 内核接口

### 2.1 UAPI 头：`kernel/include/drivers/gpu/virtio_gpu.h`

该头定义全部 virtio-gpu 命令号、响应码、capset id 与数据结构，参照 Linux UAPI `virtio_gpu.h`：

- **feature 位**：`VIRTIO_GPU_F_VIRGL`、`VIRTIO_GPU_F_EDID`、`VIRTIO_GPU_F_CONTEXT_INIT`、`VIRTIO_GPU_F_RESOURCE_UUID`。
- **2D 命令**：`GET_DISPLAY_INFO`、`RESOURCE_CREATE_2D`、`SET_SCANOUT`、`TRANSFER_TO_HOST_2D`、`RESOURCE_FLUSH`、`ATTACH/DETACH_BACKING`。
- **3D 命令**：`GET_CAPSET_INFO`(0x0108)、`GET_CAPSET`(0x0109)、`RESOURCE_CREATE_BLOB`(0x010c)、`CTX_CREATE`(0x0200)、`CTX_DESTROY`(0x0201)、`CTX_ATTACH/DETACH_RESOURCE`(0x0202/3)、`RESOURCE_CREATE_3D`(0x0204)、`TRANSFER_TO/FROM_HOST_3D`(0x0205/6)、`SUBMIT_3D`(0x0207)、`RESOURCE_MAP/UNMAP_BLOB`(0x0208/9)。
- **响应码**：`RESP_OK_NODATA/DISPLAY_INFO/CAPSET_INFO/CAPSET/EDID/RESOURCE_UUID/MAP_INFO` 与 `RESP_ERR_*`。
- **capset**：`VIRTIO_GPU_CAPSET_VIRGL`(1)、`VIRTIO_GPU_CAPSET_VIRGL2`(2)、`VIRTIO_GPU_CAPSET_VENUS`(4)、`VIRTIO_GPU_CAPSET_DRM`(6)。

### 2.2 A20 3D 透传 ioctl（`A20_GPU_IOCTL_*`）

为隔离 Linux DRM ABI 与 A20 私有的 virgl 透传，A20 定义了一组私有 ioctl 号，由 `gpu_dev_ops_t.ioctl` 分发，再经 `/dev/dri/card0` 暴露给用户态：

| ioctl | 语义 |
|---|---|
| `A20_GPU_IOCTL_VIRGL_CHECK` | 查询设备是否协商出 virgl；`-ENXIO` 表示 2D-only |
| `A20_GPU_IOCTL_CTX_CREATE` | 创建 host 端 virgl 上下文 |
| `A20_GPU_IOCTL_CTX_DESTROY` | 销毁上下文 |
| `A20_GPU_IOCTL_RES_CREATE_3D` | 创建 3D 资源（纹理等） |
| `A20_GPU_IOCTL_RES_UNREF` | 释放资源 |
| `A20_GPU_IOCTL_SUBMIT_3D` | 提交 virgl 命令流 blob |

参数统一为 `struct virtio_gpu_3d_req`：

```c
struct virtio_gpu_3d_req {
    uint32_t ctx_id;       /* 目标 virgl 上下文 */
    uint32_t resource_id;  /* 目标资源 */
    uint32_t target;       /* create_3d: GL 目标类型（GL_TEXTURE_2D=2 ...） */
    uint32_t format;       /* create_3d: GL 格式（GL_RGBA8=0x8058 ...） */
    uint32_t bind;         /* create_3d: VIRGL_BIND_* */
    uint32_t width, height, depth, array_size, last_level, nr_samples, flags;
    uint32_t context_init; /* ctx_create: CONTEXT_INIT capset id */
    uint64_t cmdbuf;       /* submit_3d: 用户态命令流指针 */
    uint64_t cmdlen;       /* submit_3d: 命令流长度（<=128 KiB） */
    char     name[32];     /* ctx_create: 调试名 */
};
```

### 2.3 分发路径

```
用户态 (virgl client / gpu3d_test)
   │ ioctl(/dev/dri/card0, A20_GPU_IOCTL_*)
   ▼
kernel/fs/devfs/devfs.c   DEVFS_DRM open → drm_create_vfile()
   ▼
kernel/drivers/gpu/drm.c drm_ioctl() default 分支
   │  req ∈ [A20_GPU_IOCTL_BASE, +16) → ops->ioctl(gpu_device, req, arg)
   ▼
kernel/drivers/gpu/virtio_gpu.c gpu_ioctl()
   │  copy_from_user(req) → 分发到 CTX_CREATE / RES_CREATE_3D / SUBMIT_3D ...
   ▼
virtio_gpu_send_cmd() / virtio_gpu_send_cmd_big() / virtio_gpu_submit_3d()
   │  controlq 描述符链 + MMIO notify
   ▼
QEMU virtio-gpu-gl → virglrenderer → 宿主 GPU/CPU
```

---

## 3. 内核驱动实现

### 3.1 feature 协商（`virtio_gpu_init_transport`）

- 读取 device features 低 32 位，协商 `VIRTIO_GPU_F_VIRGL` 与 `VIRTIO_GPU_F_EDID`；高 32 位只协商 `VIRTIO_F_VERSION_1_BIT`。
- 协商结果记录在 `inst->virgl` 与 `inst->context_init`。
- 与 2D 一样 setup controlq（queue 0），发送请求-响应对。

### 3.2 命令传输

- `virtio_gpu_send_cmd()`：固定 128 字节命令槽，实例内 `command_req/command_resp`，用于 capset info、ctx、resource 等小命令。
- `virtio_gpu_send_cmd_big()`：动态增长 `big_req/big_resp`，用于 `GET_CAPSET` 大 blob。
- `virtio_gpu_submit_3d()`：三描述符链（hdr+entry → 命令 blob → 响应），命令 blob 拷入 `big_req` 保证 DMA 安全。**禁止把调用者栈对象直接写入 descriptor。**
- 所有命令在 `command_lock` 串行；等待期间若注册了 IRQ 则 park，否则有界轮询 + yield。

### 3.3 3D 命令封装

| 封装 | 对应命令 | 说明 |
|---|---|---|
| `virtio_gpu_get_capset_info` | `GET_CAPSET_INFO` | 查 capset id/version/size |
| `virtio_gpu_get_capset` | `GET_CAPSET` | 拉取 capset blob |
| `virtio_gpu_ctx_create` | `CTX_CREATE` | 建 host GL 上下文 |
| `virtio_gpu_ctx_destroy` | `CTX_DESTROY` | 销毁上下文 |
| `virtio_gpu_resource_create_3d` | `RESOURCE_CREATE_3D` | 建 GL 资源 |
| `virtio_gpu_resource_unref` | `RESOURCE_UNREF` | 释放资源 |
| `virtio_gpu_submit_3d` | `SUBMIT_3D` | 提交命令流 |

### 3.4 DRM 透传（`kernel/drivers/gpu/drm.c`）

`drm_ioctl()` 的 `default` 分支识别 `A20_GPU_IOCTL_*` 区间并转发到 GPU 驱动的 `ioctl`。`/dev/dri/card0` 因此成为 3D 与 KMS 的统一入口。

### 3.5 drvmod 导出

`kernel/drvmod/framework.c` 的 `drv_export_table` 新增 `copy_from_user`/`copy_to_user`，供 virtio-gpu 模块透传用户请求。

### 3.6 一个已知约束：PIC 跳转表

drvmod 模块以 `-fPIC` 编译，`gpu_ioctl` 若用 `switch` 分发会生成 PIC 跳转表（`R_RISCV_ADD32/SUB32`），drvmod loader 不支持该类 reloc。**必须用 if-chain 分发**（代码中已注释说明）。新增 3D 命令时不要在该函数里引入 `switch`。

---

## 4. 用户态开发指南

### 4.1 自建测试：`gpu3d_test`

`user/cmds/core/gpu3d_test.c` 是验证内核 3D 链路的独立工具，ioctl 号与结构在文件内自包含（不依赖内核头）：

```sh
# 在 QEMU 里运行
gpu3d_test
# 期望输出：
#   GPU3D_TEST: virgl available
#   GPU3D_TEST: context 1 created
#   GPU3D_TEST: 3D resource 2 created (16x16 RGBA8)
#   GPU3D_TEST: resource 2 released
#   GPU3D_TEST: context destroyed
#   GPU3D_TEST: PASS
```

2D-only 设备（`virtio-gpu-device`）输出 `2D-only device, skipping 3D path` 并返回 0。

### 4.2 完整的 virgl 客户端栈（后续阶段）

内核透传只是搬运层。要跑起真实 GL 应用，需要用户态：

1. **libdrm**：`drmOpen` `/dev/dri/card0`、dumb-buffer 管理（已有，`user/external/gui/libdrm`）。
2. **libgbm**：GBM 提供 EGL 平台抽象；Mesa 的 `virtio_gpu` 后端把 GBM surface 映射到 virgl resource。
3. **Mesa**（EGL/GLES2）：`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, ...)` 创建 EGL display；virgl 驱动把 GL 调用序列化进 command buffer，经 `A20_GPU_IOCTL_SUBMIT_3D` 提交。
4. **合成器/应用**：Wayland 合成器（Weston 的 DRM backend + EGL 渲染器）或直接 EGL 客户端。

对接时内核侧需要补充的部分（按依赖顺序）：

- `RESOURCE_ATTACH_BACKING` / `TRANSFER_TO_HOST_3D` 透传（资源数据上传）。
- `BLOB` 命令（`RESOURCE_CREATE_BLOB` / `MAP_BLOB`）用于共享内存资源。
- DRM 侧 `GETFB2` / `PRIME_HANDLE_TO_FD` / `SYNCOBJ` 等与 GBM 深度配合的 ioctl。

### 4.3 运行环境

- 3D 需要 QEMU 以 `virtio-gpu-gl-*` 设备启动，并带 OpenGL 显示后端（如 `-display egl-headless`）。
- 检查宿主 QEMU 是否编译了 virgl：`qemu-system-riscv64 -device help | grep virtio-gpu-gl`，并确认 `libvirglrenderer.so` 存在。
- 2D 模式（`virtio-gpu-device`）不受影响，内核自动回退。

---

## 5. 验证与验收

内核侧验收（已完成）：

```sh
# riscv64 dev 镜像 + virtio-gpu-gl-device + -display egl-headless
(sleep 8; printf 'gpu3d_test\npoweroff\n') | qemu-system-riscv64 \
    -machine virt -m 1G -nographic -smp 1 -bios default \
    -global virtio-mmio.force-legacy=false \
    -drive file=.../fat32.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -display egl-headless -device virtio-gpu-gl-device,bus=virtio-mmio-bus.7 \
    -kernel .../kernel.elf
```

boot 日志应出现：

```
[GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=308 ctx_init=1
[GPU] virtio-gpu ready: 1024x768 (FB: 3 MB at 0x...)
```

随后 `gpu3d_test` 输出 PASS。2D 回归：用 `virtio-gpu-device` 应输出 `2D only (no VIRGL feature)` 且 `gpu3d_test` 跳过。

---

## 6. 文件索引

| 文件 | 内容 |
|---|---|
| `kernel/include/drivers/gpu/virtio_gpu.h` | virtio-gpu UAPI：命令/响应/capset/结构 + A20 3D ioctl 与 `virtio_gpu_3d_req` |
| `kernel/drivers/gpu/virtio_gpu.c` | 驱动：feature 协商、capset、3D 命令封装、`gpu_ioctl` 分发 |
| `kernel/drivers/gpu/drm.c` | DRM `/dev/dri/card0`：KMS + A20 3D 透传 |
| `kernel/drvmod/framework.c` | drvmod 导出表（含 `copy_from_user/to_user`） |
| `user/cmds/core/gpu3d_test.c` | 用户态 3D 自测 |
| `user/Makefile` | `gpu3d_test` 编译规则 |
| `docs/drivers/classes/display.md` | Display 类总文档（2D + 3D 综述） |

---

## 7. 相关设计约束

- **DMA 安全**：所有交给 virtio-gpu 的描述符与数据必须来自驱动持有的稳定内存（实例成员或 `kmalloc`），不能引用用户栈/调用者栈。
- **同步命令串行**：controlq 一次一个 in-flight 链，用 `command_lock` 保护；等待期间保持中断开启，避免 `spin_lock_irqsave` 包裹设备完成等待。
- **reloc 限制**：drvmod 模块的 `gpu_ioctl` 必须用 if-chain，不用 switch（PIC 跳转表 reloc 不被 loader 支持）。
- **2D/3D 共存**：2D fbdev 路径与 3D 透传路径互不干扰；无 virgl 时自动回退 2D。

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

1. **libdrm**：`drmOpen` `/dev/dri/card0`、dumb-buffer 管理（由 Alpine `libdrm` 包提供）。
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

---

## 8. 用户态 GL（Mesa/EGL/GBM）的前置条件

内核侧 3D 命令透传（第 2 节）可用，`xfce` world 现在也装了 Mesa
（`mesa`/`mesa-dri-gallium`/`mesa-gl`/`mesa-gles`/`mesa-egl`/`mesa-gbm`/
`virglrenderer` + `mesa-utils`/`mesa-demos`）。镜像里能查到
`swrast_dri.so`/`kms_swrast_dri.so`/`virtio_gpu_dri.so`/`zink_dri.so` 与
`libGL.so.1`/`libEGL.so.1`/`libgbm.so.1`/`libGLESv2.so.2`。但**用户态 GL
仍然起不来**，缺的是 DRM 侧的三样东西：

| 缺口 | 为什么需要 | 现状 |
|---|---|---|
| `DRM_IOCTL_GEM_CREATE` / `GEM_MMAP`（+ `GEM_FLINK`/`GEM_OPEN`） | Mesa/GBM 用它分配「可渲染」缓冲并 mmap 到用户态；没有它 `gbm_bo_create(GBM_BO_USE_RENDERING)` 直接失败 | 未实现（只有 `MODE_CREATE_DUMB`/`MAP_DUMB`，那是给 scanout 的线性缓冲） |
| render node `/dev/dri/renderD128` | 没有 DRM master 的普通程序要打开 GPU 只能靠 render node；`EGL_PLATFORM=surfaceless` 也要它 | **已加**：devfs 现在暴露 `renderD128`（226,128），打开它的 DRM 上下文标记为 render-only（`SET_MASTER` 返回 `-EACCES`，KMS ioctl 本就需要 master）。实测加上后 EGL 能在 Wayland 平台初始化（`EGL driver name: swrast`，且带 `EGL_EXT_image_dma_buf_import`） |
| plane **IN_FORMATS** blob | Mesa 的 DRM 平台用它构造 EGL config 列表；wlroots 也用它取 plane 的格式集 | **已实现**（`adeffcb9`）：plane 现在带 `type`(id=2) + `IN_FORMATS`(id=3) 两个属性，IN_FORMATS 指向一个 56B `drm_format_modifier_blob`（header 24B / formats@24 / modifier@32，ARGB8888+XRGB8888、LINEAR）。boot 验证：桌面正常起来（page-flip 正常、xfsettingsd/xfdesktop 都在、**不再** `Failed to create DRM backend`）。注意 wlroots 只在用 GL 渲染器时才**懒读**这个 blob——`WLR_RENDERER=pixman` 下根本不取它，所以本改动对当前桌面零影响。blob 是否被 wlroots/Mesa 正确解析，要等 GL 渲染器链路打通后才能端到端验证。 |

**IN_FORMATS 四次尝试的二分结论**（关键：触发器不是 blob，而是「plane 报了多于一个属性」）：

| 变体 | 结果 |
|---|---|
| 裸 blob（IN_FORMATS，BLOB+LINEAR） | `Failed to create DRM backend` |
| blob + `MODE_GETPLANE` 报同样两个 fourcc | 同样失败 |
| blob + `WLR_DRM_NO_MODIFIERS=1` | 同样失败 |
| 属性改名为 `FOO`（wlroots 认不出） | 同样失败 → **不是名字** |
| 属性改成 ENUM（不是 BLOB） | 同样失败 → **不是 BLOB 类型** |
| **两个属性都是已知可用的 `type`/PRIMARY** | 同样失败 → **不是第二个属性的处理代码** |

并且：在全部失败变体里，**IN_FORMATS 的 blob 从未被取过**（我在 `drm_in_formats_blob()` 里加的打印一次都没出现），说明失败发生在 wlroots 读 plane 属性列表的阶段，**早于**格式/blob 解析；wlroots 也没有打出任何具体错误行（静默失败）。所以问题不在内核返回的 blob 字节，而在「这个驱动上 plane 有 2 个属性」这一事实本身触发 wlroots/libdrm 的某个前置检查失败。

> **更正（adeffcb9，已落地）**：上面这条「plane 有 2 个属性就失败」的结论是错的。后来用一个干净的「两个 `type` 属性」复现（count_props=2）发现 wlroots **完全正常**（桌面起来、page-flip 正常）——所以多属性列表编码从来不是问题，之前那几次 IN_FORMATS 失败是**那些尝试自身的接线/编码 bug**（与本会话里我几次探针自身的栈 bug 同理）。`adeffcb9` 的实现让 plane 同时暴露 `type` + `IN_FORMATS`，boot 验证桌面无恙。IN_FORMATS 这条路本身已经通了；剩下要验证的是 blob 内容被 wlroots/Mesa 正确解析（需 GL 渲染器链路），以及后续的 GBM/EGL/真 PRIME。

下一步（下次接手时的第一件事）：写一个最小用户态程序，用 libdrm 调 `drmModeGetPlane()` + `drmModeObjectGetProperties()`，把 1 属性与 2 属性两种情况的返回值打出来；或直接读 wlroots 的 plane 属性循环源码（本机网络取不到，需离线准备）。

**上一轮用临时 ioctl 打印得到的调用序列**：

- wlroots 调用序列（失败前最后一段）：`GETRESOURCES` → `GETCRTC` → `OBJ_GETPROPERTIES`×2 → `GETPLANERESOURCES`×2 → `GETPLANE`×2 → `OBJ_GETPROPERTIES`×2 → `GETPROPERTY`×4（= plane 两个属性各两次「先取长度再取值」）→ `OBJ_GETPROPERTIES`×2 → `DROP_MASTER`(0x641f) → 失败。
- 最后的 `DROP_MASTER` 是 libdrm `drmGetNodeTypeFromFd()` 的 node-type 探测，即它发生在一个**新打开的 fd** 上——对应 wlroots 的 allocator 阶段（`wlr_drm_allocator_create`：先试 `drmModeCreateLease`，失败后 plain open 再探测），而不是 plane 循环。所以失败点在 **allocator**（dumb/gbm 后端用 plane 的格式集），不在属性本身。
- 全流程里 wlroots 唯一未实现的 ioctl 是 `DRM_IOCTL_MODE_CREATE_LEASE`（`0xc6`），它自己会 `falling back to plain open`，非致命。
- 下一步建议：写个用户态小程序 `drmModeGetPropertyBlob(IN_FORMATS)` + `drmModeGetPlane`，把内核返回的字节与 wlroots/Mesa 的解析逐字段对齐；或先只让 **dumb allocator** 的格式来源自洽（GETPLANE 与 IN_FORMATS 完全一致、并确认 modifier 解析出 XRGB8888+LINEAR），再往上试 GBM。
| 真正的 dma-buf（PRIME） | 客户端把渲染结果当 `wl_buffer` 交给合成器（`zwp_linux_dmabuf_v1`）；dumb buffer 导不出 dma-buf | `DRM_IOCTL_PRIME_HANDLE_TO_FD` 现在是**把缓冲内容 memcpy 进 memfd** 再返回该 fd（`drm_prime_handle_to_fd`），不是可共享的 dma-buf |
| `DRM_IOCTL_MODE_GETFB2` | Mesa/合成器导入 framebuffer（XWayland/DRI3 等） | 只有 `MODE_GETFB` |

实测（guest 内 `eglinfo`）：

```
libEGL warning: failed to get driver name for fd -1
libEGL warning: MESA-LOADER: failed to retrieve device information
MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)
libEGL warning: egl: failed to create dri2 screen
```

直接后果：

- **合成器只能跑 pixman**：`start-xfce4-session` 写死 `WLR_RENDERER=pixman`，
  因为 wlroots 的 GL 渲染器要先 `gbm_create_device()` +
  `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR)`，上面几样缺一不可。
  XWayland 也只能 `Failed to initialize glamor, falling back to sw`。
- **GL 客户端（Minecraft 等）无法出图**：即使 llvmpipe 能软件渲染，结果也
  无法作为 dma-buf 交给合成器。
- **host 侧也没有 3D**：GUI 实例用 `-device virtio-gpu-pci`（无 virgl），
  所以 `A20_GPU_IOCTL_VIRGL_CHECK` 返回 `-ENXIO`；要试硬件 3D 得换
  `virtio-gpu-gl-pci`（或 `virgl=on`）。

### 让 Minecraft 跑起来的顺序

1. 内核补 `GEM_CREATE`/`GEM_MMAP`（把现有 vmo 暴露成 GEM 对象）+ render node
   + `MODE_GETFB2`；
2. 把 PRIME 做成真 dma-buf（或至少让 `kms_swrast` 的 dumb buffer 能被合成器
   直接采样）；
 3. 放开 `WLR_RENDERER`，让 wlroots 用 GL 渲染器（llvmpipe 软件渲染先跑通）；
 4. 再考虑 virgl：host 开 `virtio-gpu-gl-pci`，guest 用 `virtio_gpu_dri.so`；
 5. 注意 **LWJGL 只提供 x86_64/aarch64 native**，riscv64 实例跑不了 Minecraft
    （Java 本身可以）。

### GBM/EGL 的实测定位（本轮）与一条可能更短的路

- `eglinfo -p gbm` 现在能给出精确失败点：`eglInitialize` → `DRI2: failed to create gbm device`。
  即 Mesa 的 GBM 后端（镜像里有 `/usr/lib/gbm/dri_gbm.so`）在为本驱动的设备建 `gbm_device` 时失败。
  `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` 和 `swrast` 都试过、**都不改变结果** → 说明卡点不在
  「驱动名→DRI 驱动」的映射，而在 dri_gbm 的设备初始化本身（它对本 DRM 设备的某些查询/调用不满意）。
  下一步要么 guest 里 strace（world 没装 strace）看 gbm_create_device 里哪一步失败，要么在内核 DRM ioctl
  分发上挂临时 trace 看 Mesa 在初始化阶段问了什么、哪条答得不对。
- **可能更短的路（待验证，不要当结论）**：Minecraft 走 LWJGL → `EGL_PLATFORM_WAYLAND`，而 EGL 在 Wayland
  平台**已经能初始化**（swrast）。真正的问号是**呈现**：eglSwapBuffers 时 Mesa 把渲染结果作为 `wl_buffer` 交给
  合成器。若 Mesa 的 Wayland EGL 对软件渲染用 `wl_shm`（CPU 共享内存，wlroots 原生支持）而非 `wl_dmabuf`，
  那 Minecraft 也许**根本不需要 GBM/真 dma-buf 这条链**。验证方法：跑一个真正的 EGL-Wayland 客户端
  （镜像里没有编译器，也无 weston-simple-egl，需要离线准备一个静态 musl 的 Wayland EGL 测试程序）。
- 另一个独立于 GL 的硬前提：**LWJGL natives + Minecraft jars 不在镜像里**，离线取不到，需另行准备。
- **补测（环境侧已齐备，"缺驱动"这条排除）**：镜像 `/usr/lib/dri/` 里其实**全都有**——`swrast_dri.so`、
  `kms_swrast_dri.so`、`virtio_gpu_dri.so`、`zink_dri.so`，以及 crocus/i915/iris/nouveau/r300/r600/radeonsi/vmwgfx；
  GBM 后端 `/usr/lib/gbm/dri_gbm.so` 也在。即便如此，带 `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` 的
  `eglinfo -p gbm` 仍然 `DRI2: failed to create gbm device`，而且**内核侧没有任何 DRM ioctl 返回错误**
  （用临时 trace 覆盖了整个 ioctl 分发验证过）。→ 卡点在 **Mesa `gbm_create_device()` 内部**（设备识别/后端
  初始化），既不是缺文件也不是内核答错。要继续需要 Mesa 源码，或在 guest 里 strace（world 现无 strace）
  看它到底哪一步返回 NULL。
- **真正的卡点定位（libdrm 读不到设备身份）**：`MESA-LOADER: failed to retrieve device information` 出在 libdrm 的
  `drmGetDevice2()`。从 `libdrm.so.2` 的字符串可以读出它要访问的 sysfs 路径：
  `/sys/dev/char/<maj>:<min>` → `/device` → `/device/drm`，以及
  `/sys/bus/pci/devices/<bdf>/` + `<bdf>/config` + `<bdf>/uevent`。
  而客体里实测：**`/sys/bus` 根本不存在**；`/sys/dev/char/226:0` 的 readlink 是 `../../class/drm/card0`
  （路径里没有 `pci`，libdrm 因此识别不出这是 PCI 设备）；`/sys/dev/char/226:0/device` 是个**空目录**
  （没有 `uevent`/`config`）。→ libdrm 拿不到 vendor/device，Mesa loader 无法把设备映射到 DRI 驱动，
  `gbm_create_device()` 于是返回 NULL，往外就是 `DRI2: failed to create gbm device`。
- **修法（内核 sysfs，尚未动手）**：把 DRM 设备的 sysfs 摆成 Linux 的形态——`/sys/dev/char/226:0` 应指向
  `/sys/devices/pci0000:00/<bdf>/drm/card0`，在 `<bdf>/` 下提供 `uevent`（含 `PCI_ID=1AF4:1050` 等）、
  `config`（PCI 配置空间）、`vendor`/`device`，并提供 `/sys/bus/pci/devices/<bdf>` 的链接。
  内核侧信息是齐的（`pci_dev_info` 带 vendor/device，`pci_slot_for_bdf()` 带槽位）。
  **注意**：这会动到现有 `/sys/class/drm/*` 布局，而 wlroots/libinput 现在依赖它——改之前必须先跑桌面回归，
  别把已经能用的桌面弄坏；而且这只是 GL 链的第一环，后面还有 kms_swrast 建 screen、EGL、真 dma-buf PRIME、
  MODE_GETFB2、以及放开 `WLR_RENDERER`。

## 9. libdrm 设备识别所需的 sysfs 形态（反汇编实证，可直接照做）

上一节的结论来自字符串，本节把它升级为**逐条反汇编核实**的事实。样本：客体实际使用的
`libdrm 2.4.131`（从 `build/cache/apk/x86_64/libdrm-2.4.131-r0.400a5d6e.apk` 取出）。
复现命令：

```bash
cd /tmp/opencode && mkdir drmx && tar -xzf <apk> -C drmx
objdump -d --no-show-raw-insn drmx/usr/lib/libdrm.so.2.131.0 > drm.asm
python3 -c "d=open('libdrm.so.2.131.0','rb').read(); print(repr(d[0x10a08:0x10a30].split(b'\0')[0]))"
```

### 9.1 调用链（Mesa 的 `gbm_create_device` 走的就是这条）

`drmGetDevice2(fd)` → `drmGetDeviceFromDevId(dev_id, flags, &dev)`：

1. 枚举 `/dev/dri/*`（字符串 `/dev/dri`，0x101e6；名字前缀匹配 `card`(4) / `renderD`(7)，0x101fc/0x10201）。
2. 对每个 node 先 `stat("/sys/dev/char/<maj>:<min>/device/drm")`（格式串 0x10a08）。
   **失败即 `-EINVAL` 直接返回** —— 这是整条链的**第一个、也是最硬的失败点**。
3. `drmGetNodeType(maj, min)`：`snprintf("/sys/dev/char/<maj>:<min>/device")`（0x1021f）→ 解析该路径
   → 用一张 7 项表把路径后缀映射成 bus 类型（见 9.2）。**它必须被识别成某一种 bus**，否则 node 类型
   未知、直接失败。
4. `drmGetDeviceName()`（0x66a0）：`realpath("/sys/dev/char/<maj>:<min>/device")`；若最后一段以
   **`/virtio`** 开头则截断到它的父目录（对应 Linux 上 `.../0000:00:01.0/virtio0` 的经典布局）。
5. PCI 分支：从第 4 步得到的目录读 **5 个属性文件**（表 @0x14840，`fscanf("%x")` 0x1031e）：
   `revision`、`vendor`、`device`、`subsystem_vendor`、`subsystem_device`。任一读不到即失败。
6. PCI 分支另读 `<pci>/uevent`（0x10215）并用 `sscanf("%04x:%02x:%02x.%1u")`（0x102e3）解析
   **`PCI_SLOT_NAME=`**（0x102d5），以及 `<pci>/config`（0x102f6）。
7. 枚举路径（`drmGetDevices2`）另用 `/sys/bus/pci/devices/%04x:%02x:%02x.%d/`（0x10c40）与
   其 `/drm`（0x10c10）。

### 9.2 bus 类型判定表（0x61b0，7 项 {后缀, 类型}）

| 路径后缀 | 判定 |
|---|---|
| `/pci` | PCI |
| `/usb` | USB |
| `/platform` | platform |
| `/spi` | platform |
| `/host1x` | host1x |
| `/virtio` | virtio |
| `/faux` | faux |

→ **关键**：`/sys/dev/char/226:0` 解析后的路径里必须出现上表之一。当前客体是 `../../class/drm/card0`
（`/sys/class/drm/card0`），**一个都不匹配** —— 这就是「libdrm 识别不出这是 PCI 设备」的确切原因。

### 9.3 因此需要的 sysfs 节点（Linux `virtio-pci` 形态）

```
/sys/dev/char/226:0            -> symlink 到 /sys/devices/pci0000:00/0000:00:01.0/drm/card0
/sys/dev/char/226:0/device     -> .../0000:00:01.0/virtio0        （含 /pci + /virtio，两种判定都能命中）
/sys/devices/pci0000:00/                                          （目录，父名含 /pci 供 "/.." 判定）
/sys/devices/pci0000:00/0000:00:01.0/
    vendor device subsystem_vendor subsystem_device revision       （`%x` 文本）
    config                                                        （PCI 配置空间前 64B）
    uevent                                                        （含 PCI_SLOT_NAME=0000:00:01.0）
    drm/                                                          （目录）
        card0  renderD128                                         （条目）
/sys/bus/pci/devices/0000:00:01.0 -> 上述 PCI 目录              （枚举路径）
```

其中 vendor/device 直接用 `pci_dev_info` 里的值（QEMU 的 virtio-gpu-pci 是 `1af4:1050`）；
BDF 用哪个都行，**只要 realpath 与 `/sys/bus/pci/devices/<bdf>` 自洽**——libdrm 是从 realpath 反解 BDF 的。

### 9.4 实施要点与风险

- 落点：`kernel/fs/sysfs.c`（枚举 `sf_type_t`、`sysfs_lookup`、`sysfs_content`、stat/readlink 四处），
  需要时给 `kernel/drivers/bus/pci_bus.c` 加一个公开的 PCI 信息访问器（`g_pci_infos[]` 目前是 `static`）。
- **必须保持加法式**：`/sys/class/drm/*` 与 `/sys/dev/char/<maj>:<min>` 的现有 readlink 是
  wlroots/libinput 正在依赖的布局，先跑桌面回归再谈其它。
- 改完的验收信号：`eglinfo -p gbm` 的报错**不再是** `MESA-LOADER: failed to retrieve device information`，
  而是越过设备识别、进到建 screen / 选 renderer 的下一层；同时桌面（labwc + 视频）无回归。

### 9.5 更低侵入的实现变体（推荐先试这个）

9.3 里把 `/sys/dev/char/226:0` 本身改成指向 `/sys/devices/...` 的软链，会动到一条**有明确不变量**的
现有 readlink：`sysfs_readlink()` 对 `SF_DEV_CHAR_ENTRY` 生成 `../../class/<sub>/<name>` 是刻意的——
注释写明 libudev 的 `util_resolve_sys_link()` 解析结果必须与「从 `/sys/class` 枚举得到的 syspath」
**逐字节相等**，否则 libinput 的 `evdev_device_have_same_syspath()` 会失配（输入设备依赖这条）。
**因此不要去改它。**

够用且几乎纯加法的做法：**只把 `/sys/dev/char/226:0/device` 改成一个符号链接**
（该节点当前是个没人读的空目录），指向 `/sys/devices/pci0000:00/0000:00:01.0/virtio0`：

- `drmGetNodeType()`：解析 `/sys/dev/char/226:0/device` → `/sys/devices/pci0000:00/0000:00:01.0/virtio0`
  → 同时命中 `/pci` 与 `/virtio` → 判定为 PCI ✓
- `drmGetDeviceName()`：realpath 同上，按 `/virtio` 截断 → `/sys/devices/pci0000:00/0000:00:01.0`
  → 从那里读 5 个属性 ✓
- `stat("/sys/dev/char/226:0/device/drm")` → `.../0000:00:01.0/virtio0/drm` 存在 ✓
- 注意要**只对 DRM 226:0 特判**：`SF_DEV_CHAR_ENTRY` 的 `device` 子节点是输入设备共用的，
  全局改成软链会牵动 evdev 一侧。

`/sys/bus/pci/devices/<bdf>` 只被 `drmGetDevices2` 枚举路径用到（9.1 第 7 条），若首轮验证只关心
`gbm_create_device`，可以先不提供，等确认需要再补。

### 9.6 实施状态（本轮已落地，commit `193ee6b3`）

已在内核 `kernel/fs/sysfs.c` 按 9.5 实现，**纯加法**，未改动任何既有节点的类型或 readlink：

| 节点 | 状态 |
|---|---|
| `/sys/dev/char/226:0/device/subsystem` | 软链 → `/sys/bus/virtio` |
| `/sys/dev/char/226:0/subsystem` | 软链 → `/sys/bus/pci` |
| `.../device/{revision,vendor,device,subsystem_vendor,subsystem_device}` | 十六进制文本，实测可读 |
| `.../device/uevent` | 含 `PCI_SLOT_NAME=0000:00:01.0` |
| `.../device/config` | 64B PCI 配置头 |
| `.../device/drm/{card0,renderD128}` | 条目 |
| `/sys/dev/char/226:128` | 新增 render 节点识别（此前完全不可解析） |

**两条 subsystem 链接必须目标不同**：反汇编显示 `drmGetNodeType()` 把 readlink 出的目标串拿去匹配一张
7 项表，`/pci`→0、`/virtio`→0x10，且代码显式比较 `0x10`；`device/subsystem` 命中 `/virtio` 才会进入
libdrm 的 virtio 分支（再上溯一级读 PCI 属性），父级链接则命中 `/pci`。

**实测（客体）**：所有节点读回正确；桌面（labwc + xfwm4 + panel + thunar，含 polkit/dbus）无回归、无
panic/UBSAN；在内核 `sysfs_lookup` 挂临时 trace 后可见 libdrm 的完整查找序列——
`device→drm`、`device→subsystem`、`subsystem`、`device→uevent`、`device→vendor`、`device→device`、
`device→subsystem_vendor`、`device→subsystem_device`——说明**设备身份识别已走通**（此前直接放弃）。

**仍未通**：`eglinfo -p gbm` 依旧 `DRI2: failed to create gbm device`，且 Mesa 仍打印
`failed to retrieve device information`。trace 里 **`config` 与 `revision` 始终未被查找**，说明流程在属性读取
附近就中止了。

**下一步（最有依据的一条）**：把设备路径摆成**真实 Linux 的 virtio/PCI 层级**——
`/sys/dev/char/226:0/device` 应解析到 `/sys/devices/pci0000:00/0000:00:01.0/virtio0`，这样
`drmGetDeviceName()` 的 `/virtio` 截断才会得到 PCI 父目录 `/sys/devices/pci0000:00/0000:00:01.0`，
`<realpath>/../subsystem` 才解析得到 `/sys/bus/pci`，`PCI_SLOT_NAME` 与 `config` 也才从那个父目录读取。
这需要新增 `/sys/devices/pci0000:00/...` 子树（约 100 行，仍是纯加法 + 把 DRM 的 `device` 改成软链）。
另需 `strace`/更细的 trace 确认 `config` 为何未被读取。

**另一条已实测可行的路（对 Minecraft 可能已够）**：`eglinfo -p wayland` **成功**，渲染器为
`llvmpipe (LLVM 21.1.2, 128 bits)`。LWJGL 走 `EGL_PLATFORM_WAYLAND`，若其呈现走 `wl_shm` 而非
`wl_dmabuf`，则 Minecraft 可能**不需要 GBM 这条链**。代价仍是 LWJGL natives + jars 不在镜像里。

### 9.7 补进 `/sys/bus` 之后：trace 修正与真正的停点（commit `7a6ad59c`）

已补 `/sys/bus/pci/devices/0000:00:01.0/{config,uevent,vendor,device,revision,subsystem_vendor,
subsystem_device,drm}`（内容生成器与 char-device 侧共用）。实测 `config` / `uevent` / `vendor`
均可读回，`config` 是合法的 64B PCI 头。

**但必须纠正一条中途的错误结论**：先前看到 `SYSLOOKUP t=0 name=bus` 就认定 `/sys/bus` 是卡点。
把 trace 扩到顶层后对照才发现——那 6 次 `bus` 查找**来自诊断脚本自己的 `ls`/`cat`**（我在读
`/sys/bus/...` 的节点），而 **`eglinfo -p gbm` 运行期间 libdrm 一次都没查 `/sys/bus`**。
所以这个补丁本身**并没有**解开 `gbm_create_device()`。教训：trace 只能看"问了什么"，
不能直接归因到某个消费者；要区分必须按进程/时间窗隔离。

**eglinfo 运行期间 libdrm 的真实序列**（t=30 是 `/sys/dev/char/<maj>:<min>`，t=32 是 `.../device`）：

```
device→drm, device→subsystem, subsystem(char级),
device→uevent,
device→vendor, device→device, device→subsystem_vendor, device→subsystem_device
```

然后序列就停了。**`revision` 与 `config` 从头到尾没有被查找过。**

**由此得到的真正停点**：libdrm 读完了 uevent 与 4 个属性（缺 `revision`），随后中止。两种可能：
(a) 属性表顺序与预期不同，`revision` 不是 index 0，而第 5 个属性读取失败导致整轮失败；
(b) 流程在属性读取之后、读 `config` 之前因为别的判断而返回错误。
要区分，下一步的 trace 需要**记录查找结果**（而不只是尝试），并覆盖 `sysfs_stat`/`sysfs_open_vnode`
与 attr 文件的 `read`，才能看出是"没问"还是"问了但失败"。

**结论**：设备身份识别已从"完全放弃"推进到"读完 uevent + 4 属性"；`gbm_create_device()` 仍未通，
且**已验证它不是 `/sys/bus` 缺失导致**。同时 `eglinfo -p wayland` 依旧成功（llvmpipe），
这仍是对 Minecraft 最有希望的一条路。

### 9.8 细粒度 trace：卡点不在 sysfs 侧（commit `d8d593b1`）

把 trace 下沉到 `sysfs_lookup` + `sysfs_open_vnode` + `sysfs_fread` 三处（分别记「查找/打开/读取」），
拿到了本轮最硬的一组事实。`eglinfo -p gbm` 期间，**libdrm 的全部 sysfs 交互**是：

```
SYSOPEN t=38 idx=57984   SYSREAD t=38 idx=57984 len=86   ← uevent（57984=0xE280 → 226:128 render 节点）
SYSOPEN t=36 idx=1       SYSREAD t=36 idx=1 len=5        ← vendor            = "1af4"
SYSOPEN t=36 idx=2       SYSREAD t=36 idx=2 len=5        ← device            = "1050"
SYSOPEN t=36 idx=3       SYSREAD t=36 idx=3 len=5        ← subsystem_vendor  = "1af4"
SYSOPEN t=36 idx=4       SYSREAD t=36 idx=4 len=5        ← subsystem_device  = "1100"
```

三条关键推论：

1. **libdrm 处理的是 render 节点 226:128**（`idx=57984`），不是 card0。9.6 里给 render 节点补的身份识别
   是这条链的必需项，不是可选项。
2. **`revision`（idx=0）和 `config` 从未被 open/read。** 而 libdrm 实际读的那 4 个属性**全部读到了正确值**。
   → **内核侧没有任何一个节点"提供不出来"**：卡点在读完这 4 个属性**之后**，不在 sysfs。
3. **改 subsystem 链接目标（`/sys/bus/pci` ↔ `/sys/bus/virtio`）对读取序列没有任何影响**——逐字节相同。
   因此 `d8d593b1` 选择 `/sys/bus/pci` 是基于 Mesa `loader_get_pci_id_for_fd()` 要求 `DRM_BUS_PCI`
   这一契约的**主动选择**，而非观测到的修复。

**因此下一步不该再往 sysfs 加节点。** 要解开 `gbm_create_device()`，需要看到那 4 次读之后 libdrm/Mesa
做了什么——可行手段：
- 给内核 syscall trace 加上**进程过滤**，把 eglinfo 之后的 `ioctl`（尤其 DRM ioctl）/`mmap`/后续 `openat`
  完整记下来（9.7 的教训：trace 必须能归因到具体消费者）；
- 或者拿到 Mesa/libdrm 源码后直接对照 `loader_get_pci_id_for_fd()` 与 `gbm_create_device()` 的分支条件
  （当前只能从反汇编推断"要求 DRM_BUS_PCI"）。
- `strace` 仍是最省事的工具，但 world 里没有，需离线准备静态 musl 版本。

**已验证可行的替代路径没有变化**：`eglinfo -p wayland` 成功、渲染器 `llvmpipe (LLVM 21.1.2, 128 bits)`。
考虑 Minecraft 走 `EGL_PLATFORM_WAYLAND`，这条路值得优先于继续啃 GBM。

### 9.9 环境覆盖开关全部无效：GBM 失败与驱动选择无关

为排除"只是选错了驱动"这一可能，一次性测了 Mesa 的全部常用覆盖开关（均在 `eglinfo -p gbm` 下）：

| 环境 | 结果 |
|---|---|
| `LIBGL_ALWAYS_SOFTWARE=1` | `eglInitialize failed` |
| `GALLIUM_DRIVER=llvmpipe` | `eglInitialize failed` |
| `MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu` | `eglInitialize failed` |
| `MESA_LOADER_DRIVER_OVERRIDE=swrast` | `eglInitialize failed` |
| `MESA_LOADER_DRIVER_OVERRIDE=zink` | `eglInitialize failed` |

全部失败。对照之下 `eglinfo -p wayland` 成功、`eglinfo -p surfaceless` 报的是另一套错
（`egl: failed to create dri2 screen` / `DRI2: failed to create screen`）。

**结论**：`EGL_PLATFORM=GBM` 的失败**与驱动选择、与 sysfs、与软件/硬件路径都无关**，卡在 Mesa
`gbm_create_device()` 内部（`dri_gbm` 后端为这台设备建设备时返回 NULL）。这一点**无法再靠黑盒手段推进**，
必须读 Mesa 源码（`src/gbm/backends/dri/gbm_dri.c` 与 `src/egl/drivers/dri2/platform_drm.c`），
或拿到这个版本对应的调试符号。

**同时确认的可用面**：
- `EGL_PLATFORM=wayland` → 成功，`llvmpipe` 渲染器可用（Minecraft/LWJGL 走的就是这条）；
- `EGL_PLATFORM=GBM` → 不通（wlroots 的 GL 渲染器需要它，因此当前只能继续用 `WLR_RENDERER=pixman`）。

### 9.10 反汇编 Mesa 的 loader：内核侧已满足其全部 sysfs 契约

Mesa 源码拿不到，但**客体里的 Mesa 二进制可以反汇编**——和当初破解 libdrm 是同一套方法，而且更直接。
目标：`dri_gbm.so` 里内联的 mesa-loader。

**`loader_get_pci_id_for_fd()` 的完整逻辑（0x4190 起）：**

```
fstat(fd)                              失败 → 打印 "MESA-LOADER: failed to fstat fd"
  成功 → 从 st_rdev 拆出 maj/min
       → 读 /sys/dev/char/%d:%d/device/vendor   （格式串 @0x99e2，strtoll 以 16 进制解析）
       → 读 /sys/dev/char/%d:%d/device/device
  两者都非 0 → return true                      ← 直接返回，根本不会调用 drmGetDevice2
  否则      → drmGetDevice2(fd)        失败 → "failed to retrieve device information"
                                       bustype≠0 → "device is not located on the PCI bus"
```

**三条推论：**

1. **Mesa 直读的路径正是 `/sys/dev/char/<maj>:<min>/device/{vendor,device}`**，且按 **16 进制**解析。
   我提供的节点（`1af4` / `1050`）**完全满足**，两者皆非 0 → `loader_get_pci_id_for_fd()` **返回 true**。
   → 内核侧对这个契约是**完备**的。
2. 我们看到的 `failed to retrieve device information` **不来自这个函数的主路径**（那条主路径根本不会走到
   `drmGetDevice2`）。同类型的字符串在多个 Mesa 库里都有，不能只按文案归因——这也再次印证 9.7 的教训。
3. `GBM_ALWAYS_SOFTWARE`（该 .so 里确实存在、此前漏测）实测**无效**；`get_driver_name` 走 `drmGetVersion`，
   其失败文案是 `failed to get driver name for fd %d`，也不是我们看到的那个。

**因此结论收敛为**：`gbm_create_device()` 的失败发生在 Mesa 的 gbm-dri 后端内部、**在 PCI ID 解析与驱动名
解析之后**。

**并且 `modifier` 这条线索也已排除**：反汇编 0x2440 处可见 `Only invalid modifier specified` 是通过
`fwrite` 写 stderr 且置 `errno=EINVAL` 的一条独立分支，**在我们的任何一次运行输出里都没有出现过**。
不能在"字符串里出现过"和"实际被执行过"之间划等号——这是 9.7 那条教训的又一实例。

**内核侧到此可以定论：所有 Mesa/libdrm 文档化与可反汇编出的 sysfs 需求都已满足。**
再往下必须在 Mesa 内部（或其调试符号）里推进，而非继续改内核。可用的抓手只剩：
给 `dri_gbm.so`/`libgallium-25.2.7.so` 配对应的 **debug 符号**（Alpine 有 `-dbg` 包，需离线准备），
或把 Mesa 的 `-Dbuildtype=debug` 版本换进镜像。

### 9.11 前提纠正：网络是通的；并且 Wayland 路径上的真实 GL 渲染已跑通

**先纠正一个贯穿多轮的错误前提。** 本项目此前把"网络被封"当成既定事实，并据此得出
"拿不到 Mesa 源码 → GL 阻塞"。实测 `curl https://dl-cdn.alpinelinux.org/...` 返回 200、
DNS 正常。**该前提是错的**，所有建立在它之上的"阻塞"结论都不可靠。

用这个能力做了两件事，都拿到了源码级证据：

1. **取到 Mesa 25.2.7 源码并核对**：`src/loader/loader.c` 的 `loader_get_pci_id_for_fd()`
   确实先走 `loader_get_linux_pci_id_for_fd()`——读
   `/sys/dev/char/<maj>:<min>/device/{vendor,device}` 并按十六进制解析——与 9.10 的反汇编完全一致。
   两个值都非 0 即返回 true，**根本不会调用 `drmGetDevice2()`**。内核侧对该契约是完备的。
   失败点收敛到 `dri_device_create()` → `dri_screen_create[ _sw ]()` →
   `dri_screen_create_for_driver()` → `driCreateNewScreen3()` 返回 NULL，
   即 `dri2_init_screen`（硬件）或 `dri_swrast_kms_init_screen`（软件，`GBM_ALWAYS_SOFTWARE` 走这条）
   失败。**"缺共享库"假设已排除**：`libgbm.so.1`、`libgallium-25.2.7.so`、`libLLVM`、`libdrm.so.2`
   在镜像里都在，`/usr/lib/dri/*` 是指向 `libdril_dri.so` 的正常符号链接。

2. **验证 Wayland 路径上的真实 GL 渲染（Minecraft 实际走的路径）**：
   把 `mesa-demos` 的 `egltri_wayland` / `es2gears_wayland` 注入镜像，在 labwc 会话
   （`XDG_RUNTIME_DIR=/run/user/0`、`WAYLAND_DISPLAY=wayland-0`）里运行：

   ```
   es2gears_wayland:  EGL_VERSION = 1.5          然后持续运行到测试超时（Terminated）
   eglinfo -p wayland: OpenGL core profile renderer: llvmpipe (LLVM 21.1.2, 128 bits)
                       OpenGL ES profile version: OpenGL ES 3.2 Mesa 25.2.7
   ```

   即 **EGL + GLES 3.2 的完整渲染在 Wayland 平台上可用**。途中出现的
   `failed to get driver name for fd -1` / `MESA-LOADER: failed to retrieve device information`
   是 Mesa **先用 fd=-1 探测"无设备"配置**（`drmGetVersion(-1)` 失败）再回退的中间步骤，
   不是最终失败——`EGL_VERSION = 1.5` 与持续运行证明最终配置成功。

**结论（对目标的影响）**：
- **Minecraft/LWJGL 走 `EGL_PLATFORM_WAYLAND`，这条路已验证可渲染**，很可能**不需要 GBM**；
- GBM 仍是坏的（`WLR_RENDERER=pixman` 暂时保留），但它不再是 Minecraft 的前置条件；
- "LWJGL natives + jars 取不到"这条**也不再成立**——网络是通的。是否要拉取由你决定。

### 9.12 GBM 排查补记：又两条假设被排除，失败点已钉到源码函数

拿到 Mesa 源码后继续收窄，先排除两条：

1. **"缺共享库"——排除。** 逐个对照镜像：`libgallium-25.2.7.so` 的全部 20 个 `DT_NEEDED`
   （`libLLVM.so.21.1`、`libSPIRV-Tools.so`、`libstdc++.so.6`、`libxcb-*`、`libdrm*`、`libexpat`、
   `libgcc_s`、`libz/libzstd` …）在镜像里**全都在**；`/usr/lib/dri/*` 是指向 `libdril_dri.so` 的正常
   符号链接，`libdril_dri.so` 仅依赖 `libgbm.so.1` + libc，也都在。
2. **"render 节点上 KMS ioctl 被整体拒绝"——排除（我自己的内核里就否掉了）。**
   `kernel/drivers/gpu/drm.c`：`drm_mode_getresources()` 等 KMS 入口**都没有检查 `ctx->render_only`**，
   照常执行；整个文件里只有一处 `render_only` 门控——`drm_set_master()`（第 798 行）
   `return -EACCES`。所以 render 节点上 `MODE_GETRESOURCES` 是**放行**的。

**失败点（源码级）**：`gbm_create_device()` → `dri_device_create()` →
`dri_screen_create()`（硬件）或 `dri_screen_create_sw()`（`GBM_ALWAYS_SOFTWARE`）→
`dri_screen_create_for_driver()` → `driCreateNewScreen3()` **返回 NULL**。
软件路径更具体：`pipe_loader_sw_probe_kms()` 里 `create_winsys_kms_dri(fd)` 返回 NULL 就 `goto fail`。

**剩下的两个候选**（都还没验证，供下一轮参考）：
- Mesa 的 winsys/初始化是否调用 `drmSetMaster`——本内核与 Linux 一样对 render 节点返回 `-EACCES`；
- 某条 KMS ioctl 的**返回数据**不对。注意"错误返回"与"数据错误"要分开：此前实测**没有任何 DRM ioctl
  返回错误**，所以如果问题出在 ioctl 层，形状应是"**成功但内容不对**"，而不是"被拒绝"。

**这一条不再阻塞 Minecraft**：Wayland 路径已验证可渲染（9.11）。GBM 只影响 wlroots 自己的渲染器质量。

**再补两条排除，以及一条有价值的对照：**

3. **"KMS winsys 创建失败"——排除。** `kms_dri_create_winsys()`
   （`src/gallium/winsys/sw/kms-dri/kms_dri_sw_winsys.c:510`）**除 `CALLOC_STRUCT` 失败（OOM）外不可能返回
   NULL**——它只是填一张函数指针表。所以 `create_winsys_kms_dri()` 是成功的。
4. **"DUMB ioctl 缺失"——排除。** `DRM_IOCTL_MODE_CREATE_DUMB` / `MAP_DUMB` / `DESTROY_DUMB`
   在本内核里都已实现（`drm.c:1603` 起）。

**有价值的对照（这条才是关键线索）**：`eglinfo -p wayland` 能出 `llvmpipe` 渲染器，说明 **`swrast`
（llvmpipe）这个 DRI 驱动在客体里是能正常加载并建 screen 的**。而 GBM 路径上的 `kms_swrast`
（以及硬件路径的 `virtio_gpu`）**建 screen 失败**。两者唯一的实质差别是：**`kms_swrast` 需要 KMS winsys，
`swrast` 不需要**。

→ 因此卡点几乎可以确定在**「KMS 路径」**上（而不是驱动加载、依赖、sysfs、modifier）。
剩下的两个候选也正好落在这里：Mesa 是否调用 `drmSetMaster`（本内核与 Linux 一样对 render 节点返回
`-EACCES`，见 `drm.c:798`），或某条 KMS ioctl **成功但数据不对**（注意：从未观测到任何 DRM ioctl
返回错误，所以若在 ioctl 层，形状必然是"成功但内容错"）。这两个都还没验证。

**候选一已排除**：对 Mesa 25.2.7 全源码 grep `drmSetMaster|drmAuthMagic|drmGetMagic|drmDropMaster`，
在 gbm / dri2 / kms-dri 路径上**没有任何 `drmSetMaster` 调用**——只有
`platform_drm.c:403` 的 `drmAuthMagic`（X/DRM 认证）与 `platform_wayland.c:1974` 的 `drmGetMagic`。
所以 render 节点的 `SET_MASTER` 返回 `-EACCES` 不是原因。

**只剩候选二**：kms_swrast 建 screen 时某条 KMS ioctl **成功但返回的数据不对**。
下一步应当在 `kms_swrast` 的 screen 创建路径（`src/gallium/drivers/.../kms_swrast` 与
`drmModeGetResources`/`drmModeGetConnector` 等）上，把内核实际返回的结构体与 Linux 的逐字段对照。

### 9.13 GBM 最终状态：不再当作阻塞项（含两条新观测）

**新观测一：换成 GL 变体设备也没用。** 用
`-display egl-headless -device virtio-gpu-gl-pci`（virgl 能力）启动，QEMU 接受该设备（stderr 为空），
但 `eglinfo -p gbm` **仍然** `eglInitialize failed`。所以"用非 GL 变体所以没有 virgl"这条解释**不成立**。

**新观测二：`kms_swrast` 这个名字在这套 Mesa 构建里可能根本不可用。**

```
libgallium-25.2.7.so 里 "kms_swrast" 出现次数：0
                  而 "swrast" / "virtio_gpu" / "kmsro" / "zink" 均存在
libdril_dri.so 只导出 60 个 __driDriverGetExtensions_<驱动名> 形式的后缀符号，
              没有通用的 __driDriverGetExtensions
```

（注意：前一轮我用 `strings -x` 精确匹配得出的"不存在"结论有方法缺陷，此处是**用 `grep -c` 复核后**的数字。）

**GBM 的累计结论**：失败被钉在 Mesa 的 DRI screen 创建、且**在 KMS 路径上**（`swrast` 能建 screen、
`kms_swrast` 不能，二者唯一实质差别是是否需要 KMS winsys）。已排除的假设累计 10 条：
sysfs 缺节点、全部驱动选择开关、modifier 分支、缺共享库、render 节点 KMS 被整体拒绝、缺 DUMB ioctl、
winsys 创建失败、Mesa 调用 `drmSetMaster`、virgl/QEMU 设备变体、以及"网络不可用"这个前提本身。

**建议（重要）**：**不要再把 GBM 当成阻塞项**。
- 对 Minecraft 无影响——`EGL_PLATFORM=wayland` 已用真实客户端（`es2gears_wayland`，`EGL_VERSION=1.5`
  + GLES 3.2 llvmpipe）验证可渲染（9.11）；
- GBM 只影响 wlroots 自己的渲染器质量，当前 `WLR_RENDERER=pixman` 是**正确且可用**的配置；
- 若将来确实要 GBM 加速，最有效的下一步是用 **strace**（网络已通，依赖可现取）看 DRI screen 创建
  到底停在哪一步——那比继续做假设-验证循环划算得多。

### 9.14 Minecraft 的 JNI 前置条件：已补齐并实测通过（commit `bcd18ec8`）

排查 Minecraft 还剩什么时，找到的**不是又一条假设，而是一个具体且可修的前提**：

- LWJGL 是开源的（BSD），所以直接取 `lwjgl-3.3.6-natives-linux.jar` 检查：
  `liblwjgl.so` 的 `DT_NEEDED` 是 **`libc.so.6` / `libdl.so.2` / `libpthread.so.0`——glibc 的 soname**；
  可选的 classifier 只有 freebsd / linux / linux-arm32 / linux-arm64 / linux-ppc64le / linux-riscv64 /
  macos / macos-arm64 / windows / windows-arm64 / windows-x86——**上游不提供 musl 变体**。
- 而本镜像是**纯 musl**（`ld-musl-x86_64.so.1`），既没有 glibc 也没有 gcompat，缓存里也没有——
  也就是 **JNI 层根本无法 `dlopen` 任何 LWJGL native**。

**修法**：把 `gcompat`（Alpine main，1.1.0-r4）加进 `packages/world/xfce.world` 的 JVM 段。

**验证（重建镜像 + 实机运行，不是假设）**：
- 构建日志：`(158/518) Installing gcompat (1.1.0-r4)`；
- 新镜像内：`/lib/ld-linux-x86-64.so.2`（22728 B）+ `libc.so.6 -> libgcompat.so.0` +
  `libm.so.6` / `libpthread.so.0` / `librt.so.1`；
- 在客体里跑**宿主用 glibc 动态链接编出来的**测试程序：

  ```
  GLIBCTEST: start
  GLIBCTEST: dlopen(libm.so.6)=OK
  GLIBCTEST: dlopen(libpthread)=OK
  GLIBCTEST: end            rc=0
  ```

  glibc 动态二进制能加载、且能继续 `dlopen` 其它 glibc 共享库。

**边界（不要过度解读）**：gcompat 是**部分** glibc 兼容层，所以这只证明"JNI 加载路径通了"这个前置条件成立，
**不等于** LWJGL/游戏已可运行。游戏自身的 jar 与资源是受版权保护的商业内容，**未下载**。

### 9.15 还缺 `libdl.so.2`：用真实 LWJGL native 验证的第二个前提

9.14 加 gcompat 时**还没做完**：清点 gcompat 实际提供的文件后发现，它只给了
`libc.so.6` / `libm.so.6` / `libpthread.so.0` / `librt.so.1` / `libcrypt.so.1` / `libresolv.so.2` /
`libutil.so.1` + `ld-linux-x86-64.so.2`，**唯独没有 `libdl.so.2`**。

而 LWJGL 的 native 依赖里**正好要它**：

```
liblwjgl.so            DT_NEEDED: libdl.so.2, libpthread.so.0, libc.so.6
libglfw.so             DT_NEEDED: librt.so.1, libm.so.6, libdl.so.2, libpthread.so.0, libc.so.6
liblwjgl_opengles.so   DT_NEEDED: libpthread.so.0, libc.so.6
```

**修法**：glibc ≥2.34 起 `libdl` 已并入 libc，所以补一个软链即可——
`packages/overlay/xfce/lib/libdl.so.2 -> libc.so.6`。

**验证（重建镜像 + 注入真实 LWJGL 3.3.6 native 实测，不是推断）**：

```
/lib/libdl.so.2 -> libc.so.6                              （镜像内已落地）
LWJGLT: dlopen(libdl.so.2)            = OK
LWJGLT: dlopen(/usr/lib/liblwjgl.so)          = OK
LWJGLT: dlopen(/usr/lib/liblwjgl_opengles.so) = OK
LWJGLT: dlopen(/usr/lib/libglfw.so)           = OK
rc=0
```

即 **JNI + glibc 依赖这条路上，LWJGL 的三个 native 库都能真正加载**。

**仍存边界**：这验证的是"native 能被加载"（用的是上游真实二进制），**不是**"Java 侧绑定可用"——
客体只有 JRE（无 `javac`），Java 侧的 EGL/GLES 调用没能在此验证；游戏 jar/资源是版权内容，未下载。

**一处流程教训**：这次注入 native 时我第一次用 `find -maxdepth 4` 定位解包产物，而实际路径深度是 7，
`find` 静默返回空、循环里的 `[ -z ] && continue` 把三个 native 全跳过了——日志里表现为
"natives present: No such file"。**"注入成功"没有输出不等于真的写进去了**；后来改用显式路径并加
`injected <dst>` 回显才确认。这类静默跳过值得在脚本里一律显式回显。

### 9.16 OpenAL：把 LWJGL 重定向到发行版的 musl 版（本轮实测）

承接 9.14/9.15 那条「LWJGL 的 native 是 glibc 构建」的线：MC 的死因最后落在
`natives/libopenal.so`（glibc）抛出的**未捕获 `std::system_error`**（稳定抛点
`libopenal.so + 0xf168`，详见 `docs/distro/known-issues.md`）。而镜像里其实**已经有 musl 版的 OpenAL**
（Alpine `openal-soft`：`/usr/lib/libopenal.so.1 -> libopenal.so.1.24.3`）。

**修法（本次已落地）**：给 launcher 的 java 命令行加一行

```
-Dorg.lwjgl.openal.libname=/usr/lib/libopenal.so.1
```

（`packages/overlay/xfce/usr/local/bin/minecraft`）。

**实测结果（三次采样一致）⇒ 结论是：这条改动有害，已回滚**：
- `CXA_THROW` / `terminate called` / `MAP_HIT` **全部消失**，`DLOPEN ... openal` 也没有出现
  ⇒ glibc OpenAL 的抛异常路径**确实被绕开了**；
- **但 MC 反而停得更早**：加了这个 flag 之后三次采样都停在
  `Backend library: LWJGL version 3.3.3+5` 之后，**再也到不了 `Using optional rendering extensions`**；
  而**不加** flag 时，多次采样都能走到 `Using optional rendering extensions`（只是随后被 OpenAL 的抛异常打死）。
- ⇒ **`-Dorg.lwjgl.openal.libname` 把失败点提前了**（很可能是 LWJGL 因此提早去碰 OpenAL，
  而「glibc 语境里 `dlopen` 一个 musl 库」这条路本身会卡）。**这是一次回归，已从 overlay 回滚。**

**修正后的判断**：MC 的 OpenAL 抛异常**不是**渲染阶段的直接阻塞点 —— 不加 flag 时 MC 能走到
`Using optional rendering extensions`，抛异常发生在**那之后**（声音引擎初始化阶段）。所以下一步不是
「换一个 OpenAL」，而应该是：
（a）查清 glibc 版 OpenAL 在 gcompat 下**为什么**抛（稳定抛点 `libopenal.so + 0xf168`），或
（b）让 MC 在声音引擎初始化失败时**优雅降级**（它本该如此），而不是让 glibc native 的 C++ 异常
    逃逸成 `terminate`。

### 9.17 OpenAL 这一环解决了：MC 走到了主菜单（但仍受另一个间歇性停点影响）

9.16 的回滚结论只对了一半：把 OpenAL 指向 musl 版之所以变成回归，是因为**只换了库、没换后端**。
真正起作用的组合是**两件事一起**（已落在 launcher overlay 里）：

```
export ALSOFT_DRIVERS=null                                # 让 OpenAL Soft 用空设备后端
-Dorg.lwjgl.openal.libname=/nonexistent/libopenal.so      # 让 LWJGL 不走它自带的 glibc OpenAL
```

**实测（客体，有一轮完整走通）**：MC 的日志一路到

```
Using optional rendering extensions: ...
Reloading ResourceManager: vanilla
OpenAL initialized on device No Output        ← 声音引擎这次起来了
Sound engine started
Created: 512x256x0 minecraft:textures/atlas/particles.png-atlas
...（blocks / gui / items / chest / shulker_boxes 等整套 atlas 都建了）
[Download-*/ERROR]: Failed to fetch Realms feature flags / yggdrasil public key
```

最后几行网络错误正是**标题界面**会做的事（拉 Realms 通知与用户资料），并且**全程没有任何
`CXA_THROW` / `terminate called`** ⇒ **MC 走到了主菜单** ✓。

**机制**：LWJGL 自带的 `libopenal.so` 是 **glibc 构建**，在这个 musl 客体里经 gcompat 运行，
在声音引擎初始化时抛**未捕获的 C++ 异常**（先看到 `std::system_error`；沿这条线改到走 fallback 后，
看到 OpenAL Soft 自己的 `al::backend_exception`）⇒ `terminate` ⇒ 进程死。
`ALSOFT_DRIVERS=null` 把后端变成空设备（日志里即 `No Output`），于是不再抛。
**两件事缺一不可**：只给 `ALSOFT_DRIVERS=null`（不改 libname）时，MC 仍停在
`Backend library: LWJGL version 3.3.3+5` 之后（实测一轮一致）。

**仍不稳定（必须记下）**：**同一配置换一轮**会停在 `Backend library: LWJGL version 3.3.3+5` 之后、
到不了 `Using optional rendering extensions` —— 与本文件第 8 节记的「渲染器起来之后输出就停」
是**同一现象**，它**独立于 OpenAL**，是另一条间歇性停点（与 mpv/LuaJIT 那类未解释的崩溃同属一类）。

⇒ **结论**：OpenAL 这一环**已解决**，MC 能到达主菜单；要「稳定地能玩」，还需要解决那条间歇性停点。

**顺带更正两条陈旧记录**：
- 本文 8 节末的「LWJGL natives + Minecraft jars 不在镜像里」**已经不成立** —— 现在的 xfce 镜像里
  `/usr/share/a20-media/1.21.11/` 完整存在：`client.jar`(31 MB)、`libraries/`、`assets/`、
  `natives/`（libglfw/libopenal/liblwjgl/libjemalloc/... 齐全）、`classpath.txt`。
- 结合 9.13 的结论（**GBM 不是阻塞项**、Wayland 路径上真实 GL 渲染已跑通），Minecraft 现在的**真正
  卡点**是上面那条「停点」与 glibc-native 类问题，**不是** GEM/dma-buf 那条链。

# Display 与 Framebuffer 驱动

A20OS 的 display 类（`DEV_CLASS_DISPLAY`，`gpu_dev_ops_t`）包含两层：2D 扫描输出与 framebuffer（本文件主题），以及 virtio-gpu 3D (virgl) 透传（[3D 图形加速栈](../../graphics/3d-graphics.md)）。2D 层只承诺扫描输出；3D 层通过 virtio-gpu 的 `SUBMIT_3D` 把 virgl 命令流转发到 host 端 GL 上下文。

virtio-gpu 已按每个 device 独立分配原生 DMA 实例，内嵌 controlq、锁、等待队列和暂存均由该绑定持有；停止失败保留本实例，不影响其他设备。双设备的逻辑注销/重绑和默认 promotion 已有 QEMU 验证。当前 fbdev/DRM devfs 仍是默认聚合入口，内部实例化不等于多 GPU 节点、跨 GPU PRIME 或显示拓扑管理，详见 [每设备实例](../guide/native-runtime.md#virtio-gpu-的每设备实例)。

## 驱动接入

probe 初始化完成并设置 `dev->drv_priv` 后调用：

```c
if (gpu_device_register(dev) < 0)
    goto fail;
```

默认选择保留已注册 display，并使用 ABI 12 显式原生角色：只选择 SCANOUT，优先 RENDER，同级优先非 BOOT_SCANOUT，等分保持；不再从 virgl 表推断能力。remove 必须先停刷新/scanout，再 `gpu_device_unregister(dev)`。当前 default registry 会在注销时尝试重选设备，fbdev 仍按每次操作选默认设备；DRM 在 open 时固定 publication，并分区 GEM/CRTC/EDID/事件，跨 publication PRIME 拒绝。这不等于完整多 GPU 节点或热拔插保护。DRM 持久 backing 要求驱动显式 remove，失败不能回收设备仍可访问的存储，见 [DRM 退休](../guide/native-runtime.md#drm-文件命名空间与失败退休)。新原生 GPU 驱动应先明确每设备实例与渲染/显示资源的所有权，不能把 virgl 回调视为通用硬件提交接口。异步任务需在 quiesce 中停止并结束，再在 remove 中释放存储；见 [原生驱动运行时](../guide/native-runtime.md)。

以下是旧 get_fb 接入的简化骨架；实际驱动必须补齐部分初始化回滚及可失败硬件停止，不能照此假定 stop 一定成功。RAM framebuffer 应采用下述 owning scanout：

```c
/* 1. 注册驱动 */
static driver_t my_gpu_driver = {
    .name       = "my-gpu",
    .bus        = &pci_bus,
    .id_table   = my_ids,
    .probe      = my_gpu_probe,
    .remove     = my_gpu_remove,
    .class_type = DEV_CLASS_DISPLAY,
    .class_ops  = &my_gpu_ops,
};
DRIVER_REGISTER(my_gpu_driver);

/* 2. probe：初始化硬件、分配 framebuffer、注册 display */
static int my_gpu_probe(device_t *dev){
    my_gpu_t *g = kzalloc(sizeof(*g));
    if (!g) return -ENOMEM;

    dev->drv_priv = g;

    if (init_pci_bar(g, dev) < 0) goto fail;
    if (alloc_fb(g, WIDTH, HEIGHT, 32) < 0) goto fail;
    if (first_set_scanout(g) < 0) goto fail;

    /* 注册是最后一步 */
    if (gpu_device_register(dev) < 0) goto fail_fb;

    return 0;

fail_fb:
    free_fb(g);
fail:
    kfree(g);
    return -ENODEV;
}

/* 3. 使用：用户 mmap 后写像素 */
static int my_gpu_get_info(device_t *dev, uint32_t *w, uint32_t *h, uint32_t *bpp){
    my_gpu_t *g = dev->drv_priv;
    *w   = g->width;
    *h   = g->height;
    *bpp = g->bpp;
    return 0;
}

static int my_gpu_get_fb(device_t *dev, uintptr_t *paddr, size_t *size){
    my_gpu_t *g = dev->drv_priv;
    *paddr = g->fb_paddr;
    *size  = g->fb_size;
    return 0;
}

static int my_gpu_flush(device_t *dev, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height)
{
    my_gpu_t *g = dev->drv_priv;
    if (x >= g->width || y >= g->height)
        return -EINVAL;
    if (width > g->width - x)  width = g->width - x;
    if (height > g->height - y) height = g->height - y;
    /* ... 提交设备刷新命令 ... */
    return 0;
}

/* 4. remove：下列两步为设备/平台相关协议，失败必须保留实例。
 * retire_fb_views 是骨架占位，当前 raw fbdev 没有通用撤销 helper；
 * 尚不能撤销时不能 free_fb。RAM VMO 路径改为只 put producer 引用。 */
static int my_gpu_remove(device_t *dev){
    my_gpu_t *g = dev->drv_priv;
    int ret = stop_scanout(g);
    if (ret) return ret;
    ret = retire_fb_views(g);
    if (ret) return ret;
    gpu_device_unregister(dev);
    free_fb(g);
    dev->drv_priv = NULL;
    kfree(g);
    return 0;
}
```

## 原生能力声明

probe 建立实例后、`gpu_device_register` 前，必须准备纯缓存 `get_capabilities`。以下是上述原生 2D 骨架的能力部分；设备状态与各操作定义仍由实际驱动提供：

```c
static int my_gpu_get_capabilities(device_t *dev, struct gpu_capabilities *out)
{
    if (!out) return -EINVAL;
    *out = (struct gpu_capabilities){0};
    if (!dev || !dev->drv_priv) return -ENODEV;
    out->flags = GPU_CAP_SCANOUT;
    return 0;
}
static const gpu_dev_ops_t my_gpu_ops = {
    .get_info = my_gpu_get_info,
    .get_fb = my_gpu_get_fb,
    .flush = my_gpu_flush,
    .get_capabilities = my_gpu_get_capabilities,
};
```

固件保留输出另声明 BOOT_SCANOUT；有 owning RAM get_scanout 才声明 RAM_SCANOUT；仅在渲染后端已建立时声明 RENDER，不能据硬件型号或函数入口推断。headless RENDER 可以发布而不占默认输出。consumer 在已有 access 内调用 `gpu_device_capabilities_get`；标志不替代实际操作校验、VMO 引用或 DMA 权限。四个现有 provider 已接入，当前模块 ABI 为 12，ABI 11 的旧包必须重建；规则见 [原生能力](../guide/native-runtime.md#原生-gpu-能力与默认选择)。

## 类操作

`get_info` 返回当前可见宽、高和 bpp，输出指针无效返回 `-EINVAL`。`get_fb` 只返回借用的 CPU 物理地址和可见 backing 长度，不是完整 VRAM aperture，也不保留存储。只有映射层需要物理地址，驱动的寄存器访问继续使用内核虚拟 MMIO 地址。

`flush(x, y, width, height)` 提交矩形。当前实现把零宽或零高解释为全屏。先验证起点，再用减法裁剪，避免 `x + width` 溢出：

```c
if (x >= fb_width || y >= fb_height)
    return -EINVAL;
if (width > fb_width - x)
    width = fb_width - x;
```

如果设备从普通 RAM backing 读取像素，提交命令前对覆盖区域执行 DMA for-device 同步。stride 可能大于 `width * bytes_per_pixel`，同步长度和用户 ABI 的 `line_length` 都必须使用实际 pitch。

## RAM scanout 的原生所有权

ABI 11 在 GPU vtable 末尾增加可选 `get_scanout`。RAM producer 用 `vmo_create_contiguous()` 创建 backing，记录一份 owner；在有效 class 调用内与模式替换协调，vmo_ref 后返回 `struct gpu_scanout {memory, offset, size}`。extent 和 slot 固定，像素内容可写；offset 页对齐、可见 size 为正，整页覆盖必须落在 backing 内。producer 退出只交还自己的 VMO 引用，不能原阶 pfa_free。只有确认硬件不再访问后才允许释放其硬件 owner。

消费者用 `gpu_device_scanout_get(&access, &snapshot)`，成功后可结束 class access；VMA 或调用者保留 RAM，最后 `gpu_scanout_put()`。helper 验证类型/范围并清理错误输出；回调即使失败，也交出非空 memory 的一份引用。缺少回调才选旧物理分支，已有回调失败则返回错误。最终 put 只依赖内核 MM，不调用已解绑模块。该接口未覆盖 VRAM、每文件设备身份或硬件停止确认，完整错误与锁契约见 [连续 RAM VMO](../guide/native-runtime.md#连续-ram-vmo-与-owning-scanout)。

## `/dev/fb0` ABI

声明位于 `kernel/include/drivers/gpu/framebuffer.h`：

| ioctl | 行为 |
|---|---|
| `FBIOGET_VSCREENINFO` | 返回完整 Linux `fb_var_screeninfo`（含 `xres_virtual`、`xoffset`、RGB bitfield）|
| `FBIOGET_FSCREENINFO` | 返回完整 Linux `fb_fix_screeninfo`（`smem_start`、`smem_len`、`type=FB_TYPE_PACKED_PIXELS`、`visual=FB_VISUAL_TRUECOLOR`、`line_length`）|
| `FBIOPUT_VSCREENINFO` | 不支持模式设置；请求与当前模式一致时按 no-op 成功返回，否则 `-EINVAL` |
| `FBIO_MAP_FB` | 把 framebuffer 映射到参数指定的用户虚拟地址（A20OS 专有）|
| `FBIO_FLUSH` | 当前提交全屏刷新（A20OS 专有）|

`/dev/fb0` 同时支持标准 `mmap(2)`。MMU 上有 get_scanout 的 RAM 路径由普通 `VM_VMO|VM_SHARED|VM_NATIVE_CAP` VMA 保留 VMO，采用 Normal RAM 属性、支持 fork/split，mprotect 不可超过创建时有效硬件权限。偏移页对齐，覆盖不超过可见长度的页取整范围，末页 padding 已清零。旧 get_fb 分支通过共享 MM/Rust 物理事务安装 `VM_PFNMAP|VM_SHARED|VM_DONTFORK`；CPU 缓存类型按 boot framebuffer 别名策略核准，不能统一假设为 Device 或 Normal。hint 检查整个区间，fixed/NOREPLACE 与回滚复用原生 MM。

FBIO_MAP_FB 同样优先 owning RAM 路径，映射完整 scanout 页覆盖并使用固定 NOREPLACE；旧 get_fb 分支继续借用物理存储。用户地址须非零、页对齐且整段不超过 USER_VA_LIMIT。NOMMU 不建 VMA，保留 fixed info 的直接地址兼容行为。物理地址暴露是现有兼容 ABI，不应复制到其他类。

## VirtIO GPU 范例

`virtio_gpu.c` 支持 MMIO/PCI transport。probe 顺序为 modern feature negotiation、controlq、分配连续 framebuffer、`RESOURCE_CREATE_2D`、`ATTACH_BACKING`、`SET_SCANOUT`、首次 flush、注册 display。flush 依次执行 cache clean、`TRANSFER_TO_HOST_2D`、`RESOURCE_FLUSH`。framebuffer 已用连续 VMO 工厂创建，remove 复位 transport、注销 display 并交还 producer 的 VMO 引用；尚存 VMA/snapshot 独立保留 RAM。初始化和退出已通过有界 status=0 读回确认 reset，失败初始化走原生可失败 remove，超时保留绑定、IRQ 和内部 DMA 存储；四条命令路径超时或异常 completion 后封闭共享暂存区，成功 reset 前不复用。当前单静态 controlq 以 owner 守卫拒绝第二设备覆盖，尚未实现多实例；旧 DRM 裸调用仍须外层生命周期协调。详见 [VirtIO reset](../guide/native-runtime.md#virtio-reset-确认与-gpu-退出)。

当前模式固定 1024x768x32，controlq 使用实例 mutex 串行化同步命令；请求和响应先复制到实例内稳定 DMA staging，等待期间保持中断开启。不得把调用者栈对象直接写入 descriptor，也不得用 `spin_lock_irqsave` 包围设备完成等待。

动态模式设置必须先实现 display info 查询和 prepare/commit/rollback，不能只部分实现 `FBIOPUT_VSCREENINFO`。

### VirtIO GPU 3D (virgl)

当 QEMU 以 `virtio-gpu-gl-*` 设备（带 `-display egl-headless` 等 OpenGL 后端）启动时，驱动协商 `VIRTIO_GPU_F_VIRGL`，并可通过 `DRM_IOCTL_VIRTGPU_*` 把 virgl 命令流转发给 host 端 GL 上下文（`CTX_CREATE`/`RESOURCE_CREATE_3D`/`SUBMIT_3D`）。接口、实现与用户态对接见 [3D 图形加速栈](../../graphics/3d-graphics.md)。2D-only 设备自动回退，`gpu3d_test` 读 `VIRTGPU_PARAM_3D_FEATURES` 发现未协商到 virgl 时以退出码 77 报告 `SKIP 2D-only device`。

## VMSVGA/SVGAv3 范例

`vmsvga.c` 匹配 VMware `15ad:0405` 与 VirtualBox ARM SVGAv3 `15ad:0406`。PCI BAR 必须按物理 BAR 号取：BAR0 寄存器，BAR2（部分设备退回 BAR1）VRAM。SVGAv3 从设备读取 `FB_OFFSET`、pitch 和 `FB_SIZE`，只把可见 scanout 暴露给用户态，不能把整段 VRAM 当 framebuffer。

SVGAv3 先启动 device command context，再设置模式；flush 提交 `SVGA3_CMD_UPDATE`。VRAM 在 VirtualBox ARM 映射为 Device memory，因此不要对该 MMIO aperture 使用普通 RAM DMA cache API。remove 关闭 display enable、注销默认设备并清实例。

## 新 display 驱动检查

- 验证 format、bpp、pitch、offset 和 `pitch * height` 的溢出及 aperture 边界。
- 不假定物理 framebuffer 从 BAR 起点开始。
- probe 的自检不得破坏后续用户画面，且失败时撤销类注册。
- `get_fb` 返回的长度必须恰好覆盖用户可写 scanout。
- flush 序列必须符合设备 cache/doorbell 协议。
- 显存映射属性不能与内核已有别名冲突。

不要把整个 PCI BAR 或整段 VRAM 当成 framebuffer 长度返回。`get_fb` 的长度必须只覆盖可见 scanout，否则用户态会映射到未定义或受保护的设备内存。

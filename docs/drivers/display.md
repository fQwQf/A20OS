# Display 与 Framebuffer 驱动

A20OS 当前 display 类只承诺 2D 扫描输出和 framebuffer，不承诺 3D render context、命令验证或 GPU 内存管理。类编号为 `DEV_CLASS_DISPLAY`，操作类型为 `gpu_dev_ops_t`。

## 驱动接入

probe 初始化完成并设置 `dev->drv_priv` 后调用：

```c
if (gpu_device_register(dev) < 0)
    goto fail;
```

第一个成功注册的 display 成为 `/dev/fb0` 默认设备。remove 必须先停刷新/scanout，再 `gpu_device_unregister(dev)`。当前 default registry 不会自动选择第二个设备接替；这是热拔插限制，不应在新驱动里再建私有全局 getter。

典型生命周期：

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
static int my_gpu_probe(device_t *dev)
{
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
static int my_gpu_get_info(device_t *dev, uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    my_gpu_t *g = dev->drv_priv;
    *w   = g->width;
    *h   = g->height;
    *bpp = g->bpp;
    return 0;
}

static int my_gpu_get_fb(device_t *dev, uintptr_t *paddr, size_t *size)
{
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

/* 4. remove：先注销，再释放资源 */
static int my_gpu_remove(device_t *dev)
{
    my_gpu_t *g = dev->drv_priv;
    gpu_device_unregister(dev);
    stop_scanout(g);
    free_fb(g);
    kfree(g);
    return 0;
}
```

## 类操作

`get_info` 返回当前可见宽、高和 bpp，输出指针无效返回 `-EINVAL`。`get_fb` 返回 framebuffer 的物理地址和可见 backing 长度，不是完整 VRAM aperture。只有映射层需要物理地址，驱动的寄存器访问继续使用内核虚拟 MMIO 地址。

`flush(x, y, width, height)` 提交矩形。当前实现把零宽或零高解释为全屏。先验证起点，再用减法裁剪，避免 `x + width` 溢出：

```c
if (x >= fb_width || y >= fb_height)
    return -EINVAL;
if (width > fb_width - x)
    width = fb_width - x;
```

如果设备从普通 RAM backing 读取像素，提交命令前对覆盖区域执行 DMA for-device 同步。stride 可能大于 `width * bytes_per_pixel`，同步长度和用户 ABI 的 `line_length` 都必须使用实际 pitch。

## `/dev/fb0` ABI

声明位于 `kernel/include/drivers/gpu/framebuffer.h`：

| ioctl | 行为 |
|---|---|
| `FBIOGET_VSCREENINFO` | 返回 `xres/yres/bits_per_pixel` |
| `FBIOGET_FSCREENINFO` | 返回物理地址、长度和 `line_length` |
| `FBIO_MAP_FB` | 把 framebuffer 映射到参数指定的用户虚拟地址 |
| `FBIO_FLUSH` | 当前提交全屏刷新 |
| `FBIOPUT_VSCREENINFO` | 常量保留，当前未实现 |

MMU 映射要求用户地址页对齐、范围不溢出 `USER_VA_LIMIT`、不与已有 VMA/PTE 重叠；失败撤销已映射页。NOMMU 不建页表，应用从 fixed info 使用物理地址。物理地址暴露是现有兼容 ABI，不应复制到其他类。

## VirtIO GPU 范例

`virtio_gpu.c` 支持 MMIO/PCI transport。probe 顺序为 modern feature negotiation、controlq、分配连续 framebuffer、`RESOURCE_CREATE_2D`、`ATTACH_BACKING`、`SET_SCANOUT`、首次 flush、注册 display。flush 依次执行 cache clean、`TRANSFER_TO_HOST_2D`、`RESOURCE_FLUSH`。remove 复位 transport、注销 display、释放 framebuffer 页。

当前模式固定 1024x768x32，controlq 使用实例 mutex 串行化同步命令；请求和响应先复制到实例内稳定 DMA staging，等待期间保持中断开启。不得把调用者栈对象直接写入 descriptor，也不得用 `spin_lock_irqsave` 包围设备完成等待。

动态模式设置必须先实现 display info 查询和 prepare/commit/rollback，不能只部分实现 `FBIOPUT_VSCREENINFO`。

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

 注意：不要把整个 PCI BAR 或整段 VRAM 当成 framebuffer 长度返回。`get_fb` 的长度必须只覆盖可见 scanout，否则用户态会映射到未定义或受保护的设备内存。

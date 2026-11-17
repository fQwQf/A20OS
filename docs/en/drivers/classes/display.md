# Display and Framebuffer Drivers

The display class in A20OS (`DEV_CLASS_DISPLAY`, `gpu_dev_ops_t`) consists of two layers: 2D scan output and framebuffer (the focus of this document), as well as virtio-gpu 3D (virgl) passthrough ([3D Graphics Acceleration Stack](../../graphics/3d-graphics.md)). The 2D layer only handles scan output; the 3D layer forwards virgl commands to the host-side GL context via virtio-gpu’s `SUBMIT_3D` function.

## Driver Integration

This is called after the probe initialization is complete and `dev->drv_priv` has been set:

```c
if (gpu_device_register(dev) < 0)
    goto fail;
```

The first successfully registered display becomes the default device, `/dev/fb0`. Before using `gpu_device_unregister(dev)`, it is necessary to stop the refresh/scanout process first. The current default registry does not automatically select a second device to take over; this is a limitation due to hot-plugging and should not be circumvented by implementing custom global getters in new drivers.

Typical sequence for registration, probing, usage, and removal:

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

/* 4. remove：先注销，再释放资源 */
static int my_gpu_remove(device_t *dev){
    my_gpu_t *g = dev->drv_priv;
    gpu_device_unregister(dev);
    stop_scanout(g);
    free_fb(g);
    kfree(g);
    return 0;
}
```

## Class Operations

`get_info` returns the currently visible width, height, and bpp. If the output pointer is invalid, `-EINVAL` is returned. `get_fb` returns the physical address of the framebuffer and the visible backing length, which is not the entire VRAM aperture. Only the mapping layer requires the physical address; driver register accesses continue to use the kernel’s virtual MMIO addresses.

`flush(x, y, width, height)` submits a rectangle to the display. The current implementation interprets zero width or zero height as full screen. The starting point is first verified, and then cropping is performed using subtraction to prevent `x + width` from exceeding the boundaries.

```c
if (x >= fb_width || y >= fb_height)
    return -EINVAL;
if (width > fb_width - x)
    width = fb_width - x;
```

If the device reads pixels from regular RAM backing, perform DMA for-device synchronization on the overlapping area before submitting the command. The stride may be greater than `width * bytes_per_pixel`; both the synchronization length and the `line_length` specified by the user ABI must use the actual pitch value.

## `/dev/fb0` ABI

The declarations are located in `kernel/include/drivers/gpu/framebuffer.h`:

| ioctl | Behavior |
|---|---|
| `FBIOGET_VSCREENINFO` | Returns the complete Linux `fb_var_screeninfo` (including `xres_virtual`, `xoffset`, RGB bitfield) |
| `FBIOGET_FSCREENINFO` | Returns the complete Linux `fb_fix_screeninfo` (`smem_start`, `smem_len`, `type=FB_TYPE_PACKED_PIXELS`, `visual=FB_VISUAL_TRUECOLOR`, `line_length`) |
| `FBIOPUT_VSCREENINFO` | Does not support mode setting; returns a no-op if the request matches the current mode, otherwise returns `-EINVAL` |
| `FBIO_MAP_FB` | Maps the framebuffer to the user virtual address specified by the parameter (A20OS-specific) |
| `FBIO_FLUSH` | Flushes the current full-screen display (A20OS-specific) |

The `/dev/fb0` device supports both the standard `mmap(2)` mechanism (implemented in `kernel/drivers/gpu/framebuffer.c` via `fbdev_linux_mmap`) and additional mapping methods: The offset of the mapped page is aligned, and the range does not exceed `smem_len`. When the address is 0, `mm_find_gap` is used to select an appropriate location for mapping. The VMA (Virtual Memory Area) is marked with `VM_PFNMAP|VM_SHARED|VM_DONTFORK`. The PTE (Page Table Entry) does not contain the `PTE_MAT1` bit, which preserves the device memory attributes on AArch64. As a result, standard fbdev userland implementations (such as Xfbdev and Weston fbdev backends) can map the scanout area without any modifications.

The FBIO_MAP_FB mapping requirement is that the user-addressed page is aligned, the range does not exceed `USER_VA_LIMIT`, and there is no overlap with existing VMA/PTE entries; if a mapping fails, the previously mapped pages are revoked. In NOMMU (No Memory Management Unit) mode, no page tables are created, and the application uses the physical address directly from the fixed information. Exposing the physical address is part of the existing compatible ABI and should not be copied to other contexts.

## VirtIO GPU Example

The `virtio_gpu.c` module supports MMIO (Memory-Mapped I/O) and PCI transports. The probe sequence includes modern feature negotiation, control queue management, allocation of a continuous framebuffer, calling `RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `SET_SCANOUT`, performing the initial flush operation, and registering the display. The flush process involves clearing the cache, executing `TRANSFER_TO_HOST_2D`, and finally calling `RESOURCE_FLUSH`. The remove operation resets the transport, deregisters the display, and frees the allocated framebuffer pages.

The current mode is fixed at 1024x768x32. ControlQ uses instance mutexes to serialize synchronization commands; requests and responses are first copied to the instance’s stable DMA staging area. During the waiting period, interrupts should remain enabled. The caller’s stack objects must not be written directly to the descriptor, nor should the device wait be handled by using `spin_lock_irqsave`.

For dynamic mode settings, it is necessary to first implement the display information query functions, as well as the prepare/commit/rollback processes. It is not sufficient to only partially implement the `FBIOPUT_VSCREENINFO` function.

### VirtIO GPU 3D (virgl)

When QEMU is started with the `virtio-gpu-gl-*` device (along with an OpenGL backend such as `-display egl-headless`), the driver negotiates the `VIRTIO_GPU_F_VIRGL` protocol. This allows virgl commands to be forwarded to the host's GL context via `DRM_IOCTL_VIRTGPU_*` functions (e.g., `CTX_CREATE`, `RESOURCE_CREATE_3D`, `SUBMIT_3D`).The interface, implementation, and user-space integration are covered in the [3D Graphics Acceleration Stack](../../graphics/3d-graphics.md).Devices that are only compatible with 2D mode automatically revert to their default settings. The `gpu3d_test` script checks the `VIRTGPU_PARAM_3D_FEATURES` value; if it determines that the virgl feature has not been negotiated, it reports an exit code of 77 with the message “`SKIP 2D-only device`”.

## VMSVGA/SVGAv3 Example

The `vmsvga.c` file is compatible with both VMware `15ad:0405` and VirtualBox ARM SVGAv3 `15ad:0406`. The PCI BAR numbers must be mapped according to the physical BAR positions: BAR0 for the register, and BAR2 (in some devices, BAR1 is used instead) for the VRAM. SVGAv3 reads the `FB_OFFSET`, pitch, and `FB_SIZE` from the device, and only exposes the visible portion of the scanout to the user space; it is not possible to use the entire VRAM as a framebuffer.

SVGAv3 first initializes the device command context and then sets the mode; the `SVGA3_CMD_UPDATE` command is used to perform updates. In VirtualBox ARM, the VRAM is mapped as device memory, so do not use the regular RAM DMA cache APIs for this MMIO region. The remove command disables the display, unregisters the default device, and clears the instance.

## New display driver checks

- Verify for overflow in the format, bpp, pitch, offset, and `pitch * height`, as well as the aperture boundaries.
- Do not assume that the physical framebuffer starts at the beginning of the BAR (Buffer Address Range).
- The self-check of the probe must not damage subsequent user frames; in case of failure, cancel the class registration.
- The length returned by `get_fb` must exactly cover the user-writable scanout area.
- The flush sequence must comply with the device’s cache/doorbell protocol.
- The video memory mapping attributes must not conflict with any existing aliases in the kernel.

Do not return the entire PCI BAR or a entire segment of VRAM as the framebuffer length. The length returned by `get_fb` must only cover the visible scanout area; otherwise, the user space may map to undefined or protected device memory.

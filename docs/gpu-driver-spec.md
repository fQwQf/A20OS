# A20OS GPU 驱动规范 (GPU Driver Specification)

## 1. 概述

A20OS 中的 GPU 驱动层级负责为用户态提供图形显示功能。由于内核设计的极简原则，GPU 驱动并不包含复杂的 3D 渲染和命令流解析（由用户态库完成，如 LVGL 或 OpenGL），而是主要提供帧缓冲（Framebuffer）内存的分配、映射，以及硬件特有的屏幕刷新（Flush/Commit）机制。

GPU 驱动设备属于 `CLASS_GPU` (class type = 5)。

## 2. 核心架构与职责

GPU 驱动的主要职责包括：
1. **设备发现与初始化**：通过 `virtio-mmio`、`pci` 或 `platform` 总线枚举设备并探测 (Probe)。
2. **显存管理**：分配或映射用于 Framebuffer 的物理内存连续或非连续块（依赖硬件支持）。
3. **接口暴露**：向 DevFS 注册一个主次设备节点（如 `/dev/fb0`），暴露通用 IOCTL 接口。
4. **渲染提交**：对于类似 VirtIO GPU 这种并非直接内存映射显示的硬件，需要处理资源 Flush 请求，将本地显存数据传递给宿主机或显示控制器。

## 3. 标准化接口 (Framebuffer API)

所有的 GPU 驱动都需要注册设备到 `devfs`，并且在 `vfile_ops_t` 虚表中实现对以下 IOCTL 的支持：

- `FBIOGET_VSCREENINFO` (0x4600): 获取当前屏幕分辨率、色深等可变信息。
  - 数据结构：`struct fb_var_screeninfo`
- `FBIOPUT_VSCREENINFO` (0x4601): 设置屏幕分辨率、色深等信息（如果驱动支持动态分辨率）。
- `FBIOGET_FSCREENINFO` (0x4602): 获取显存物理基址、显存大小等固定信息。
  - 数据结构：`struct fb_fix_screeninfo`
- `FBIO_MAP_FB` (0x4603): 【A20OS 专有】将显存映射到当前进程的指定虚拟地址 (通过 arg 传递)。
- `FBIO_FLUSH` (0x4604): 【A20OS 专有】通知 GPU 驱动需要刷新 Framebuffer 的部分或全部区域到屏幕。

### 数据结构定义 (`framebuffer.h`)

```c
struct fb_var_screeninfo {
    uint32_t xres;           /* 可见分辨率 宽 */
    uint32_t yres;           /* 可见分辨率 高 */
    uint32_t bits_per_pixel; /* 色深，通常为 32 */
};

struct fb_fix_screeninfo {
    char id[16];             /* 识别符，如 "virtio-gpu" */
    unsigned long smem_start;/* 显存物理起始地址 */
    uint32_t smem_len;       /* 显存总长度 (字节) */
    uint32_t line_length;    /* 每行像素占用的字节数 (Pitch) */
};
```

## 4. 驱动实现指南

### 4.1 硬件探针 (`probe`)
在探测函数中，驱动需要分配内部结构，设置设备为 `CLASS_GPU`，并完成与具体硬件设备的握手。随后将自身注册至 `devfs` 体系。
如 VirtIO GPU，需获取 MMIO 资源，握手协商特性，并请求 Host 分配一个 2D 扫描输出缓冲区 (Scanout buffer)。

### 4.2 显存映射
对于 `FBIO_MAP_FB`，GPU 驱动应当调用底层虚拟内存管理子系统（如 `vmar_map` 机制），将 `smem_start` 对应的物理内存映射给进程。

### 4.3 图像刷新
对于直接扫描输出 (Scanout) 的物理硬件显示器，写入显存即立刻生效，此时 `FBIO_FLUSH` 可以是一个空操作 (No-op) 或者作为 VSync 等待机制。
对于 VirtIO GPU，`FBIO_FLUSH` 是必须的。用户态库调用此 IOCTL 时，驱动应构造 `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` 和 `VIRTIO_GPU_CMD_RESOURCE_FLUSH` 命令发送至 Virtqueue，通知宿主机 QEMU 刷新图像。

## 5. 用户态调用范例 (LVGL 对接)

LVGL 在其 `disp_flush_cb` 函数中应当通过以下顺序与内核交互：
1. `open("/dev/fb0")`
2. 使用 `ioctl(FBIOGET_VSCREENINFO)` 获知分辨率，据此分配绘制缓冲区。
3. 使用 `ioctl(FBIO_MAP_FB)` 请求内核映射 Framebuffer 至用户态地址空间。
4. 将绘制结果（或部分无效区域）拷贝至映射的地址空间。
5. 使用 `ioctl(FBIO_FLUSH)` 通知内核提交渲染。

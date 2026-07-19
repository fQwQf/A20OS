# 设备类接口

设备类把硬件细节和内核使用者隔开。子系统只看 `class_ops`，不看具体硬件。权威声明在 `kernel/drivers/core/driver_class.h`。

驱动在 `driver_t.class_type` 里选择类，把完全匹配的 ops 指针放进 `class_ops`。类操作接收内核地址，不接收用户指针。想让用户态访问设备，还要走 devfs 固定节点适配器，详见 [用户接口与 devfs](userspace-and-devfs.md)。

## 通用调用规则

每个入口必须检查 `dev`、`dev->drv_priv`、buffer 和长度。可能并发的入口必须在实例内同步。可选操作留 `NULL`；不要“返回成功但什么也不做”来假装支持。未知 ioctl 返回 `-ENOTTY`，硬件不支持的已知能力返回 `-EOPNOTSUPP`。

`device_find_by_class(class, index)` 按注册顺序返回已绑定设备。调用者遇到 `NULL` 就停止枚举。设备类枚举只在设备生命周期内使用；调用者不能缓存裸 `device_t *` 跨越 remove。

## Block：`DEV_CLASS_BLOCK`

```c
typedef struct block_dev_ops {
    int (*read)(device_t *, uint64_t lba, void *buf, size_t sectors);
    int (*write)(device_t *, uint64_t lba, const void *buf, size_t sectors);
    int (*flush)(device_t *);
    int (*ioctl)(device_t *, unsigned long req, void *arg);
    uint64_t (*capacity)(device_t *);
    uint32_t (*sector_size)(device_t *);
} block_dev_ops_t;
```

- `lba` 和 `sectors` 的单位是逻辑扇区；`capacity()` 返回总扇区数，`sector_size()` 返回每扇区字节数。
- `read/write` 当前约定整次成功返回 `0`，失败返回负 errno。必须拒绝 `lba >= capacity`、`sectors > capacity - lba`、空 buffer 和算术溢出。
- 不允许静默短 I/O。硬件有单命令上限时，驱动内部拆分请求；任一子请求失败则返回错误。
- `flush` 在所有此前成功写入已达到持久介质后返回 `0`。仅在硬件无 volatile cache 或保证写穿时才可 no-op。
- 文件系统适配器可能把块类转接到 `block_dev_t` 请求对象；这不改变驱动的类接口。新 block 驱动必须实现 `block_dev_ops_t`，不得把 `block_dev_t` getter 作为设备发现或驱动注册接口。

参考实现：`virtio_blk.c`（并发 VirtIO）、`virtio_scsi.c`（VirtualBox ARM）、`ahci.c`（VirtualBox x86_64）。

典型生命周期：

```c
/* 1. 注册：启动段把驱动挂到总线 */
static driver_t my_blk_driver = {
    .name       = "my-blk",
    .bus        = &pci_bus,
    .id_table   = my_ids,
    .probe      = my_blk_probe,
    .remove     = my_blk_remove,
    .class_type = DEV_CLASS_BLOCK,
    .class_ops  = &my_blk_ops,
};
DRIVER_REGISTER(my_blk_driver);

/* 2. probe：绑定设备并初始化 */
static int my_blk_probe(device_t *dev)
{
    my_blk_t *b = kzalloc(sizeof(*b));
    if (!b) return -ENOMEM;

    dev->drv_priv = b;
    if (init_hardware(b) < 0) {
        kfree(b);
        return -EIO;
    }
    return 0;
}

/* 3. open/use：文件系统通过 class_ops 发 I/O */
static int my_blk_read(device_t *dev, uint64_t lba,
                       void *buf, size_t sectors)
{
    my_blk_t *b = dev->drv_priv;
    if (lba >= b->capacity || sectors > b->capacity - lba)
        return -EINVAL;
    /* ... 整次完成 ... */
    return 0;
}

/* 4. close 由 VFS 层管理；remove 释放硬件 */
static int my_blk_remove(device_t *dev)
{
    my_blk_t *b = dev->drv_priv;
    stop_queue(b);
    kfree(b);
    return 0;
}
```

⚠️ 注意：不要把 `0` 当成“部分成功”。文件系统会把 `0` 理解为整次 I/O 完成，静默短 I/O 会直接破坏文件系统一致性。

## Network：`DEV_CLASS_NET`

```c
typedef struct net_dev_ops {
    int (*open)(device_t *);
    int (*stop)(device_t *);
    int (*send)(device_t *, const void *pkt, size_t len);
    int (*recv)(device_t *, void *buf, size_t maxlen);
    const uint8_t *(*mac)(device_t *);
    void (*poll)(device_t *);
    int (*ioctl)(device_t *, unsigned long req, void *arg);
} net_dev_ops_t;
```

- lwIP 注册至少要求 `send`、`recv`、`mac`；当前 MTU 固定为 1500。
- `send` 成功返回提交的字节数，队列暂满返回 `-EAGAIN`，参数错误返回 `-EINVAL`。
- `recv` 返回一帧字节数；当前 lwIP 适配约定无包返回 `0`，负值表示错误。不得把一帧拆成多次返回；buffer 太小应丢弃或返回 `-EMSGSIZE`，策略必须写在驱动注释中。
- `mac` 返回六字节地址，指针至少在设备仍绑定期间有效。
- `poll` 不得睡眠或分配，可回收 TX、推进 RX。它可能在 `g_lwip_lock` 下调用，锁顺序必须是 `g_lwip_lock -> device lock`，驱动锁下不得回调 lwIP。
- `open/stop` 由网络适配器在需要时调用；probe 必须完成硬件可用性和队列准备，remove 必须停止数据面。驱动不得假设 open 一定会在 probe 后立即发生。

参考实现：`virtio_net.c`、`e1000.c`。

典型生命周期：

```c
/* 1. 注册 */
static driver_t my_net_driver = {
    .name       = "my-net",
    .bus        = &pci_bus,
    .id_table   = my_ids,
    .probe      = my_net_probe,
    .remove     = my_net_remove,
    .class_type = DEV_CLASS_NET,
    .class_ops  = &my_net_ops,
};
DRIVER_REGISTER(my_net_driver);

/* 2. probe：初始化硬件和队列 */
static int my_net_probe(device_t *dev)
{
    my_net_t *n = kzalloc(sizeof(*n));
    if (!n) return -ENOMEM;

    dev->drv_priv = n;
    if (init_rx_tx_rings(n) < 0) {
        kfree(n);
        return -EIO;
    }
    return 0;
}

/* 3. open：lwIP 启动网卡时调用 */
static int my_net_open(device_t *dev)
{
    my_net_t *n = dev->drv_priv;
    enable_rx(n);
    return 0;
}

/* 4. send/recv/poll：数据面 */
static int my_net_send(device_t *dev, const void *pkt, size_t len)
{
    my_net_t *n = dev->drv_priv;
    if (len > 1500) return -EINVAL;
    if (!tx_slot_free(n)) return -EAGAIN;
    return post_packet(n, pkt, len);
}

/* 5. stop/remove：停数据面并释放 */
static int my_net_stop(device_t *dev)
{
    my_net_t *n = dev->drv_priv;
    disable_rx_tx(n);
    return 0;
}

static int my_net_remove(device_t *dev)
{
    my_net_t *n = dev->drv_priv;
    my_net_stop(dev);
    free_rings(n);
    kfree(n);
    return 0;
}
```

❌ 不要这样做：在 `poll` 里分配 mbuf 或尝试获取可能睡眠的锁。`poll` 运行在 lwIP 锁下，一旦睡眠或重入 lwIP 会造成死锁。

## Character：`DEV_CLASS_CHAR`

```c
typedef struct char_dev_ops {
    int (*read)(device_t *, void *buf, size_t count);
    int (*write)(device_t *, const void *buf, size_t count);
    int (*ioctl)(device_t *, unsigned long req, void *arg);
    int (*poll)(device_t *, short events);
} char_dev_ops_t;
```

`read/write` 成功返回字节数。非阻塞无进展返回 `-EAGAIN`。`poll` 只观察 readiness，不得消费数据或睡眠。UART 硬件驱动只负责收发和中断，通用行规、前台进程组和 termios 应由 tty/devfs 层承担。

当前 devfs 没有通用动态 char 节点注册 API，因此实现 class ops 不会自动生成 `/dev/<name>`。需要新节点时必须先扩展通用 devfs registry，不能在具体驱动里直接拼 VFS vnode。详见 [用户接口与 devfs](userspace-and-devfs.md)。

典型生命周期：

```c
/* 1. 注册 */
static driver_t my_uart_driver = {
    .name       = "my-uart",
    .bus        = &platform_bus,
    .id_table   = my_ids,
    .probe      = my_uart_probe,
    .remove     = my_uart_remove,
    .class_type = DEV_CLASS_CHAR,
    .class_ops  = &my_uart_ops,
};
DRIVER_REGISTER(my_uart_driver);

/* 2. probe：初始化硬件 */
static int my_uart_probe(device_t *dev)
{
    my_uart_t *u = kzalloc(sizeof(*u));
    if (!u) return -ENOMEM;

    dev->drv_priv = u;
    init_uart(u, dev->resources);
    return 0;
}

/* 3. read/write：字节流 */
static int my_uart_read(device_t *dev, void *buf, size_t count)
{
    my_uart_t *u = dev->drv_priv;
    size_t n = min(count, rx_ready(u));
    if (n == 0) return -EAGAIN;
    copy_from_rx(u, buf, n);
    return (int)n;
}

/* 4. remove：关中断、释放 */
static int my_uart_remove(device_t *dev)
{
    my_uart_t *u = dev->drv_priv;
    disable_uart_irq(u);
    kfree(u);
    return 0;
}
```

⚠️ 注意：不要从 UART 驱动里直接实现 `termios` 或行编辑。这些属于 tty 层；把行规塞进硬件驱动会让新板子无法复用同一套 UART 代码。

## Input：`DEV_CLASS_INPUT`

```c
typedef struct input_dev_ops {
    int (*read)(device_t *, void *buf, size_t count);
    int (*ioctl)(device_t *, unsigned long req, void *arg);
    int (*poll)(device_t *, short events);
} input_dev_ops_t;
```

事件 ABI 为 `kernel/include/drivers/input/virtio_input.h` 中的：

```c
struct input_event {
    uint32_t time_sec;
    uint32_t time_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
```

- `read` 只返回完整事件，成功返回字节数；buffer 小于一个事件返回 `-EINVAL`，无事件返回 `-EAGAIN`。
- `poll` 返回非零表示至少一个事件可读，不消费事件。
- 键盘/鼠标一批状态变化后应输出 `EV_SYN/SYN_REPORT`，避免消费者看到不完整帧。
- 每实例维护 ring；满时不得覆盖尚未读取的数据，必须记录丢弃计数或通过类 ioctl 暴露可观察的 overflow 状态。
- `/dev/event0` 聚合所有 `DEV_CLASS_INPUT`。新 input 驱动只需正确注册类，不要新增硬件私有 getter。

参考实现：`virtio_input.c`、`xhci_hid.c`。PS/2 是 x86 板级控制器服务，不属于可复用 input class；新驱动必须使用本类接口。

典型生命周期：

```c
/* 1. 注册 */
static driver_t my_hid_driver = {
    .name       = "my-hid",
    .bus        = &pci_bus,
    .id_table   = my_ids,
    .probe      = my_hid_probe,
    .remove     = my_hid_remove,
    .class_type = DEV_CLASS_INPUT,
    .class_ops  = &my_hid_ops,
};
DRIVER_REGISTER(my_hid_driver);

/* 2. probe：初始化并创建事件 ring */
static int my_hid_probe(device_t *dev)
{
    my_hid_t *h = kzalloc(sizeof(*h));
    if (!h) return -ENOMEM;

    dev->drv_priv = h;
    h->ring = ring_alloc(INPUT_RING_SIZE);
    if (!h->ring) {
        kfree(h);
        return -ENOMEM;
    }
    init_hid(h);
    return 0;
}

/* 3. 中断：合成事件并推入 ring */
static void my_hid_irq(device_t *dev)
{
    my_hid_t *h = dev->drv_priv;
    struct input_event ev;

    ev.type = EV_KEY;
    ev.code = KEY_A;
    ev.value = 1;
    push_event(h, &ev);

    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    push_event(h, &ev);
}

/* 4. read/poll：暴露给用户聚合器 */
static int my_hid_read(device_t *dev, void *buf, size_t count)
{
    my_hid_t *h = dev->drv_priv;
    if (count < sizeof(struct input_event))
        return -EINVAL;
    if (ring_empty(h->ring))
        return -EAGAIN;
    return ring_pop(h->ring, buf, sizeof(struct input_event));
}

/* 5. remove：停止中断、释放 ring */
static int my_hid_remove(device_t *dev)
{
    my_hid_t *h = dev->drv_priv;
    disable_hid(h);
    ring_free(h->ring);
    kfree(h);
    return 0;
}
```

❌ 不要这样做：当 ring 满时直接覆盖旧事件。这会让用户态看到乱序按键；应该记录丢弃计数，并通过 ioctl 或状态位暴露 overflow。

## Display：`DEV_CLASS_DISPLAY`

```c
typedef struct gpu_dev_ops {
    int (*get_info)(device_t *, uint32_t *width, uint32_t *height, uint32_t *bpp);
    int (*get_fb)(device_t *, uintptr_t *paddr, size_t *size);
    int (*flush)(device_t *, uint32_t x, uint32_t y,
                 uint32_t width, uint32_t height);
    int (*ioctl)(device_t *, unsigned long req, void *arg);
} gpu_dev_ops_t;
```

Display 驱动成功 probe 后调用 `gpu_device_register(dev)`；remove 前调用 `gpu_device_unregister(dev)`。首个成功注册的设备拥有 `/dev/fb0`。操作与映射规则详见 [Display/Framebuffer](display.md)。

典型生命周期：

```c
/* 1. 注册 */
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

/* 2. probe：分配 framebuffer 并注册 display */
static int my_gpu_probe(device_t *dev)
{
    my_gpu_t *g = kzalloc(sizeof(*g));
    if (!g) return -ENOMEM;

    dev->drv_priv = g;
    g->fb = alloc_framebuffer(WIDTH, HEIGHT, 32);
    if (!g->fb) {
        kfree(g);
        return -ENOMEM;
    }

    if (gpu_device_register(dev) < 0) {
        free_framebuffer(g->fb);
        kfree(g);
        return -EIO;
    }
    return 0;
}

/* 3. 使用：用户映射 framebuffer 后写像素 */
static int my_gpu_get_fb(device_t *dev, uintptr_t *paddr, size_t *size)
{
    my_gpu_t *g = dev->drv_priv;
    *paddr = g->fb_paddr;
    *size  = g->fb_size;
    return 0;
}

static int my_gpu_flush(device_t *dev, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h)
{
    my_gpu_t *g = dev->drv_priv;
    if (x >= g->width || y >= g->height)
        return -EINVAL;
    if (w > g->width - x)  w = g->width - x;
    if (h > g->height - y) h = g->height - y;
    /* ... 提交设备命令 ... */
    return 0;
}

/* 4. remove：注销 display、释放显存 */
static int my_gpu_remove(device_t *dev)
{
    my_gpu_t *g = dev->drv_priv;
    gpu_device_unregister(dev);
    free_framebuffer(g->fb);
    kfree(g);
    return 0;
}
```

⚠️ 注意：注册 display 是 probe 的最后一个步骤。在它之前失败，用户不会看到半成品设备；在它之后失败，必须先 `gpu_device_unregister` 再释放资源，否则 `/dev/fb0` 可能指向已释放内存。

## 新增设备类

仅在至少两个硬件实现，或一个硬件实现和一个独立内核消费者共享稳定语义时新增类。必须同时完成：在 `driver_class.h` 定义最小 ops；规定每个参数单位、返回值、阻塞和并发语义；实现按类枚举的消费者；定义用户 ABI 时使用固定宽度字段并带版本/长度；增加文档和构建门禁。不得把任意厂商命令 blob 塞入通用 ioctl 代替类设计。

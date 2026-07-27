# PCI 与 VirtIO 驱动开发

本章讲 A20OS 的 PCI/PCIe 枚举、BAR 资源和 modern VirtIO transport。VirtualBox ARM64、VirtualBox x86_64 以及多种 QEMU/物理平台共享这些基础设施。平台相关运行细节见 [VirtualBox 驱动栈](../platforms/virtualbox.md)、[VirtualBox ARM64 运行手册](../platforms/virtualbox-aarch64.md) 和 [VirtualBox x86_64 运行手册](../platforms/virtualbox-x86_64.md)。

## PCI 发现路径

平台先获得 ECAM 地址和 bus 范围，再调用：

```c
pci_enumerate(ecam_kernel_va, first_bus, last_bus_exclusive);
```

枚举器注册 `pci_bus`，扫描 bus/device/function，为每个 function 创建长生命周期 `device_t`、PCI 私有 `plat_data` 和资源数组，然后 `device_register()`。此时 BAR 只被记录，尚未保证启用/分配；具体驱动 probe 调用 `pci_enable_and_assign_bars(dev)`。

VirtualBox ARM64 的 ECAM 来自 UEFI ACPI RSDP -> XSDT/RSDT -> MCFG。平台只接受 segment 0、bootstrap 页表已覆盖且低于 4 GiB 的 ECAM，目前只枚举首个可用 segment。x86_64 的 host 配置由架构 PCI host 实现提供。

## PCI ID 表

```c
static const device_id_t ids[] = {
    { .vendor = 0x8086, .device = 0x100e,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY,
      .driver_data = MODEL_82540EM },
    { 0 },
};
```

`vendor/device` 是 PCI configuration space 的 16 位值。未限制的 subsystem 字段必须显式填 ANY。class 驱动使用 ANY ID 表和 `driver_t.match`，在 bus ID 匹配后验证完整的 class/subclass/prog-if；不能只检查 class/subclass 后假定所有 programming interface 相同。

匹配后 `dev->matched_id` 可读取 `driver_data`。`pci_device_id(dev)` 返回 `vendor << 16 | device`；`pci_class_code(dev)` 返回 `class << 16 | subclass << 8 | prog-if`。

## BAR 处理

probe 首先：

```c
int ret = pci_enable_and_assign_bars(dev);
if (ret < 0)
    return ret;
resource_t *regs = pci_get_bar_resource(dev, 0);
```

helper 会在 BAR sizing 时暂时关闭地址 decoding，计算大小，为 x86_64 或 LoongArch 未分配的 MMIO BAR 从平台 PCI 窗口分配地址，启用 memory/bus-master，并把 MMIO BAR 转换成 `RES_MMIO`。原始 BAR 为零可能表示“已实现但固件尚未分配”，因此仍须写全一 sizing；只有 sizing mask 也表明 size 为零时才是未实现 BAR。分配时必须保留 sizing mask 返回的 I/O、32/64 位和 prefetchable 类型位。

LoongArch 分配必须完全落在 `PCIE_MMIO_BASE..PCIE_MMIO_BASE+PCIE_MMIO_SIZE`，越界时 probe 失败而不是写入截断地址。I/O port BAR 当前不进入资源数组。INTx line 如果有效会追加 `RES_IRQ`。

64 位 BAR 占两个配置 BAR slot，但只生成一个 MMIO resource，所以必须用 `pci_get_bar_resource(dev, physical_bar_number)`。校验 `end >= start` 和最小 aperture 大小后才能访问。BAR 地址已经通过 `arch_pci_bar_to_resource` 变成内核可访问地址。

## PCI probe 模式

```c
static driver_t my_driver = {
    .name = "my-pci-device",
    .id_table = ids,
    .bus = &pci_bus,
    .probe = my_probe,
    .remove = my_remove,
    .class_ops = &my_ops,
    .class_type = DEV_CLASS_BLOCK,
};
```

把 `.bus` 设为 `&pci_bus` 最清晰。部分兼容驱动设 `NULL` 以同时匹配 PCI 和 VirtIO-MMIO，只有 ID 表和 probe 真正支持两个 transport 时才允许这样做。

完整生命周期示例：

```c
/* 1. 注册驱动 */
static driver_t my_pci_driver = {
    .name       = "my-pci-device",
    .bus        = &pci_bus,
    .id_table   = my_ids,
    .probe      = my_pci_probe,
    .remove     = my_pci_remove,
    .class_type = DEV_CLASS_BLOCK,
    .class_ops  = &my_ops,
};
DRIVER_REGISTER(my_pci_driver);

/* 2. probe：启用 BAR、初始化 transport、注册类 */
static int my_pci_probe(device_t *dev)
{
    my_pci_dev_t *d = kzalloc(sizeof(*d));
    if (!d) return -ENOMEM;

    dev->drv_priv = d;

    if (pci_enable_and_assign_bars(dev) < 0) goto fail;

    resource_t *regs = pci_get_bar_resource(dev, 0);
    if (!regs || regs->end < regs->start + MIN_APERTURE) goto fail;
    d->reg_base = (void *)regs->start;

    if (init_transport(d) < 0) goto fail;
    if (setup_queues(d) < 0) goto fail;

    return 0;

fail:
    kfree(d);
    return -ENODEV;
}

/* 3. remove：停止 I/O、释放资源 */
static int my_pci_remove(device_t *dev)
{
    my_pci_dev_t *d = dev->drv_priv;
    stop_queues(d);
    kfree(d);
    return 0;
}
```

## 协议驱动的可移植契约

PCI 协议驱动只能依赖以下公共输入：

- `device_t`、`matched_id` 与 `pci_class_code()` 提供身份；
- `pci_enable_and_assign_bars()` 与 `pci_get_bar_resource()` 提供可访问 MMIO；
- `read*/write*` 提供有序寄存器访问；
- `dma_alloc_coherent_aligned()` 返回 CPU 地址和设备 DMA handle，`dma_sync_for_*()` 转移可见性；
- `request_irq/free_irq` 提供中断能力，或由驱动明确记录轮询模式。

驱动不得包含架构私有 `platform.h`，不得自行加 `PAGE_OFFSET`，也不得假定 DMA handle 等于 CPU 指针。平台若缺少 PCI 枚举、BAR 窗口分配或正确 DMA/cache hook，应在平台层补齐；不能用 `CONFIG_<ARCH>` 把通用协议代码隐藏起来。

当前平台状态：

| 平台 | PCI 发现/BAR | NVMe/HDA 证据 |
|---|---|---|
| QEMU x86_64 q35 | ECAM 与 MMIO BAR 分配 | HDA BDL DMA smoke 已验证；NVMe 可绑定 |
| QEMU LoongArch64 virt | ECAM 与 `0x40000000` PCI MMIO 窗口分配 | HDA BDL DMA、NVMe admin queue/Identify 已联合验证 |
| VirtualBox AArch64 | ACPI MCFG；依赖固件预分配且低于已映射范围的 BAR | 通用驱动可编译，尚无 HDA/NVMe 运行日志 |
| QEMU AArch64/RISC-V64 virt | 当前 board 只枚举 VirtIO-MMIO | 驱动可编译，不构成 PCI 运行支持 |

LoongArch 验证入口是 `make smoke-pci-portability`。它在同一客户机挂载 HDA codec 与 NVMe namespace，要求 HDA PCM DMA 完成且两个 class 驱动都绑定。NVMe 当前验证到 Identify；块数据读写仍需独立的可丢弃镜像测试。

## VirtIO transport

`virtio_transport_t` 提供 MMIO 风格的 `read32/write32`、私有数据、legacy 标志和 IRQ。设备驱动使用统一 `VIRTIO_MMIO_*` offset；PCI transport 在内部翻译 common/notify/ISR/device capabilities，MMIO transport 直接访问 slot。

modern PCI 初始化：

```c
virtio_transport_t vt;
if (pci_virtio_transport_init(dev, VIRTIO_ID_SCSI, &vt) < 0)
    return -ENODEV;
```

helper 要求 capability list 中存在 common cfg、notify cfg、device cfg 和有效 notify multiplier。当前 PCI transport 设置 `irq = -1`，采用轮询。

## VirtIO feature 协商

标准顺序：设备 status 清零；置 ACKNOWLEDGE/DRIVER；读取 feature words；只写驱动理解的位；必须协商 `VIRTIO_F_VERSION_1`；置 FEATURES_OK 并回读确认；配置所有 queue；最后置 DRIVER_OK。任何失败把 FAILED 写入 status，probe 回滚。

不得接受后不实现 feature。例如协商 packed ring、indirect descriptors、event idx 后就必须遵守其布局。当前驱动使用 split virtqueue。

典型协商流程：

```c
static int virtio_negotiate(virtio_transport_t *vt)
{
    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    vt->write32(vt, VIRTIO_MMIO_STATUS,
                 VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    uint32_t features = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    features &= VIRTIO_F_VERSION_1 | MY_DRIVER_FEATURES;
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, features);

    vt->write32(vt, VIRTIO_MMIO_STATUS,
                 VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
                 VIRTIO_CONFIG_S_FEATURES_OK);
    if (!(vt->read32(vt, VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK))
        return -ENODEV;

    setup_queues(vt);
    vt->write32(vt, VIRTIO_MMIO_STATUS,
                 VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
                 VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
    return 0;
}
```

## Split virtqueue

descriptor 包含 DMA 地址、长度、flags 和 next。设备可读 descriptor 必须在设备可写 descriptor 之前，具体设备协议可能进一步规定顺序。构造链后同步 descriptor/buffer，写 avail ring 和 idx，屏障后 notify。完成时同步 used ring，验证：

- used id 小于 queue size，并且是本次请求 head；
- used length 不超过提供 buffer；
- 16 位 idx 回绕用无符号差值处理；
- 请求未完成前 descriptor 和 buffer 不复用。

VirtIO-SCSI data-in 的链顺序是 request -> response -> data-in；VirtualBox 会在首个 writable descriptor 处分割 outbound/inbound，因此不能把 data-in 放到 response 前。

```c
/* 构造一次请求：desc[0] 设备可读，desc[1] 设备可写 */
static int submit_request(vq_t *vq, void *out, size_t out_len,
                          void *in, size_t in_len)
{
    uint16_t head = vq->free_head;
    vq->desc[head].addr  = dma_phys(out);
    vq->desc[head].len   = out_len;
    vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[head].next  = head + 1;

    vq->desc[head + 1].addr  = dma_phys(in);
    vq->desc[head + 1].len   = in_len;
    vq->desc[head + 1].flags = VIRTQ_DESC_F_WRITE;

    dma_sync(vq->desc, sizeof(vq->desc[0]) * 2);
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    vq->avail->idx++;
    dma_sync(vq->avail, sizeof(*vq->avail));
    memory_barrier();
    notify(vq);
    return 0;
}
```

## 现有 VirtIO PCI ID

| 类型 | modern ID | transitional ID | A20OS 驱动 |
|---|---|---:|---|
| network (1) | `1af4:1041` | `1af4:1001` | `virtio_net.c` |
| block (2) | `1af4:1042` | `1af4:1002` | `virtio_blk.c` |
| SCSI (8) | `1af4:1048` | `1af4:1008` | `virtio_scsi.c` |
| GPU (16) | `1af4:1050` | `1af4:1010` | `virtio_gpu.c` |
| input (18) | `1af4:1052` | `1af4:1012` | `virtio_input.c` |

transitional ID 的 subsystem device 常用来区分 VirtIO type，ID 表必须按现有 bus match 语义填写。

PCI BAR 的 sizing、分配和 capability 地址解析只属于 `pci_enumerate()` 与 `pci_virtio_transport_init()`。驱动、类消费者和 `arch_virtio_*_probe()` 不得再次扫描同一 PCI host 或重写 BAR。QEMU/VirtualBox 的 PCI VirtIO 设备走统一 PCI bus；VirtIO-MMIO 设备由 `virtio_mmio_enumerate()` 发布，二者最终进入同一 driver probe，不以运行期 fallback 互相探测。

## 失败定位

没有 probe 日志时先找 `[BUS] pci ... id=vendor:device`；没有设备说明 ECAM/固件问题。有设备但未绑定，检查 ID 表和 `.driver_init`。BAR setup 失败检查 BAR size/地址窗口。`incomplete capabilities` 是 VirtualBox 控制器模式或 capability 解析问题。feature rejected 是驱动写了设备不接受的位。queue timeout 时同时检查 DMA 地址是否为物理地址、cache sync、descriptor writable 顺序、queue notify offset 和设备 status。

 不要这样做：在 ID 表里只写 `vendor = 0x1af4, device = 0x1000` 这种宽泛 class ID 就指望所有 VirtIO 设备都能匹配。 subsystem 字段必须显式填 ANY，否则匹配语义会错；更不要把不支持的 feature 位写进 `DRIVER_FEATURES` 里协商。

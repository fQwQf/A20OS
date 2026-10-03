# PCI 与 VirtIO 驱动开发

A20OS 的 PCI/PCIe 枚举、BAR 资源和 modern VirtIO transport 由 VirtualBox ARM64、VirtualBox x86_64 以及多种 QEMU/物理平台共享。平台相关运行细节见 [VirtualBox 驱动栈](../../platforms/virtualbox.md)、[VirtualBox ARM64 运行手册](../../platforms/virtualbox-aarch64.md) 和 [VirtualBox x86_64 运行手册](../../platforms/virtualbox-x86_64.md)。

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

## MSI-X

`kernel/drivers/bus/pci_msix.c` 提供与协议无关的 MSI-X 层：解析能力、按
平台提供的信息编排表项、分配向量。协议驱动只声明"我要几个向量"。

```c
int r = pci_msix_enable(dev, vectors);          /* 关 Enable + 置 function mask */
for (i = 0; i < vectors; i++)
    pci_msix_program_vector(dev, i, base + i);  /* 表项初值，先 masked */
request_irq(base + i, handler, 0, dev);        /* 装 handler */
pci_msix_set_vector_mask(dev, i, 0);            /* 逐项解除 mask */
pci_msix_commit(dev);                           /* 清 function mask，置 Enable */
```

失败一律回到 `-EOPNOTSUPP`/`-EINVAL` 等负 errno，由驱动退回 INTx 或轮询；
不允许"半装好"的中间态存在。

### 表在哪里：两个字段，两种编码

能力头之后偏移 `0x10` 是 **Vector Control**（PCIe 形式），偏移 `0x04` 是
**Message Address Lower**（继承自 PCI 之前的 MSI 编码）。两处都可能描述同一
件事，且**字段决定编码，不由设备决定**：

| 字段 | BIR 位 | 偏移位 | 偏移是否缩放 |
|---|---|---|---|
| Vector Control | `[3:1]` | `[31:12]` | 是，字节偏移 = 值 << 4 |
| Message Address Lower | `[2:0]` | `[31:3]` | 否，字节偏移就是值 |

`pci_msix_capability()` 优先读 Vector Control，读到零则退回 Message Address
Lower，并在 `pci_msix_info_t::from_vector_ctrl` 里记录来源。

为什么 Vector Control 常读到零：**它由平台固件写**。用 `-kernel` 引导意味着
没有任何固件跑过，所有设备的 Vector Control 都是零。QEMU 的 virtio-pci
（`virtio_pci_dc_realize` → `msix_init_exclusive_bar`）和 e1000e 都走
`msix_init()` 里的 `pci_set_long(config + PCI_MSIX_TABLE, ...)`，即把表位置
写进 Message Address Lower，用的正是 pre-PCIe 编码。所以这不是兼容包袱而是
实际布局。

两处都是零时，BIR 0 / 偏移 0 与"没人配置过"无法区分，`pci_msix_enable()`
返回 `-EOPNOTSUPP` 并保留传统中断路径——对 BAR0 是 I/O 窗口的设备，照字面
解释会把表项写到活寄存器上。

**BAR 号是实现细节，不要猜。** QEMU 的 virtio-pci 表在 BAR1（`msix_bar_idx
= 1`），e1000e 在 BAR3，virtio spec 一个字都没规定。表项写完立刻读回，
不符就是打错了窗口，`pci_msix_program_vector()` 因此会拒绝
`"this window is not an MSI-X table"`。

### `queue_msix_vector` 是表索引，不是中断号

common config 偏移 `0x1A`（与队列选择 `0x16` 配合使用）里的值是 **MSI-X 表的
第几项**，不是这条中断线的号。QEMU 的 `virtio_pci_common_write()` 会判
`val < proxy->nvectors`，超界就不调用 `msix_vector_use()`，之后
`msix_notify()` 因为 `msix_entry_used[vector]` 为 0 直接返回——表项看起来
编程得完全正确，永远不会有中断。把 208 写进去正是这样。

表项本身携带的是中断号（`pci_msix_program_vector()` 写进 message data），
两处不要混。

### 平台钩子与消息地址

```c
int arch_msix_message_address(uint32_t vector, uint32_t *addr_lo, uint32_t *addr_hi);
int arch_msix_vector_setup(uint32_t vector, int masked);
int arch_irq_msix_vector_range(int *base, int *end);
```

三个都是 weak 符号，默认返回负 errno。只有 x86_64 实现了它们：向量窗口
`0xD0..0xF0`，LVT 按 `LAPIC_LVT_TIMER + ((V - 0x10) & 0xFF) * 16` 定位。
其他架构返回失败，驱动因此停在轮询或 INTx，而不会去编程一条永远不会被投递
的中断。

消息地址必须是 **APIC 自己那一页的基地址**（x86_64 上是 `LAPIC_PHYS_BASE`
`0xFEE00000`），向量放在消息数据里，不放在地址里。真实硬件确实会忽略该页内
的偏移，但软件没有理由去依赖这一点：LAPIC 窗口同时是寄存器文件，把向量 OR
进地址得到的偏移落在前 1 KiB 内，实现按寄存器写解码，**什么都不投递，也不
报错**。表项编程正确、message control 正确、function mask 已清，设备照常
notify，就是一条中断都没有。向页基址投递是所有实现都解释为"这是一个中断"
的唯一地址。

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
static int my_pci_probe(device_t *dev){
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
static int my_pci_remove(device_t *dev){
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

| 平台 | PCI 发现/BAR | NVMe/HDA/VirtIO Sound 证据 |
|---|---|---|
| QEMU x86_64 q35 | ECAM 与 MMIO BAR 分配 | HDA BDL DMA、virtio-sound PCI PCM/WAV 已验证；NVMe 可绑定 |
| QEMU LoongArch64 virt | ECAM 与 `0x40000000` PCI MMIO 窗口分配 | HDA BDL DMA；NVMe queue/Identify、跨 chunk 写入/flush/读回已联合验证 |
| QEMU RISC-V64 virt | ECAM 与高半核映射的 PCI MMIO BAR 窗口 | HDA 环形 PCM 与 Wayland/PulseAudio 已验证；virtio-sound MMIO probe 已验证 |
| VirtualBox AArch64 | ACPI MCFG；依赖固件预分配且低于已映射范围的 BAR | 通用驱动可编译，尚无 HDA/NVMe 运行日志 |
| QEMU AArch64 virt | 当前 board 只枚举 VirtIO-MMIO | 驱动可编译，不构成 PCI 运行支持 |

LoongArch 验证入口是 `make smoke-pci-portability`。它在同一客户机挂载 HDA codec 与 NVMe namespace，要求 HDA PCM DMA 完成且两个 class 驱动都绑定。目标自动创建 128 MiB 可丢弃 NVMe 镜像，并以 `CONFIG_NVME_SMOKE_TEST` 写入 LBA 0 开始的 17 个 512 字节扇区，强制跨越 8 KiB bounce chunk，再执行 flush、读回和逐字节比较。该配置会改写介质，禁止对普通镜像或真实磁盘启用。

NVMe controller 启用前必须由 `CAP.CSS` 声明 NVM command set，并由 `CAP.MPSMIN/MPSMAX` 覆盖内核使用的 4 KiB memory page；驱动据此设置 `CC.MPS`，并按 `CAP.TO` 等待 `CSTS.RDY`。能力不兼容时 probe 直接返回 `-EOPNOTSUPP`，不能写 `CC.EN` 后等待无意义的固定超时。

## VirtIO transport

`virtio_transport_t` 提供 MMIO 风格的 `read32/write32`、私有数据、legacy 标志和 IRQ。设备驱动使用统一 `VIRTIO_MMIO_*` offset；PCI transport 在内部翻译 common/notify/ISR/device capabilities，MMIO transport 直接访问 slot。block、net、GPU、input 和 sound 都复用该边界；virtio-sound 的同一个协议驱动可绑定 PCI device 25 或 VirtIO-MMIO device 25。

modern PCI 初始化：

```c
virtio_transport_t vt;
if (pci_virtio_transport_init(dev, VIRTIO_ID_SCSI, &vt) < 0)
    return -ENODEV;
```

helper 要求 capability list 中存在 common cfg、notify cfg、device cfg 和有效 notify multiplier。PCI transport 的 `msix_prepare`/`msix_arm`/`msix_teardown` 把 MSI-X 暴露成与 MMIO transport 同形的三个回调，block/net 驱动据此按「MSI-X → INTx → 轮询」的顺序降级，`irq = -1` 只表示这条 INTx 线已经让给了 MSI-X。

## VirtIO feature 协商

标准顺序：设备 status 清零；置 ACKNOWLEDGE/DRIVER；读取 feature words；只写驱动理解的位；必须协商 `VIRTIO_F_VERSION_1`；置 FEATURES_OK 并回读确认；配置所有 queue；最后置 DRIVER_OK。任何失败把 FAILED 写入 status，probe 回滚。

不得接受后不实现 feature。例如协商 packed ring、indirect descriptors、event idx 后就必须遵守其布局。当前驱动使用 split virtqueue。

典型协商流程：

```c
static int virtio_negotiate(virtio_transport_t *vt){
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

## Board-bound transport 的发布与重试

PCI 与 `virtio_mmio_enumerate()` 两条路径的差别只有「谁来构造这个 transport」，但它们必须落到同一个可见性规则上：**驱动不会自动出现在设备模型里**。

`device_find_by_class()` 只看 `dev->drv->class_type`；`driver_matches_device()` 对双方都没有 bus 的组合，要求存在 `match()` 回调，否则拒绝绑定。board 自己构造的 transport（QEMU virt 的 `virtio-mmio` slot、ppc64le 的 `spapr-vio`）没有 bus，也没有 `match()`，所以它必须**自己调用 `device_register()`**，否则 `mount_setup_block_device()` 之类的 class 消费者永远看不到这块盘。症状是启动日志里 transport 建好了、`notify` 也在动，但 `/bin` 挂不上、`init` 报 `no init program found`。

第二个坑是重试次数：QEMU virt 上 virtio-mmio slot 是递增的，板级代码若按「probe 一次，失败就放弃」，那么 slot 0 上挂的设备会把整条总线判死。应当**一直 probe 到 slot 返回非设备为止**（`VIRTIO_MMIO_MAGIC_VALUE` 为 0 即无设备）。

这两点合起来解释了一个很难定位的故障：同一个 QEMU 机器上 aarch64 与 ppc64le 行为不同——因为 deployment profile 不同（见 `deployment-profiles.md`），virtio-blk 在 aarch64 是**可加载模块**、在 ppc64le 是**内建**。内建路径下 `device_register()` 是直接调用，加载路径下它必须出现在 `drv_export_table[]` 里，否则模块加载时就是 `unresolved symbol 'device_register'`。

## 失败定位

没有 probe 日志时先找 `[BUS] pci ... id=vendor:device`；没有设备说明 ECAM/固件问题。有设备但未绑定，检查 ID 表和 `.driver_init`。BAR setup 失败检查 BAR size/地址窗口。`incomplete capabilities` 是 VirtualBox 控制器模式或 capability 解析问题。feature rejected 是驱动写了设备不接受的位。queue timeout 时同时检查 DMA 地址是否为物理地址、cache sync、descriptor writable 顺序、queue notify offset 和设备 status。

ID 表里只写 `vendor = 0x1af4, device = 0x1000` 这种宽泛 class ID，匹配语义会错，也无法覆盖所有 VirtIO 设备：subsystem 字段必须显式填 ANY。同样不要把不支持的 feature 位写进 `DRIVER_FEATURES` 里协商。

# 可安装内核驱动

A20OS 现在同时支持三种驱动路径：

| 路径 | 执行位置 | 适用范围 |
|---|---|---|
| `kernel/drivers/` | 内核镜像内 | 已接入统一 bus/class 模型的内建驱动 |
| `drvmod` (`*.a20drv`) | 内核态、内核 direct-map | 运行时可分发的原生驱动模块 |
| 用户态驱动（`*.a20drv`）| 用户态 | 双驻留驱动（hybrid-kernel）的用户侧，见 `docs/hybrid-kernel/04-dual-placement.md` |

`drvmod` 是 A20OS 的可加载内核模块机制：模块是 ELF64 `ET_REL` 可重定位对象，由内核加载进 direct-map，运行时只允许解析驱动框架（`kernel/drvmod/framework.c`）的导出符号白名单。加载器、框架与示例位于 `kernel/drvmod/`，模块 ABI 定义在 `kernel/include/drvmod/drvmod.h`。

## 统一 `.a20drv` 格式与统一驱动管理器

所有可加载驱动文件（内核模块与用户态驱动）都必须使用 `.a20drv` 后缀，并带有只读 ELF `.a20drv` 段。该段的 `a20_driver_descriptor_t` 由 `kernel/include/drivers/driver_descriptor.h` 定义，是**唯一的驱动元数据来源**（没有 `.a20inf` 等旁车清单），包含：

- `magic` 与 `version`：描述符格式身份；
- `placement`：`kernel-module` 或 `user-service` —— 强边界，不是可互换模式；
- `type`：RTC、BLOCK、INPUT、AUDIO、SECURITY、NET、DISPLAY 或 USB；
- `name`：稳定的驱动名；
- `abi` 与 `resource_mask`：驱动接口版本与资源需求（MMIO/IRQ/IOPORT/DMA）；
- `flags`：生命周期所有权（`A20_DRIVER_FLAG_SUPERVISED` 表示用户态服务的 spawn/restart/健康由外部服务监督者如 svcmgr 拥有，内核管理器只记录）；
- `match[]` / `match_count`：该驱动可拥有的设备身份（bus/vendor/device）。

后缀不决定权限域：ELF 类型与 `placement` 共同决定加载器。内核 `drvmod_load()` 只接受 ELF64 `ET_REL` 且 `placement=kernel-module` 的文件；普通 ELF 执行加载器在执行以 `.a20drv` 结尾的文件时只接受 `placement=user-service` 的描述符。因此内核模块不能作为用户程序执行，用户态驱动也不能被映射进内核 direct-map。

**统一驱动管理器**（`kernel/drivers/core/driver_manager.c`）是可选驱动发现与激活的唯一权威。`driver_manager_init()` 在启动期（`init_kthread`）：

1. 注册模块/用户服务绑定的板级设备（goldfish-rtc、ps2、tpm、virtio-input-slot5 等）为统一 `platform_device_t`；
2. 扫描 DriverStore `/bin/lib/drivers` 下所有 `.a20drv`，读取描述符；
3. `kernel-module` 包：`drvmod_load` + `drvmod_init_all`，模块经 `drv_driver_register` 注册统一 `driver_t`，由驱动核心按 `device_t` 匹配并 probe；
4. `user-service` 包：若 `flags` 带 `SUPERVISED` 只记录（svcmgr 负责生命周期）；否则当描述符匹配的 MMIO 窗口真实存在（`udriver_window_present` 探测）时，由 `driver_manager_spawn_user` 直接生成本地用户进程。

每个物理设备只有一个 owner：`device_t.user_owned` 表示设备归用户态驱动所有，驱动核心只允许 `driver_t.read_only_probe` 的内核驱动绑定它（双驻留内核壳），完整初始化归用户驱动。

## 模块接口

模块只能解析 `kernel/drvmod/framework.c` 中的导出表。当前导出接口包括：

- 内存与日志：`drv_alloc`、`drv_free`、`drv_log`。
- MMIO：`drv_map_mmio`（返回校验后的 direct-map VA）、`drv_unmap_mmio`、`drv_read32`、`drv_write32`。
- 端口 I/O（x86）：`drv_in8`、`drv_out8`。
- DMA：`drv_dma_alloc_coherent`、`drv_dma_free_coherent`（coherent，稳定设备地址）。
- 延时/时间：`drv_udelay`、`drv_mdelay`、`drv_clock_ticks`。
- 中断：`drv_register_isr`、`drv_unregister_isr`（桥接 hwapi `request_irq`，向量在注册时校验范围，arch IRQ 分发真实投递）。
- 统一驱动核心桥接：`drv_driver_register/unregister`、`drv_device_get_resource`、`device_get_resource`、`device_find_by_class`、`drv_driver_probe_all`、`platform_bus`。
- PCI 类驱动访问器（device_t 为中心，模块以 `bus = &pci_bus` 注册标准 `driver_t` 后使用）：`pci_bus`、`pci_class_code`、`pci_device_id`、`pci_get_bar_resource`、`pci_intx_irq`、`pci_enable_and_assign_bars`。
- 控制台输入路径（PS/2 模块）：`uart_receive_char`。
- 调度/等待原语（模块完成路径）：`proc_park_prepare/commit/cancel/finish`、`proc_wake_q_init/flush`、`wait_queue_init/link/unlink/collect_one/collect_all`、`mutex_init/lock/unlock`、`timer_get_ticks`、`mdelay/udelay`。
- 日志与分配器：`klog_write/klog_level`、`kmalloc/kfree/kcalloc`。模块内日志建议走 `drv_log`：`klog_level` 是外部变量，经 GOT 加载（loongarch64 的 GOT 支持已实现，但模块日志用纯 CALL36 更稳）。
- DMA 增强：`dma_alloc_coherent_aligned/free_coherent_aligned`、`dma_sync_for_cpu/device`。
- ACPI 发现（x86_64）：`firmware_acpi_tpm2`。
- 输入 mux 唤醒与 virtio PCI 传输：`input_mux_wake`、`pci_virtio_transport_init`、`arch_current_cpu_id`。
- 基础字符串/内存函数：`strncpy`、`memset`、`memcpy`、`memcmp`、`strcmp`、`strlen`、`strstr`，以及自旋锁内联用到的 `proc_current`/`proc_task_pid`/`printf`。

模块不得直接引用普通内核符号。未解析符号、未知重定位、非法 ELF 类型、越界的 section/symbol/strtab/relocation 都会在加载前拒绝，不会修改内核页表或堆。

## 模块编写模型

模块与内建驱动共用**同一个** `driver_t` 模型，没有第二套 `drv_driver_t`。入口约定：

```c
uintptr_t DriverEntry(void)
{
    static driver_t drv = {
        .name       = "my-driver",
        .id_table   = my_ids,       /* 与内建驱动相同的 device_id_t 表 */
        .bus        = &platform_bus,
        .probe      = my_probe,     /* probe(device_t*) */
        .remove     = my_remove,
        .class_type = DEV_CLASS_NONE,
    };
    return drv_driver_register(&drv) == 0 ? 0 : 1;
}
```

`DriverEntry` 只注册驱动，**不得**在入口里访问硬件；设备绑定由驱动核心按既有 `device_t` 匹配路径完成（`device_register` 触发的重探或后续设备注册）。模块内声明统一的 `.a20drv` 描述符，由驱动管理器在加载前校验。

## 双驻留共享代码（drv_env）

`kernel/include/drivers/dual/drv_env.h` 为同一设备协议提供三种部署后端：`DRV_ENV_KERNEL`（内建内核驱动）、`DRV_ENV_USER`（用户态服务）、`DRV_ENV_DRVMOD`（模块）。模块用第三种后端的共享头（如 `virtio_input.h`）与用户态 `uinputd` 共享同一份协议源码，见 `kernel/drvmod/examples/vinput_probe.c`。

## 构建约束

模块按架构交叉编译为 ELF `ET_REL`，Makefile 的 `DRVMOD_CFLAGS` 按架构选择代码模型，使外部调用重定位没有（或只有 QEMU 尺寸内的）PC 相对范围限制：

| 架构 | 编译器 | 代码模型 | 外部调用重定位 |
|---|---|---|---|
| riscv64 | `riscv64-unknown-elf-gcc` | `-mcmodel=medany -march=rv64g -mno-relax` | `R_RISCV_CALL`（±2 GiB）|
| x86_64 | `x86_64-linux-gnu-gcc` | `-mcmodel=large -mno-red-zone -fno-pie` | `R_X86_64_64`（绝对地址，无范围限制）|
| aarch64 | `aarch64-linux-gnu-gcc` | `-mcmodel=large -fno-pic` | `R_AARCH64_CALL26`（±128 MiB，超范围时 loader 自动生成 veneer 跳板）|
| loongarch64 | `loongarch64-linux-gnu-gcc` | `-mcmodel=medium -fno-pic` | `R_LARCH_CALL36`（±2 GiB）|

加载器按 `e_machine` 在运行时选择重定位解码器，四种架构共用同一份边界检查、符号白名单和生命周期。`rv64g` 是有意选择的：禁用 `C` 扩展可避免把 16 位压缩指令误当作 32 位指令修补。模块必须小于 `DRV_MOD_MAX_SIZE`，并由连续物理页承载，确保 direct-map 地址连续。

LoongArch64 注意：工具链对局部地址引用使用 `pcalau12i + addi.d`（页相对）而非 `pcaddi`（字节相对），loader 按指令 opcode 自动区分；复杂 64 位 PCALA 链（`lu32i.d`/`lu52i.d` 与 ADD/SUB 表达式重定位）只出现在被丢弃的 `.eh_frame` 中。AArch64 超出 ±128 MiB 的外部 `BL` 会被 loader 改写为模块尾部的 veneer（`ldr x16,[pc,#8]; br x16; .quad S`），因此 kernel 文本与模块窗口的距离不再限制模块。

## 加载与绑定生命周期

1. 内核注册硬件设备资源：板级/驱动管理器把模块绑定的设备注册为统一 `platform_device_t`（或由 PCI/VirtIO 总线枚举得到 `device_t`）。
2. `drvmod_load()` 读取并验证 ELF 与 `.a20drv` 描述符（文件级、section 级、symbol/strtab、relocation 偏移全量边界检查），建立 shadow 布局。
3. 读缓冲与 shadow 均从 **frame 池**（`pfa_alloc`）分配，而非 kmalloc：kmalloc slab 与模块页在同一 buddy 上取页，重叠时 `kfree` 会把模块自己的页还给分配器，后续 DMA/其他模块分配会覆盖已加载模块的 GOT/rodata（loongarch64 实测踩过）。分配连续物理页，以最终运行地址（direct-map 窗口）计算重定位，在 shadow 上修补，校验 `DriverEntry` 位于 `.text`。
4. 复制到 direct-map，执行 `fence.i`/ICache 同步。
5. `drvmod_init_all()` 调用每个模块的 `DriverEntry`；模块经 `drv_driver_register` 把统一 `driver_t` 注册进驱动核心。
6. 驱动核心按既有 `device_t` 匹配路径绑定：`driver_register` 同步重探未绑定设备或后续设备注册触发 probe。没有第二套匹配表。
7. 注册了驱动的模块被 pin（卸载返回 `-EBUSY`），直到 remove 生命周期接入完整。

启动日志顺序（riscv64 QEMU virt）：

```text
[INIT] init_kthread started (pid=1)
[DRIVERMGR] driver manager init
[DRVMOD] loaded 'rtc.a20drv' id=0 ...
[GOLDFISH-RTC] probe ok: epoch=...
[DRIVER] device 'goldfish-rtc' bound to driver 'goldfish-rtc'
```

驱动模块是 ring 0 原生代码。描述符资源约束和框架 API 白名单降低准入风险，但不能隔离驱动崩溃；模块 bug 仍可能导致 kernel panic。

## DriverStore 与安装

镜像中的 DriverStore 位于 FAT32 的 `/lib/drivers`，由于开发镜像把 FAT32 挂载到 A20OS 的 `/bin`，运行时路径是 `/bin/lib/drivers`。**内核模块和用户态驱动包都放在这里**，由统一驱动管理器在启动时按描述符发现与激活。`drvctl` 提供暂存管理：

```text
drvctl install MODULE NAME
drvctl remove NAME
drvctl list
```

`drvctl install` 校验包内 `.a20drv` 描述符（唯一的元数据，**没有 `.a20inf` manifest**），把包复制为 `/bin/lib/drivers/NAME.a20drv`，并打印其 placement 与 type；`drvctl list` 从每个包的描述符读出 `placement/type/match`。暂存后由统一驱动管理器在下次启动激活；签名验证和运行时 unload 是下一阶段工作，不能把暂存成功误报为已经运行。

## 示例与验证

`kernel/drvmod/examples/` 下的示例：

| 模块 | 架构 | 说明 |
|---|---|---|
| `goldfish_rtc.c` → `rtc.a20drv` | riscv64/aarch64/loongarch64 | goldfish RTC，QEMU virt 启动即绑定 |
| `pc_spkr.c` → `pc-spkr.a20drv` | x86_64 | PC speaker，统一驱动核心桥接注册 AUDIO 类 |
| `vinput_probe.c` → `vinput-probe.a20drv` | riscv64/aarch64 | virtio-input 内核只读探针（双驻留共享代码的模块部署），挂键盘时输出设备身份 |
| `vinput.c` → `vinput.a20drv` | 四架构 | virtio-input 完整驱动（状态迁移 + 事件 virtqueue + IRQ + input class 设备）；事件经 mux 的 `/dev/event0` 投递；slot 5 双驻留样本保持 user-owned 归 uinputd |
| `ps2.c` → `ps2.a20drv` | x86_64 | PS/2 键鼠控制器，初始化 + 双向量 ISR 注册；键盘字符经 `uart_receive_char` 进控制台 |
| `nvme.c` → `nvme.a20drv` | x86_64/loongarch64 | 架构无关 PCI 类 block 驱动（标准 `driver_t` 注册）；`DRVMOD_SMOKE=1` 构建携带与内建版本相同的 capability/I/O smoke 测试，`smoke-pci-portability` 在 loongarch64 上验证 |
| `tpm.c` → `tpm.a20drv` | x86_64 | TPM 2.0 TIS/FIFO 驱动，经 `firmware_acpi_tpm2` 做 ACPI 发现；无 TPM 时 probe 优雅返回 |
| `hda.c`+`hda_codec.c` → `hda.a20drv` | 四架构 | 架构无关 PCI 类 audio 驱动（`hda_match` 按 `pci_class_code` 匹配）；`DRVMOD_SMOKE=1` 携带 in-probe 流 smoke（`HDA_STREAM_SMOKE`），`smoke-hda`（x86_64）与 `smoke-pci-portability`（loongarch64）验证 |

验证命令：

```sh
make ARCH=riscv64 ABI=both smoke-drvmod-riscv64      # rtc.a20drv + drvctl + syscall_smoke
make ARCH=x86_64 ABI=both smoke-drvmod-x86_64        # pc-spkr.a20drv
make ARCH=aarch64 ABI=both smoke-drvmod-aarch64      # rtc.a20drv
make ARCH=loongarch64 ABI=both smoke-drvmod-loongarch64  # rtc.a20drv
make smoke-drvmod                                    # 全部四架构
make ARCH=riscv64 ABI=both smoke-dual-input          # vinput-probe.a20drv + 用户态 uinputd
```

这些门禁构建 dev 镜像、在对应架构 QEMU 中验证模块加载、DriverEntry、绑定与 probe。

## 迁移状态与策略

已迁移到 drvmod 的内建驱动：

| 驱动 | 原内建位置 | 模块 | 架构 | 状态 |
|---|---|---|---|---|
| goldfish RTC | `kernel/drvmod/examples/goldfish_rtc.c`（已删除）| `kernel/drvmod/examples/goldfish_rtc.c` | riscv64/aarch64/loongarch64 | 已迁移；QEMU virt 启动即绑定 |
| PC speaker | `kernel/drvmod/examples/pc_spkr.c`（已删除）| `kernel/drvmod/examples/pc_spkr.c` | x86_64 | 已迁移；统一驱动核心桥接注册 AUDIO 类并绑定 platform 设备 |
| virtio-input 内核探针 | `kernel/drvmod/examples/vinput_probe.c`（已删除）| `kernel/drvmod/examples/vinput_probe.c` | riscv64/aarch64 | 已迁移；`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| virtio-input 完整驱动 | `kernel/drvmod/examples/vinput.c`（已删除）| `kernel/drvmod/examples/vinput.c` | 四架构 | 已迁移；`/dev/event0` 的 devfs mux 服务拆分至 `kernel/drivers/input/input_mux.c`（class 设备消费 + EVIOCG* ioctl 面），模块 ISR 经 `input_mux_wake` 唤醒 mux；riscv64 MMIO 与 x86_64 PCI 路径 QEMU 实测事件流（`[INPUT] event type=...` + EV_SYN）|
| PS/2 键鼠控制器 | `kernel/drvmod/examples/ps2.c`（已删除）| `kernel/drvmod/examples/ps2.c` | x86_64 | 已迁移；arch IRQ 分发经 hwapi 投递到模块 ISR，`smoke-drvmod-x86_64` 验证初始化与双向量注册 |
| NVMe | `kernel/drvmod/examples/nvme.c`（已删除）| `kernel/drvmod/examples/nvme.c` | x86_64/loongarch64 | 已迁移；PCI 类标准驱动，`smoke-pci-portability`（loongarch64 dev 镜像 + `DRVMOD_SMOKE=1`）验证绑定与 NVME_CAP/IO_SMOKE |
| TPM 2.0 | `kernel/drvmod/examples/tpm.c`（已删除）| `kernel/drvmod/examples/tpm.c` | x86_64 | 已迁移；ACPI TPM2 发现 + TIS FIFO，无设备时优雅失败 |
| Intel HDA | `kernel/drvmod/examples/hda.c`+`hda_codec.c`（已删除）| `kernel/drvmod/examples/hda.c` | 四架构 | 已迁移；PCI 类 audio 驱动，`smoke-hda` 验证流 smoke + 绑定 |
| virtio-blk | `kernel/drivers/block/virtio_blk.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_blk.c` | 四架构（Early）| 已迁移；早期模块嵌入 `/boot/drivers`，根盘挂载前加载并绑定，解除 bootstrap 循环；generic 根挂载与 swap 统一走 block-class 查询 |
| virtio-scsi | `kernel/drivers/block/virtio_scsi.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_scsi.c` | 四架构（Early）| 已迁移；PCI 1af4:1048/1008，Early DriverStore |
| AHCI | `kernel/drivers/block/ahci.c`（generic 不再内建）| `kernel/drvmod/examples/ahci.c` | x86_64（Early）| 已迁移；PCI 8086:2922/2829，Q35/VBox 根盘路径 |
| DW SDIO | `kernel/drivers/block/dw_sdio.c`（generic 不再内建）| `kernel/drvmod/examples/dw_sdio.c` | riscv64（Early）| 已迁移；platform bus + `hardware_id` 绑定 VF2 SDIO |
| virtio-net | `kernel/drivers/net/virtio_net.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_net.c` | 四架构 | 已迁移；lwIP 桥接符号导出，loongarch64 QEMU 实测绑定 |
| E1000 | `kernel/drivers/net/e1000.c`（generic 不再内建）| `kernel/drvmod/examples/e1000.c` | x86_64 | 已迁移；PCI 8086:100e |
| virtio-gpu | `kernel/drivers/gpu/virtio_gpu.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_gpu.c` | 四架构 | 已迁移；GPU class registry + frame 导出 |
| VMSVGA | `kernel/drivers/gpu/vmsvga.c`（generic 不再内建）| `kernel/drvmod/examples/vmsvga.c` | x86_64 | 已迁移 |
| virtio-snd | `kernel/drivers/audio/virtio_snd.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_snd.c` | 四架构 | 已迁移 |
| xHCI | `kernel/drivers/usb/host/xhci.c`（generic 不再内建）| `kernel/drvmod/examples/xhci.c` | 四架构 | 已迁移；USB core 桥接符号导出 |
| USB HID | `kernel/drivers/usb/class/usb_hid.c`（generic 不再内建）| `kernel/drvmod/examples/usb_hid.c` | 四架构 | 已迁移 |
| USB storage | `kernel/drivers/usb/class/usb_storage.c`（generic 不再内建）| `kernel/drvmod/examples/usb_storage.c` | 四架构 | 已迁移 |
| StarFive/LS2K GMAC | embedded 静态 | 无（板级 platform 驱动）| 板级 | 通过 `platform_bus` + `hardware_id` 绑定，不再无总线通配匹配 |

**当前迁移账本（启动顺序约束）**：只有真正的设备驱动进入 `.a20drv` 迁移表。`loop`、`udisk`、`pty`、`uart`、`framebuffer`/`gpu_core`、`audio_core`、`input_mux` 和 `usb_core` 是内核服务或 class 聚合层，继续静态链接，不应标为 “不可迁移设备驱动”。

generic 不再保留任何内建设备驱动。根盘相关驱动（virtio-blk、virtio-scsi、AHCI、DW SDIO）以及 RTC 全部以 `.a20drv` 嵌入 Early DriverStore（`/boot/drivers`，root ramfs），在挂载真实根盘前加载；其余设备包从 Runtime DriverStore（`/bin/lib/drivers`）加载。板级设备（VF2 的 SDIO/GMAC、LS2K1000 的 GMAC）通过 `platform_bus` + `hardware_id` 注册，驱动按 platform id 绑定；总线无关设备没有通配匹配——无总线设备只有在驱动显式 `match()` 接受时才绑定，UART 串口服务即通过名称匹配发布 char 设备，不再抢占任意板级设备。

**框架 API 现状**：DMA 对象（coherent/aligned/sync）、PCI BAR 访问（`pci_get_bar_resource`/`pci_enable_and_assign_bars`/`pci_intx_irq`/`pci_class_code`）、block/net/input/audio/display class 操作（统一核心桥接 + 头文件）、调度/等待原语（park/wait_queue/mutex）、`firmware_acpi_tpm2`、virtq（双驻留共享层）、`clock_ticks_per_sec` 与 `input_mux_wake` 均为可导出 API；NVMe、TPM、HDA、virtio-input 完整事件投递（`vinput.a20drv` + `input_mux.c`）即以模块形式实现并受 smoke 门禁覆盖。

**其余设备包**：网络、显示、音频、USB、存储 transport 驱动现在与 RTC、输入、NVMe、TPM、HDA 一样由 `tools/driver-modules.mk` 生成 `.a20drv`；PCI/MMIO/platform bus、USB core、framebuffer 和各类 mux/class consumer 仍是内核框架服务。通用 profile 的完整驱动源账本见 `tools/driver-sources.mk` 的 `EMBEDDED_DEVICE_DRIVER_SRCS`（embedded 静态链接全集）。

迁移顺序（每步都必须有等价回归证据）：

1. 把资源、中断、DMA 和 class 操作收敛到框架 API。
2. 用 `.a20drv` 描述符声明设备身份与资源，模块内注册统一 `driver_t`。
3. 为 probe/remove 建立失败回滚和设备引用关系。
4. 用一个真实设备 smoke 验证，再从内建源列表移除该驱动。

因此 generic 不再保留任何内建设备驱动，迁移账本只区分内核服务与设备驱动；embedded 则静态链接完整驱动集。详见[驱动部署 Profile](deployment-profiles.md)。

# 可安装内核驱动

A20OS 现在同时支持三种驱动路径：

| 路径 | 执行位置 | 适用范围 |
|---|---|---|
| `kernel/drivers/` | generic 内核服务、embedded 静态驱动，或被模块包装复用 | 已接入统一 bus/class 模型的共享实现 |
| `drvmod` (`*.a20drv`) | 内核态、内核 direct-map | 运行时可分发的原生驱动模块 |
| 用户态驱动（`*.a20drv`）| 用户态 | 双驻留驱动（hybrid-kernel）的用户侧，见 `docs/hybrid-kernel/04-dual-placement.md` |

`drvmod` 是 A20OS 的可加载内核模块机制：模块是 ELF64 `ET_REL` 可重定位对象，由内核加载进 direct-map，运行时只允许解析驱动框架（`kernel/drvmod/framework.c`）的导出符号白名单。加载器、框架与示例位于 `kernel/drvmod/`，模块 ABI 定义在 `kernel/include/drvmod/drvmod.h`。

原生 `queue.h` 的 request/queue 与 `device_add_execution_action` 通过 framework 导出；generic 模块与 embedded 共用相同的 admission、完成、退休和可失败停止。新增导出未改变既有 device/driver/class 布局，ABI 12 不变；queue ops 自带实际 size。开发流程见[原生请求与队列](request-queues.md)。

## 统一 `.a20drv` 格式与统一驱动管理器

所有可加载驱动文件（内核模块与用户态驱动）都必须使用 `.a20drv` 后缀，并带有只读 ELF `.a20drv` 段。该段的 `a20_driver_descriptor_t` 由 `kernel/include/drivers/driver_descriptor.h` 定义，是**唯一的驱动元数据来源**（没有 `.a20inf` 等旁车清单），包含：

- `magic` 与 `version`：描述符格式身份；
- `placement`：`kernel-module` 或 `user-service`，强边界，不是可互换模式；
- `type`：RTC、BLOCK、INPUT、AUDIO、SECURITY、NET、DISPLAY、USB 或 CHAR；
- `name`：稳定的驱动名；
- `abi` 与 `resource_mask`：驱动接口版本与资源需求（MMIO/IRQ/IOPORT/DMA）；
- `flags`：生命周期所有权（`A20_DRIVER_FLAG_SUPERVISED` 表示用户态服务的 spawn/restart/健康由外部服务监督者如 svcmgr 拥有，内核管理器只记录）；
- `match[]` / `match_count`：该驱动可拥有的设备身份（bus/vendor/device）。

后缀不决定权限域：ELF 类型与 `placement` 共同决定加载器。内核 `drvmod_load()` 只接受 ELF64 `ET_REL` 且 `placement=kernel-module` 的文件；普通 ELF 执行加载器在执行以 `.a20drv` 结尾的文件时只接受 `placement=user-service` 的描述符。因此内核模块不能作为用户程序执行，用户态驱动也不能被映射进内核 direct-map。当前普通 ELF 校验器只接受 RTC、BLOCK、INPUT、AUDIO、SECURITY 五种 user-service type；描述符枚举中的 NET、DISPLAY、USB、CHAR 可供内核模块使用，但用户服务在扩展 `elf_validate_user_driver()` 前会被拒绝。

统一驱动管理器（`kernel/drivers/core/driver_manager.c`）是可选驱动发现与激活的唯一权威。`driver_manager_init()` 在启动期（`init_kthread`）：

1. 注册模块/用户服务绑定的板级设备（goldfish-rtc、ps2、tpm、virtio-input-slot5 等）为统一 `platform_device_t`；
2. 扫描 DriverStore `/bin/lib/drivers` 下所有 `.a20drv`，读取描述符；
3. `kernel-module` 包：`drvmod_load` + `drvmod_init_all`，模块经 `drv_driver_register` 注册统一 `driver_t`，由驱动核心按 `device_t` 匹配并 probe；
4. `user-service` 包：Early 扫描一律延后；Runtime 扫描时，若 `flags` 带 `SUPERVISED` 只记录（svcmgr 负责生命周期），否则仅当描述符匹配的白名单 MMIO 窗口真实存在时调用 `driver_manager_spawn_user`。该自动生成路径要求内核启用 Native ABI，否则返回 `-EOPNOTSUPP`。

每个物理设备只有一个 owner：`device_t.user_owned` 表示设备归用户态驱动所有，驱动核心只允许 `driver_t.read_only_probe` 的内核驱动绑定它（双驻留内核壳），完整初始化归用户驱动。

## 模块接口

驱动 ABI 当前为 12：GPU vtable 末尾新增显式原生 `get_capabilities`；ABI 11 已增加 owning RAM `get_scanout`；ABI 10 已增加 HCD 可失败 `release_endpoint`，ABI 9 已有 `driver_t.quiesce`。所有旧模块必须重建，loader/`drvctl` 会拒绝不匹配版本。模块可使用导出的 `driver_dma_buffer_create/get/put/data/address/size`（显式存储引用，最终 put 不能代替停止 DMA）、`driver_dma_domain_*`、`driver_dma_map_buffer`、`driver_dma_map_vmo`、`driver_dma_map_owned_vmo` 和页级 DMA mapping 和 `driver_fence_*`；provider 模块必须保持驻留直到 domain 释放，完成中断的 producer 必须保留 fence 引用。`device_t`/`driver_t` 本轮不改布局，USB HCD 与 GPU vtable 已扩展；`device_unregister` 已导出并返回 int，`drv_driver_unregister` 传播失败。非零 remove 保留登记/实例/devres，源码调用方须处理错误，直到成功注销才可释放设备或驱动代码；这不表示模块运行期卸载已经可用。USB HID 的既有控制台桥通过导出的只读 `fbcon_active()` 查询当前 scanout 所有者；模块不取得 fbcon 内部状态。PC 模块边界测试实际编译 virtio-gpu、xHCI、HID、hub、storage 等包装器并核对全部未定义符号可解析。详细约束见 [原生驱动运行时](native-runtime.md)。

模块只能解析 `kernel/drvmod/framework.c` 中的导出表。当前导出接口包括：

- 内存与日志：`drv_alloc`、`drv_free`、`drv_log`。
- MMIO：`drv_map_mmio`（返回校验后的 direct-map VA）、`drv_unmap_mmio`、`drv_read32`、`drv_write32`。
- 端口 I/O（x86）：`drv_in8`、`drv_out8`。
- DMA：`drv_dma_alloc_coherent`、`drv_dma_free_coherent`（coherent，稳定设备地址）。该入口不受 mask 约束（drv_env 契约在内核/用户/模块三种放置下逐字节一致，都没有 `device_t`），恒定按 64 位全通分配；持有 `device_t` 的模块驱动必须改用导出的 `dma_set_mask()` + `dma_alloc_coherent(dev, ...)`。
- 延时/时间：`drv_udelay`、`drv_mdelay`、`drv_clock_ticks`。
- 中断：`drv_register_isr`、`drv_unregister_isr`（桥接 hwapi `request_irq`，向量在注册时校验范围，arch IRQ 分发真实投递）。
- 板级 IRQ 资源与内核参数：`platform_device_irq`、`bootargs_get`。前者是模块驱动读自己 IRQ 线号的唯一入口，返回 `>= 0` 表示可用线号、`-ENODEV` 表示板级没发布 `RES_IRQ`（应当轮询，不是错误）；后者读 `a20.*` 内核参数串，配合导出表里已有的 `memcmp` 手工切 token，避免为一次参数解析再引入 `strncmp` 依赖。
`usb_device_poll` 和 `usb_interface_release_endpoints` 另导出给 USB class 模块；后者用于当前 remove，在 class drain 后逐项永久释放 endpoint，错误保留 URB/实例/模块所有权并显式重试。前者的调用者持有协议存储，忙时返回 `-EAGAIN`；不得从 active class 调用直接阻塞等待拓扑锁。

- 统一驱动核心桥接：`drv_driver_register/unregister`、`drv_device_get_resource`、`device_get_resource`、`device_find_get_by_class`、`device_register/unregister`、`device_probe_use_remove`、`device_add_remove_prepare`、`driver_lifecycle_begin/try_begin/end`、`drv_driver_probe_all`、`platform_bus`。`device_register` 是自建实例的驱动必须调用的发布入口：不调用，`device_find_get_by_class()` 就看不到它，class 消费者（例如 `mount_setup_block_device()`）会认为这块盘不存在。
- PCI 类驱动访问器（device_t 为中心，模块以 `bus = &pci_bus` 注册标准 `driver_t` 后使用）：`pci_bus`、`pci_class_code`、`pci_device_id`、`pci_get_bar_resource`、`pci_intx_irq`、`pci_enable_and_assign_bars`。
- VirtIO GPU 实例：驱动使用既有 `driver_dma_buffer_create/data/put` 持有每绑定队列/锁/暂存，实例退出后不留下模块回调；模块边界测试实际编译包装器并核对这些 imports。私有实例不扩展公开布局（当前模块 ABI 为 12）；停止失败保留 allocation，不允许卸载代码，见 [每设备实例](native-runtime.md#virtio-gpu-的每设备实例)。
- VirtIO transport：`virtio_transport_reset` 通过既有 read32/write32 完成有界 status 读回确认，失败不释放 queue/IRQ/存储；不扩展 transport vtable，模块 ABI 沿用当前版本 12，见 [退出协议](native-runtime.md#virtio-reset-确认与-gpu-退出)。
- Resident RAM：新增导出 `vmo_create_resident`，参数/错误、预算 owner 与回滚由原生 MM 定义。只增加函数和内部 kind 5，不改公开 VMO/driver/class 布局或 ABI 12；普通 VMA 自持 backing，页 pin 不保留预算 owner，不授予 DMA 权限，见[resident RAM](native-runtime.md#固定-resident-ram-vmo-与预算-owner)。
- RAM scanout：`gpu_device_scanout_get`、`gpu_scanout_put`、`vmo_create_contiguous`、`vmo_contiguous_info`、`vmo_ref`、`vmo_release` 已加入白名单。snapshot 和普通 VMA 自持 MM 存储，不需要在最终 release 时调用设备模块；不授予 DMA 权限或证明硬件停止，见 [连续 RAM VMO](native-runtime.md#连续-ram-vmo-与-owning-scanout)。
- 原生绑定停止：`class_device_binding_stopped` 记录旧 publication 的显式成功 remove，`device_release_resource_action` 在串行 probe/lifecycle 中精确撤销一个局部回收动作；核心私有凭据不扩展公开布局，ABI 沿用当前版本 12。离线不是停止，详见 [原生停止凭据](native-runtime.md#原生-publication-的绑定停止凭据)。
- DRM adapter：文件固定 publication，active call 内取得 ops；失败清理保留旧资源身份，旧绑定停止后只进入内核 MM 回收，不调用旧模块。`drm_create_vfile_for` 是内核 adapter 接口，未增加模块导出或 ABI 版本；这不代表运行期模块卸载已完成，见 [DRM 退休](native-runtime.md#drm-文件命名空间与失败退休)。
- GPU 原生能力：`gpu_device_capabilities_get` 在既有 access 内复制角色，失败清零，不取得存储/硬件权限；provider 只复制缓存状态。GPU provider 必须实现 ABI 12 的尾部回调，kernel guard 与 drvctl 拒绝旧 ABI 11，所有包需重建。见 [原生能力与默认策略](native-runtime.md#原生-gpu-能力与默认选择)。
- GPU publication：`gpu_device_default_get`、`gpu_device_access_begin`、`gpu_device_default_begin`、`gpu_device_access_end`。前者返回需 put 的 class 引用，begin/end 拥有独立引用与 active call；不增加第二套驱动模型；当前全局 ABI 12 的 GPU vtable 扩展见上一项。完整契约见 [GPU 调用](native-runtime.md#gpu-publication-与作用域调用)。
- 控制台输入路径（PS/2 模块）：`uart_receive_char`。
- 内核熵池（硬件 RNG 模块）：`random_reseed`。内核已有 `kernel/core/random.c`（xoshiro 状态 + `random_u64`/`random_fill`，消费方是 `sys_getrandom()`、ASLR 与栈保护），硬件熵设备应当把读到的字节折进这个池而不是另起一套。该符号只能从进程上下文调用：`arch_entropy_sample()` 会取 `proc_current()` 与 `frame_free_count()`，在 ISR 里调它等于在中断里读调度器状态。
- 调度/等待原语（模块完成路径）：`proc_park_prepare/commit/cancel/finish`、`proc_wake_q_init/flush`、`wait_queue_init/link/unlink/collect_one/collect_all`、`mutex_init/lock/unlock`、`timer_get_ticks`、`mdelay/udelay`。
- 日志与分配器：`klog_write/klog_level`、`kmalloc/kfree/kcalloc`。模块内日志建议走 `drv_log`：`klog_level` 是外部变量，经 GOT 加载（loongarch64 的 GOT 支持已实现，但模块日志用纯 CALL36 更稳）。
- DMA 增强与地址 mask：`dma_alloc_coherent_aligned/free_coherent_aligned`、`dma_sync_for_cpu/device`、`dma_set_mask`、`dma_addr_ok`、`dma_range_ok`。分配器按 `dev->dma_mask` 校验，取不到满足窗口的内存时返回 `NULL`，由 probe 决定失败；语义见[运行时契约](runtime-contracts.md#dma-api)。
- ACPI 发现（x86_64）：`firmware_acpi_tpm2`。
- 输入 mux 唤醒与 virtio PCI 传输：`input_mux_wake`、`pci_virtio_transport_init`、`arch_current_cpu_id`。
- 基础字符串/内存函数：`strncpy`、`memset`、`memcpy`、`memcmp`、`strcmp`、`strlen`、`strstr`，`snprintf`/`vsnprintf`（需要格式化到定长缓冲的驱动，例如拼设备名），以及自旋锁内联用到的 `proc_current`/`proc_task_pid`/`printf`/`panic`。

「导出表缺一个符号」的后果不是那个符号用不了，而是**整个模块加载失败**：加载器报 `unresolved symbol 'X'` 后放弃该 `.a20drv`，而 transport 随之整个消失。所以在 aarch64 上（virtio-blk 是加载模块）少导出 `snprintf` 或 `device_register`，现象是两块盘都不出现、`/bin` 挂不上、`init` 直接 panic；在 ppc64le 上（virtio-blk 内建）同一份代码完全正常。**同一个缺陷只在其中一种 deployment profile 下显形**，改完务必按两种 profile 各验一次。

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
3. 读缓冲与 shadow 均从 frame 池（`pfa_alloc`）分配，而非 kmalloc：kmalloc slab 与模块页在同一 buddy 上取页，重叠时 `kfree` 会把模块自己的页还给分配器，后续 DMA/其他模块分配会覆盖已加载模块的 GOT/rodata（loongarch64 实测踩过）。分配连续物理页，以最终运行地址（direct-map 窗口）计算重定位，在 shadow 上修补，校验 `DriverEntry` 位于 `.text`。
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

镜像中的 DriverStore 位于 FAT32 的 `/lib/drivers`，由于开发镜像把 FAT32 挂载到 A20OS 的 `/bin`，运行时路径是 `/bin/lib/drivers`。内核模块和用户态驱动包都放在这里，由统一驱动管理器在启动时按描述符发现与激活。`drvctl` 提供暂存管理：

```text
drvctl install MODULE NAME
drvctl remove NAME
drvctl list
```

`drvctl install` 校验包内 `.a20drv` 描述符（唯一的元数据，没有 `.a20inf` manifest），把包复制为 `/bin/lib/drivers/NAME.a20drv`，并打印其 placement 与 type；`drvctl list` 从每个包的描述符读出 `placement/type/match`。暂存后由统一驱动管理器在下次启动激活；签名验证和运行时 unload 是下一阶段工作，不能把暂存成功误报为已经运行。

## 示例与验证

`kernel/drvmod/examples/` 中的包入口及历史示例如下。NVMe、PS/2、R8169只保留描述符和入口，
实际实现位于 `kernel/drivers/<class>/`，embedded与模块共用。
`make check-pc-drivers` 实际编译这些模块并检查导出依赖；这不代替模块加载和硬件验收。

| 模块 | 架构 | 说明 |
|---|---|---|
| `goldfish_rtc.c` → `rtc.a20drv` | riscv64/aarch64/loongarch64 | goldfish RTC，QEMU virt 启动即绑定 |
| `pc_spkr.c` → `pc-spkr.a20drv` | x86_64 | PC speaker，统一驱动核心桥接注册 AUDIO 类 |
| `cmos_rtc.c` → `cmos-rtc.a20drv` | x86_64 | MC146818 CMOS RTC，x86_64 墙钟来源；板级 platform 设备给 IOPORT 0x70/0x71；只读、无 IRQ、不做时区换算，century 寄存器不可信时按 20xx 处理 |
| `vinput_probe.c` → `vinput-probe.a20drv` | riscv64/aarch64 | virtio-input 内核只读探针（双驻留共享代码的模块部署），挂键盘时输出设备身份 |
| `vinput.c` → `vinput.a20drv` | 四架构 | virtio-input 完整驱动（状态迁移 + 事件 virtqueue + IRQ + input class 设备）；事件经 mux 的 `/dev/event0` 投递；slot 5 双驻留样本保持 user-owned 归 uinputd |
| `ps2.c` → `ps2.a20drv` | x86_64 | PS/2 键鼠控制器，初始化 + 双向量 ISR 注册；键盘字符经 `uart_receive_char` 进控制台 |
| `nvme.c` → `nvme.a20drv` | x86_64/loongarch64 | 架构无关 PCI 类 block 驱动（标准 `driver_t` 注册）；`DRVMOD_SMOKE=1` 构建携带与内建版本相同的 capability/I/O smoke 测试，`smoke-pci-portability` 在 loongarch64 上验证 |
| `r8169.c` → `r8169.a20drv` | x86_64 | Realtek 10ec:8168模块包装；共享NET实现，其他硅片变体需单独验收 |
| `tpm.c` → `tpm.a20drv` | x86_64 | TPM 2.0 TIS/FIFO 驱动，经 `firmware_acpi_tpm2` 做 ACPI 发现；无 TPM 时 probe 优雅返回 |
| `hda.c` → `hda.a20drv` | 四架构 | 架构无关 PCI 类 audio 驱动（`hda_match` 按 `pci_class_code` 匹配）；`DRVMOD_SMOKE=1` 携带 in-probe 流 smoke（`HDA_STREAM_SMOKE`），`smoke-hda`（x86_64）与 `smoke-pci-portability`（loongarch64）验证；原 `hda_codec.c` 已并入本模块 |
| `virtio_console.c` → `virtio-console.a20drv` | riscv64/x86_64/aarch64/loongarch64 | virtio-serial port 0 作为 CHAR 类 `/dev/vport0`；只协商 `VIRTIO_F_VERSION_1`、不协商 MULTIPORT，q0=rx/q1=tx 承载唯一端口；rx 中断驱动、tx 同步有界等待 |
| `virtio_rng.c` → `virtio-rng.a20drv` | riscv64/x86_64 | virtio-rng 熵源，CHAR 类 `/dev/hwrng`，读到的字节经新导出的 `random_reseed` 折进内核熵池；双 transport（PCI `1af4:1044`/`1005` 与 virtio-mmio device-id 4）；缓冲按需挂载，避免 probe 时持续抽宿主熵 |
| `rtl8139.c` → `rtl8139.a20drv` | x86_64 | Realtek RTL8139 PCI 网卡（`10ec:8139` + 以太网控制器 class）；此前无任何驱动认领该型号；默认中断驱动，`a20.rtl8139.poll=1` 强制轮询 |

验证命令：

```sh
make ARCH=riscv64 ABI=both smoke-drvmod-riscv64      # rtc.a20drv + drvctl + syscall_smoke
make ARCH=x86_64 ABI=both smoke-drvmod-x86_64        # pc-spkr.a20drv
make ARCH=aarch64 ABI=both smoke-drvmod-aarch64      # rtc.a20drv
make ARCH=loongarch64 ABI=both smoke-drvmod-loongarch64  # rtc.a20drv
make smoke-drvmod                                    # 全部四架构
make ARCH=riscv64 ABI=both smoke-dual-input          # vinput-probe.a20drv + 用户态 uinputd
make smoke-rtc-cmos                                  # cmos-rtc.a20drv + x86_64 墙钟对宿主时间
make smoke-rtc-cmos-fallback                         # CMOS 越界时的回退路径（尚未执行过）
make smoke-virtio-console                            # virtio-console.a20drv + /dev/vport0 双向回环
make smoke-virtio-rng                                # virtio-rng.a20drv + /dev/hwrng 读出真熵
make smoke-net-rtl8139                               # rtl8139.a20drv + hostfwd 端到端回环与中断计数
```

`smoke-rtc-cmos` 额外做了一件其它门禁不做的事：门禁内的 `expect` 只能证明来客打出了某一行，无法证明那行里的时间是真实时间，所以它把 `/bin/date` 的输出交给宿主侧的 `tools/check_rtc_wallclock.py` 复核（≥ 2026-01-01，与宿主相差 ≤ 300s）。

这些门禁构建 dev 镜像、在对应架构 QEMU 中验证模块加载、DriverEntry、绑定与 probe。

## 迁移状态与策略

已迁移到 drvmod 的内建驱动：

| 驱动 | 原内建位置 | 模块 | 架构 | 状态 |
|---|---|---|---|---|
| goldfish RTC | `kernel/drivers/char/goldfish_rtc_kdrv.c`（已删除）| `kernel/drvmod/examples/goldfish_rtc.c` | riscv64/aarch64/loongarch64 | 已迁移；QEMU virt 启动即绑定 |
| PC speaker | `kernel/drivers/audio/pc_speaker.c`（已删除）| `kernel/drvmod/examples/pc_spkr.c` | x86_64 | 已迁移；统一驱动核心桥接注册 AUDIO 类并绑定 platform 设备 |
| CMOS RTC (MC146818) | 无内建位置（新增）| `kernel/drvmod/examples/cmos_rtc.c` | x86_64（Early）| 新增；x86_64 墙钟来源，绑定两块 x86_64 板级声明的 platform 设备；限制见 [实现状态](../../status/current/drivers-implementation-status.md) |
| virtio-input 内核探针 | `kernel/drivers/input/virtio_input_kprobe.c`（已删除）| `kernel/drvmod/examples/vinput_probe.c` | riscv64/aarch64 | 已迁移；`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| virtio-input 完整驱动 | `kernel/drivers/input/virtio_input.c`（已删除）| `kernel/drvmod/examples/vinput.c` | 四架构 | 已迁移；`/dev/event0` 的 devfs mux 服务拆分至 `kernel/drivers/input/input_mux.c`（class 设备消费 + EVIOCG* ioctl 面），模块 ISR 经 `input_mux_wake` 唤醒 mux；riscv64 MMIO 与 x86_64 PCI 路径 QEMU 实测事件流（`[INPUT] event type=...` + EV_SYN）|
| PS/2 键鼠控制器 | `kernel/drivers/input/ps2.c`（共享实现）| `kernel/drvmod/examples/ps2.c` | x86_64 | 已迁移；arch IRQ 分发经 hwapi 投递到模块 ISR，`smoke-drvmod-x86_64` 验证初始化与双向量注册 |
| NVMe | `kernel/drivers/block/nvme.c`（共享实现）| `kernel/drvmod/examples/nvme.c` | x86_64/loongarch64 | 已迁移；PCI 类标准驱动，`smoke-pci-portability`（loongarch64 dev 镜像 + `DRVMOD_SMOKE=1`）验证绑定与 NVME_CAP/IO_SMOKE |
| TPM 2.0 | `kernel/drivers/security/tpm.c`（已删除）| `kernel/drvmod/examples/tpm.c` | x86_64 | 已迁移；ACPI TPM2 发现 + TIS FIFO，无设备时优雅失败 |
| Intel HDA | `kernel/drivers/audio/hda_codec.c`（已删除）| `kernel/drvmod/examples/hda.c` | 四架构 | 已迁移；PCI 类 audio 驱动，`smoke-hda` 验证流 smoke + 绑定 |
| virtio-blk | `kernel/drivers/block/virtio_blk.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_blk.c` | 四架构（Early）| 已迁移；早期模块嵌入 `/boot/drivers`，根盘挂载前加载并绑定，解除 bootstrap 循环；generic 根挂载与 swap 统一走 block-class 查询 |
| virtio-scsi | `kernel/drivers/block/virtio_scsi.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_scsi.c` | 四架构（Early）| 已迁移；PCI 1af4:1048/1008，Early DriverStore |
| AHCI | `kernel/drivers/block/ahci.c`（generic 不再内建）| `kernel/drvmod/examples/ahci.c` | x86_64（Early）| 已迁移；PCI 8086:2922/2829，Q35/VBox 根盘路径 |
| DW SDIO | `kernel/drivers/block/dw_sdio.c`（generic 不再内建）| `kernel/drvmod/examples/dw_sdio.c` | riscv64（Early）| 已迁移；platform bus + `hardware_id` 绑定 VF2 SDIO |
| virtio-net | `kernel/drivers/net/virtio_net.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_net.c` | riscv64/aarch64/loongarch64 | 已迁移；lwIP 桥接符号导出，loongarch64 QEMU 实测绑定；x86_64 generic 清单当前未列出 |
| E1000 | `kernel/drivers/net/e1000.c`（generic 不再内建）| `kernel/drvmod/examples/e1000.c` | x86_64 | 已迁移；PCI 8086:100e |
| virtio-gpu | `kernel/drivers/gpu/virtio_gpu.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_gpu.c` | 四架构 | 已迁移；GPU class 与 owning RAM scanout；x86_64 generic 的双核 2D 门禁实际加载 virtio-gpu 模块 |
| VMSVGA | `kernel/drivers/gpu/vmsvga.c`（generic 不再内建）| `kernel/drvmod/examples/vmsvga.c` | x86_64 | 已迁移 |
| virtio-snd | `kernel/drivers/audio/virtio_snd.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_snd.c` | riscv64/aarch64/loongarch64 | 已迁移；x86_64 generic 清单当前未列出 |
| xHCI | `kernel/drivers/usb/host/xhci.c`（generic 不再内建）| `kernel/drvmod/examples/xhci.c` | x86_64/aarch64/loongarch64 | 已迁移；USB core 桥接符号导出；riscv64 generic 清单当前未列出 |
| USB HID | `kernel/drivers/usb/class/usb_hid.c`（generic 不再内建）| `kernel/drvmod/examples/usb_hid.c` | x86_64/aarch64/loongarch64 | 已迁移；riscv64 generic 清单当前未列出 |
| USB storage | `kernel/drivers/usb/class/usb_storage.c`（generic 不再内建）| `kernel/drvmod/examples/usb_storage.c` | x86_64/aarch64/loongarch64 | 已迁移；riscv64 generic 清单当前未列出 |
| virtio-console | `kernel/drivers/char/virtio_console.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_console.c` | riscv64/x86_64/aarch64/loongarch64 | 新增；PCI 1af4:1043/1003，CHAR 类 `/dev/vport0`（`device_set_devfs_name()`），rx 中断 / tx 同步，默认把接收字节镜像进控制台输入。**single-port、无 MULTIPORT、单静态实例、无 MSI-X**；门禁 `smoke-virtio-console` 已实跑 PASS |
| StarFive/LS2K GMAC | embedded 静态 | 无（板级 platform 驱动）| 板级 | 通过 `platform_bus` + `hardware_id` 绑定，不再无总线通配匹配 |
| virtio-rng | `kernel/drivers/char/virtio_rng.c`（generic 不再内建）| `kernel/drvmod/examples/virtio_rng.c` | riscv64/x86_64 | 新增；PCI 1af4:1044/1005 与 virtio-mmio（device-id 4）双 transport，CHAR 类 `/dev/hwrng`（`device_set_devfs_name()`），读到的字节经新导出的 `random_reseed` 折进 `kernel/core/random.c`。**单静态实例、缓冲按需挂载、熵池只在 read 路径喂、无 MSI-X**；门禁 `smoke-virtio-rng` 已实跑 PASS |

当前迁移账本受启动顺序约束：只有真正的设备驱动进入 `.a20drv` 迁移表。`loop`、`udisk`、`pty`、`uart`、`framebuffer`/`gpu_core`、`audio_core`、`input_mux` 和 `usb_core` 是内核服务或 class 聚合层，继续静态链接，不应标为 “不可迁移设备驱动”。

generic 不再保留 `EMBEDDED_DEVICE_DRIVER_SRCS` 中的内建设备驱动。Early DriverStore 按架构生成：x86_64 包含 PC speaker、CMOS RTC、virtio-blk、virtio-scsi、AHCI；riscv64/aarch64/loongarch64 包含 RTC、virtio-blk、virtio-scsi，riscv64 另含 DW SDIO。其余被该架构 `DRVMOD_MODULES` 列出的包进入 Runtime DriverStore（`/bin/lib/drivers`）。VF2 与 LS2K1000 的 GMAC 虽通过 `platform_bus` + `hardware_id` 注册绑定，但当前没有 generic `.a20drv` 包，只能由 embedded 静态账本部署。总线无关设备没有通配匹配：无总线设备只有在驱动显式 `match()` 接受时才绑定，UART 串口服务即通过名称匹配发布 char 设备，不再抢占任意板级设备。

框架 API 现状：DMA 对象（coherent/aligned/sync）与地址 mask（`dma_set_mask`/`dma_addr_ok`/`dma_range_ok`）、PCI BAR 访问（`pci_get_bar_resource`/`pci_enable_and_assign_bars`/`pci_intx_irq`/`pci_class_code`）、block/net/input/audio/display class 操作（统一核心桥接 + 头文件）、调度/等待原语（park/wait_queue/mutex）、`firmware_acpi_tpm2`、`random_reseed`、virtq（双驻留共享层）、`clock_ticks_per_sec` 与 `input_mux_wake` 均为可导出 API；NVMe、TPM、HDA、virtio-input 完整事件投递（`vinput.a20drv` + `input_mux.c`）与 virtio-rng（`virtio-rng.a20drv` → `/dev/hwrng`）即以模块形式实现并受 smoke 门禁覆盖。

其余设备包：`tools/mk/driver-modules.mk` 的每架构 `DRVMOD_MODULES` 是 generic 包的权威清单，不能从 `kernel/drvmod/examples/` 中存在某个包装文件推断所有架构都会部署它。PCI/MMIO/platform bus、USB core、framebuffer 和各类 mux/class consumer 仍是内核框架服务。`tools/mk/driver-sources.mk` 的 `EMBEDDED_DEVICE_DRIVER_SRCS` 只表示 embedded 当前显式静态集合，不是 generic 模块清单或两种 profile 的能力并集。

迁移顺序（每步都必须有等价回归证据）：

1. 把资源、中断、DMA 和 class 操作收敛到框架 API。
2. 用 `.a20drv` 描述符声明设备身份与资源，模块内注册统一 `driver_t`。
3. 为 probe/remove 建立失败回滚和设备引用关系。
4. 用一个真实设备 smoke 验证，再从内建源列表移除该驱动。

因此 generic 的内核只保留显式服务账本，设备能力取决于目标架构实际列出的包；embedded 只静态链接 `EMBEDDED_DEVICE_DRIVER_SRCS` 当前列出的共享驱动，不与 generic 模块清单自动保持能力对等。RTC、PC speaker、PS/2、NVMe、TPM、HDA、vinput 及 vinput-probe 等 module-only 实现当前不在 embedded 静态账本。详见[驱动部署 Profile](deployment-profiles.md)。

`driver_dma_map_owned_vmo` 为固定 kind 4/5 的 RAM 显式保留整个 VMO/预算 owner，成功与失败清理句柄同样持有；完整回滚不发布 owner。buffer/VMO 最终 put 在 domain 锁外，try_unmap 的临时 domain 引用跨 owner 回调；构造回滚依靠 caller 全程持有的 domain 引用。内部 adopt helper 不导出，公开布局与 ABI 12 不变；旧 map_vmo 的裸帧快照语义保持不变。详见 [VMO owner](native-runtime.md#持有-vmo-的-dma-视图与锁外-owner-退休)。

原生 `driver_fence_watch_create` / `driver_fence_watch_try_remove` 已加入模块导出；公开结构与 ABI 12 不变。登记仅持 fence，不替模块保留 callback 代码或上下文。IRQ-safe callback 只能执行短操作；最终 watch 回收在任务上下文，RUNNING 返回 EBUSY 时必须保留模块/实例供重试。成功 remove 后还须停止/排空它已调度的 work；这不使模块运行期卸载自动可用。见 [一次性通知](native-runtime.md#一次性通知与显式撤销)。

## 组合协议与模块扩展

新模块通过 device_add_protocol 在自己的 probe 中登记独立版本/大小/能力接口，原生 endpoint 复用原有 publication。框架复制描述符，模块必须保证 ops 及其代码在绑定退出前有效；成功 access 与原有 active call 使用同一个排空计数。offline 后不能取得旧表，消费者保留的元数据不阻止模块在正常解绑后交还代码。

device_add_protocol、device_endpoint_get、driver_endpoint_get/put、driver_endpoint_protocol_count/info、driver_protocol_begin/end 及 class_net_read_stats 已导出。新增扩展不修改 device_t/driver_t 或既有 net 表，保持 ABI 12；公开对象布局或传统表布局的未来变更仍须遵循 ABI 检查。VirtIO-net 与 RTL8139 的真实模块包装器已作为导入边界验证对象，统计不再依赖内建弱符号。完整接口和开发流程见[原生协议](core-model.md#原生协议组合能力而非扩大全局-vtable)。

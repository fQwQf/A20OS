# 可安装内核驱动

A20OS 现在同时支持三种驱动路径：

| 路径 | 执行位置 | 适用范围 |
|---|---|---|
| `kernel/drivers/` | 内核镜像内 | 已接入统一 bus/class 模型的内建驱动 |
| `drvmod` (`*.drv`) | 内核态、内核 direct-map | 运行时可分发的原生驱动模块 |
| 用户态服务（`user/svc/*d`） | 用户态 | 双驻留驱动（hybrid-kernel）的用户侧，见 `docs/hybrid-kernel/04-dual-placement.md` |

`drvmod` 是 A20OS 的可加载内核模块机制：模块是 ELF64 `ET_REL` 可重定位对象，由内核加载进 direct-map，运行时只允许解析驱动框架（`kernel/drvmod/framework.c`）的导出符号白名单。加载器、框架与示例位于 `kernel/drvmod/`，模块 ABI 定义在 `kernel/include/drvmod/drvmod.h`。

## 模块接口

模块只能解析 `kernel/drvmod/framework.c` 中的导出表。当前导出接口包括：

- 内存与日志：`drv_alloc`、`drv_free`、`drv_log`。
- MMIO：`drv_map_mmio`、`drv_unmap_mmio`、`drv_read32`、`drv_write32`。
- 端口 I/O（x86）：`drv_in8`、`drv_out8`。
- DMA：`drv_dma_alloc_coherent`、`drv_dma_free_coherent`（coherent，稳定设备地址）。
- 延时/时间：`drv_udelay`、`drv_mdelay`、`drv_clock_ticks`。
- 中断：`drv_register_isr`、`drv_unregister_isr`（桥接 hwapi `request_irq`，向量在注册时校验范围，arch IRQ 分发真实投递）。
- 设备生命周期：`drv_device_register`、`drv_device_unregister`。
- 统一驱动核心桥接：`drv_driver_register/unregister`、`drv_device_register_core/unregister_core`、`drv_device_get_resource`、`drv_driver_probe_all`、`device_find_by_class`、`platform_bus`。
- PCI 类驱动访问器（device_t 为中心，模块以 `bus = &pci_bus` 注册标准 `driver_t` 后使用）：`pci_bus`、`pci_class_code`、`pci_device_id`、`pci_get_bar_resource`、`pci_intx_irq`、`pci_enable_and_assign_bars`。
- 控制台输入路径（PS/2 模块）：`uart_receive_char`。
- 基础字符串/内存函数：`strncpy`、`memset`、`memcpy`、`memcmp`、`strcmp`、`strlen`、`strstr`，以及自旋锁内联用到的 `proc_current`/`proc_task_pid`/`printf`。

模块不得直接引用普通内核符号。未解析符号、未知重定位、非法 ELF 类型、越界的 section/symbol/strtab/relocation 都会在加载前拒绝，不会修改内核页表或堆。

## 模块编写模型

模块入口约定（注册 + 生命周期回调）：

```c
uintptr_t DriverEntry(drv_driver_t **out)
{
    static drv_driver_t drv;
    drv.name = "my-driver";
    drv.match_count = 1;
    drv.match[0].bus = 3;                 /* 0=fixed, 1=PCI, 2=USB, 3=mmio */
    drv.match[0].vendor = 0x101000;       /* mmio 设备编码基地址 */
    drv.match[0].device = 0;
    drv.probe = my_probe;                 /* per-device init */
    drv.remove = my_remove;               /* per-device teardown */
    *out = &drv;
    return 0;
}
```

`DriverEntry` 只发布驱动描述，**不得**在入口里访问硬件。设备绑定由内核的自动匹配阶段完成（见下）。

另一条路径是**统一驱动核心桥接**：模块把标准 `driver_t`（`probe/remove/class_ops`，与内建驱动完全相同的模型）经 `drv_driver_register` 注册进内核驱动核心，device 匹配、class 发布、devfs 节点全部走既有路径；模块内存里的回调由核心直接调用。示例见 `kernel/drvmod/examples/pc_spkr.c`。

## 双驻留共享代码（drv_env）

`kernel/include/drivers/dual/drv_env.h` 为同一设备协议提供三种部署后端：`DRV_ENV_KERNEL`（内建内核驱动）、`DRV_ENV_USER`（用户态服务）、`DRV_ENV_DRVMOD`（模块）。模块用第三种后端的共享头（如 `virtio_input.h`）与用户态 `uinputd` 共享同一份协议源码，见 `kernel/drvmod/examples/vinput_probe.c`。

## 构建约束

模块按架构交叉编译为 ELF `ET_REL`，Makefile 的 `DRVMOD_CFLAGS` 按架构选择代码模型，使外部调用重定位没有（或只有 QEMU 尺寸内的）PC 相对范围限制：

| 架构 | 编译器 | 代码模型 | 外部调用重定位 |
|---|---|---|---|
| riscv64 | `riscv64-unknown-elf-gcc` | `-mcmodel=medany -march=rv64g -mno-relax` | `R_RISCV_CALL`（±2 GiB） |
| x86_64 | `x86_64-linux-gnu-gcc` | `-mcmodel=large -mno-red-zone -fno-pie` | `R_X86_64_64`（绝对地址，无范围限制） |
| aarch64 | `aarch64-linux-gnu-gcc` | `-mcmodel=large -fno-pic` | `R_AARCH64_CALL26`（±128 MiB，超范围时 loader 自动生成 veneer 跳板） |
| loongarch64 | `loongarch64-linux-gnu-gcc` | `-mcmodel=medium -fno-pic` | `R_LARCH_CALL36`（±2 GiB） |

加载器按 `e_machine` 在运行时选择重定位解码器，四种架构共用同一份边界检查、符号白名单和生命周期。`rv64g` 是有意选择的：禁用 `C` 扩展可避免把 16 位压缩指令误当作 32 位指令修补。模块必须小于 `DRV_MOD_MAX_SIZE`，并由连续物理页承载，确保 direct-map 地址连续。

LoongArch64 注意：工具链对局部地址引用使用 `pcalau12i + addi.d`（页相对）而非 `pcaddi`（字节相对），loader 按指令 opcode 自动区分；复杂 64 位 PCALA 链（`lu32i.d`/`lu52i.d` 与 ADD/SUB 表达式重定位）只出现在被丢弃的 `.eh_frame` 中。AArch64 超出 ±128 MiB 的外部 `BL` 会被 loader 改写为模块尾部的 veneer（`ldr x16,[pc,#8]; br x16; .quad S`），因此 kernel 文本与模块窗口的距离不再限制模块。

## 加载与绑定生命周期

1. 内核注册硬件设备资源：`drv_device_register()` 把 `bus/vendor/device` 身份、MMIO 范围、IRQ 放入框架设备表。
2. `drvmod_load()` 读取并验证 ELF（文件级、section 级、symbol/strtab、relocation 偏移全量边界检查），建立 shadow 布局。
3. 分配连续物理页，以最终运行地址（direct-map 窗口）计算重定位，在 shadow 上修补，校验 `DriverEntry` 位于 `.text`。
4. 复制到 direct-map，执行 `fence.i`/ICache 同步。
5. `drvmod_init_all()` 调用每个模块的 `DriverEntry`。
6. `drvmod_bind_all()` 把每个模块的 `match[]` 表与框架设备表匹配，命中后调用 `probe(dev)`；probe 成功则设备绑定该模块。
7. 未绑定模块可以卸载；已经绑定设备或返回有效 `drv_driver_t` 的模块返回 `-EBUSY`，直到设备 remove 生命周期接入完整。

启动日志顺序（riscv64 QEMU virt）：

```text
[INIT] init_kthread started (pid=1)
[DRVMOD] loaded 'rtc.drv' id=0 ...
[GOLDFISH-RTC] probe ok: epoch=...
```

驱动模块是 ring 0 原生代码。签名、manifest 资源约束和框架 API 白名单降低准入风险，但不能隔离驱动崩溃；模块 bug 仍可能导致 kernel panic。

## DriverStore 与安装

镜像中的 DriverStore 位于 FAT32 的 `/lib/drivers`，由于开发镜像把 FAT32 挂载到 A20OS 的 `/bin`，运行时路径是 `/bin/lib/drivers`。`drvctl` 提供暂存管理：

```text
drvctl install MODULE MANIFEST NAME
drvctl remove NAME
drvctl list
```

manifest 使用当前的 line-oriented MVP 格式，至少包含：

```text
name=goldfish-rtc
module=goldfish-rtc
bus=mmio
match=0x101000:0x100
```

`drvctl install` 会校验必需字段，并把模块复制为 `/bin/lib/drivers/NAME.drv`，把 manifest 复制为 `NAME.a20inf`。当前命令完成的是持久化暂存；内核 `init_kthread` 会在下次启动时扫描 DriverStore 中所有 `*.drv` 并逐个加载、绑定，因此 `drvctl install` 的效果在重启后生效。签名验证和运行时 unload 是下一阶段工作，不能把暂存成功误报为已经运行。

## 示例与验证

`kernel/drvmod/examples/` 下的示例：

| 模块 | 架构 | 说明 |
|---|---|---|
| `goldfish_rtc.c` → `rtc.drv` | riscv64/aarch64/loongarch64 | goldfish RTC，QEMU virt 启动即绑定 |
| `pc_spkr.c` → `pc-spkr.drv` | x86_64 | PC speaker，统一驱动核心桥接注册 AUDIO 类 |
| `vinput_probe.c` → `vinput-probe.drv` | riscv64/aarch64 | virtio-input 内核探针（双驻留共享代码的模块部署），挂键盘时输出设备身份 |
| `ps2.c` → `ps2.drv` | x86_64 | PS/2 键鼠控制器，初始化 + 双向量 ISR 注册；键盘字符经 `uart_receive_char` 进控制台 |

验证命令：

```sh
make ARCH=riscv64 ABI=both smoke-drvmod-riscv64      # rtc.drv + drvctl + syscall_smoke
make ARCH=x86_64 ABI=both smoke-drvmod-x86_64        # pc-spkr.drv
make ARCH=aarch64 ABI=both smoke-drvmod-aarch64      # rtc.drv
make ARCH=loongarch64 ABI=both smoke-drvmod-loongarch64  # rtc.drv
make smoke-drvmod                                    # 全部四架构
make ARCH=riscv64 ABI=both smoke-dual-input          # vinput-probe.drv + 用户态 uinputd
```

这些门禁构建 dev 镜像、在对应架构 QEMU 中验证模块加载、DriverEntry、绑定与 probe。

## 迁移状态与策略

已迁移到 drvmod 的内建驱动：

| 驱动 | 原内建位置 | 模块 | 架构 | 状态 |
|---|---|---|---|---|
| goldfish RTC | `kernel/drivers/char/goldfish_rtc_kdrv.c`（已删除） | `kernel/drvmod/examples/goldfish_rtc.c` | riscv64/aarch64/loongarch64 | 已迁移；QEMU virt 启动即绑定 |
| PC speaker | `kernel/drivers/audio/pc_speaker.c`（已删除） | `kernel/drvmod/examples/pc_spkr.c` | x86_64 | 已迁移；统一驱动核心桥接注册 AUDIO 类并绑定 platform 设备 |
| virtio-input 内核探针 | `kernel/drivers/input/virtio_input_kprobe.c`（已删除） | `kernel/drvmod/examples/vinput_probe.c` | riscv64/aarch64 | 已迁移；`smoke-dual-input` 验证与用户态 uinputd 读到同一设备身份 |
| PS/2 键鼠控制器 | `kernel/drivers/input/ps2.c`（已删除） | `kernel/drvmod/examples/ps2.c` | x86_64 | 已迁移；arch IRQ 分发经 hwapi 投递到模块 ISR，`smoke-drvmod-x86_64` 验证初始化与双向量注册 |

**无法迁移（启动顺序约束）**：这些驱动在模块加载（`init_kthread`）之前就必须工作，不能改为后期加载：

- UART：第一个串口字符在 boot 早期输出。
- virtio-blk：根文件系统（FAT32 `/bin`）挂载发生在 init_kthread 之前，而模块本身正是从该文件系统读取的（除非引入 initramfs）。
- virtio-net / net_init：网络初始化在 init_kthread 之前。
- virtio-gpu、USB core、PCI bus、virtio-mmio bus：早期枚举。

**当前不可迁移（框架 API 缺口）**：这些驱动依赖框架尚未导出的能力，迁移前必须先收敛依赖：

- DMA/PCI BAR/class ops：NVMe、AHCI、HDA、E1000、virtio-blk/net/scsi、vmsvga、virtio-gpu —— 需要框架新增 DMA 对象、PCI BAR 访问与 block/net/display/audio class 操作导出。
- VirtIO 队列：virtio-input 的完整事件投递 —— 需要 virtq 进入共享层并作为框架 API（当前共享层只覆盖配置空间与只读探针）。
- ACPI/板级发现：TPM（ACPI TPM2 表）、GMAC/SDIO（板级单实例轮询）。
- 完整事件投递的输入路径（virtio-input event queue）与板级服务（TPM 的 ACPI 发现、GMAC/SDIO 的单实例轮询）——需要对应子系统 API 进入框架导出。

迁移顺序（每步都必须有等价回归证据）：

1. 把资源、中断、DMA 和 class 操作收敛到框架 API。
2. 把 bus identity 与 manifest match 分离。
3. 为 probe/remove 建立失败回滚和设备引用关系。
4. 用一个真实设备 smoke 验证，再从内建源列表移除该驱动。

因此当前 `drvmod` 与内建 `kernel/drivers` 是并存阶段，不会在没有等价回归证据时批量删除现有 GPU/网络/块驱动。

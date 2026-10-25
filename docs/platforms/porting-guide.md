# A20OS 平台移植指南

本页是开发者接口参考，不声明某个设备的支持状态或部署方法。已有架构上的新开发板主要通过 `kernel/arch/<arch>/boards/<board>/` 接入；板级地址和拓扑由平台提供，架构入口通过明确的板级头文件和 hook 接收启动参数。

## 代码边界

`kernel/arch/<arch>/` 负责指令集机制：异常入口、寄存器访问、页表切换、secondary trampoline 和读取硬件 CPU ID。`kernel/arch/<arch>/boards/<board>/` 负责板级事实：RAM/MMIO 布局、CPU 拓扑、固件启动方法、中断控制器实例、设备资源和关机方式。可复用设备协议放在 `kernel/drivers/`，调度、online 状态和启动编排放在 `kernel/core/` 与 `kernel/proc/`。

新板不得通过 `CONFIG_BOARD_*` 在 `kernel/arch/<arch>/platform/smp.c` 中增加启动分支。若架构缺少一种通用指令机制，应先增加架构 helper，再由 platform hook 选择和提供参数。

## 构建接入

创建 `kernel/arch/<arch>/boards/<board>/board.c` 并定义唯一的：

```c
const board_config_t *const current_board;
```

构建系统自动加入该目录顶层的 `*.c`，并把该目录加入头文件搜索路径。平台若提供 `kernel/arch/<arch>/boards/<board>/ldscript.ld`，它会覆盖架构默认链接脚本。平台子目录和平台汇编不会被自动递归加入；需要它们时必须先扩展 Makefile 的明确规则。

实例接入使用 `instances/<board>.toml` 声明架构、板卡、构建参数和传输方式；板级约束可由 `kernel/arch/<arch>/boards/<board>/board.mk` 提供。实例能解析不代表启动链已完成。`tools/a20 build` 需要有效编译规则；`run` 需要真实可用的仿真机器配置；`prepare`、`package`、`deploy`、`flash` 各自需要对应的固件准备、镜像、介质或编程器实现。新增打包类型仍需接入工具分派和校验，不能只增加 TOML 字段。

架构入口会使用 `board_boot.h`、`board_console.h` 等可选板级头文件。添加板配置时必须核对 Image header、链接地址、初始映射、UART 协议和固件参数是否真正被启动代码采用；不能只看 `board.c` 中的设备表。

若目标属于新的构建档位，在 `components/trim.toml` 中调整 `[profile.mcu].arches` 或 `[capability.*]` 矩阵，再执行 `make regen-trim-fragment` 和 `make check-trim-registry`。该注册表描述裁剪能力，不代替引脚、时钟和内存布局。

`board_config_t` 当前包含 RAM 描述、irqchip、timer、SMP、early init、电源控制和设备枚举。新增字段必须确认存在真实调用方，不能只增加未接通的操作表。

## SMP 接口

支持多核的平台设置 `.smp`：

```c
static const smp_platform_ops_t board_smp_ops = {
    .discover = board_smp_discover,
    .start = board_smp_start,
    .send_ipi = board_smp_send_ipi,
    .secondary_init = board_smp_secondary_init,
};
```

`discover` 填充不超过 `capacity` 个 `smp_cpu_desc_t`，并返回实际填充数。描述符中的 `hw_id` 是 MPIDR、hart ID、APIC ID 或 CoreID；`logical_id` 由 core 重新整理为从 0 开始的连续编号。必须包含 boot CPU，硬件 ID 不得重复。真实开发板应从 DT、ACPI 或固件枚举；只有硬件模型固定的仿真平台才可按构建容量生成拓扑。

`start` 接收 secondary 入口物理地址和 `logical_context`。平台必须通过 PSCI context、SBI HSM opaque、AP trampoline 参数或对应 mailbox 把该逻辑编号交给 secondary。返回 0 只表示启动请求已接受；core 会等待 CPU 发布 online。失败应返回非零，不得永久等待。

`send_ipi` 根据描述符中的硬件 ID 发送 `SMP_IPI_RESCHEDULE`。发送前必须保证唤醒相关内存写入对目标 CPU 可见。接收路径先清中断或 EOI，再决定是否从用户态触发调度，不能从持锁的任意内核上下文直接切换任务。

`secondary_init` 只初始化当前 CPU 的板级控制器状态，例如 GIC CPU interface、PLIC context 或 IOCSR IPI enable。共享 lifecycle 的顺序是：

```text
proc_init_secondary
-> platform secondary_init
-> trap_init
-> timer_init
-> publish online
-> enable local IRQ
-> idle_loop
```

不支持 secondary 启动的平台不要提供伪造的空 `smp_platform_ops`。保持 `.smp = NULL`，并按平台实际约束拒绝多核构建或明确报告 BSP-only，不能伪报 secondary online。

## 中断与定时器

板级 irqchip 负责设备 IRQ 的 enable、disable、ack 和 eoi。调度 IPI 只通过 `smp_platform_ops.send_ipi`，不属于设备 irqchip 操作表。硬件协议可复用时，放到 `kernel/drivers/` 下对应子目录，例如已有的 `kernel/drivers/irqchip/plic.c`；board 提供基址、IRQ 和 affinity 数据。

每个 secondary 都必须有可用的本地 timer。`timer_get_ticks()` 在 CPU 迁移前后必须单调，频率必须与 `ARCH_TIMER_FREQ` 契约一致；不同 CPU 的本地 timer 配置不能只依赖 BSP 的一次性寄存器初始化。

## 设备与内存

固定设备由 platform 构造 `device_t` 和 `resource_t`；PCI、VirtIO MMIO 等总线优先调用已有枚举器。可复用驱动不得读取 `CONFIG_BOARD_*`，也不得硬编码某块板的 IRQ 或 MMIO 地址。

`board_config_t.ram_base/ram_end` 目前不是内存分配器的唯一来源。新板仍需核对架构启动页表、`platform.h` 和链接脚本中的 RAM 映射，直到内存布局也完全改为运行时平台描述。

## Bring-up 顺序

1. 使用 `BRINGUP=1 NR_CPUS=1` 验证链接地址、串口、异常和 timer。
2. 验证 irqchip 和块设备，进入完整用户态 shell。
3. 设置 `NR_CPUS=2`，确认 secondary online、timer IRQ 和 reschedule IPI。
4. 扩展到目标 CPU 数，运行 `smp_bench`、`sched_stress` 和 `proc_stress`。
5. 对缺失 CPU、启动失败和错误固件表进行测试，系统应降级而不是卡死。

示例命令：

```sh
make ARCH=<arch> BOARD=<board> ABI=both BRINGUP=1 NR_CPUS=1 kernel-only
make ARCH=<arch> BOARD=<board> ABI=linux BRINGUP=0 NR_CPUS=2 \
    ALLOW_UNVERIFIED_SMP=1 dev-build
make check-smp-platform-boundary
make check-arch-boundary
make check-concurrency-foundation
```

新平台在完成运行时多核验证前，需要显式设置 `ALLOW_UNVERIFIED_SMP=1`。不要仅凭成功链接就把平台加入已验证列表。

## 平台操作文档

每个目标的单平台文档应能独立使用，包含：

1. 适用型号、RAM/固件变种、当前已实现设备，以及虚拟机和实机分别达到的支持状态。
2. 主机系统、GNU 工具链、Rust 版本/targets/组件、Python 与固件工具依赖，以及仓库初始化命令。
3. 完整的构建与打包入口、实际输出路径、固件来源、启动协议及地址约束。
4. 串口接线、速率、电平、介质准备、启动步骤、日志位置、可观察的成功和失败标记。
5. 恢复方法、缺失驱动与操作限制；尚不可部署时，明确列出阻断和当前可运行的构建/检查入口。

正文记录操作系统的现状与可重复操作，不记录工作过程、某位开发者的主机路径、逐日进度或一次性跑测叙述。源码链接和通用指南是补充材料，不能代替本页的必要命令。运行日志留在构建输出，动态产物哈希由实例账本生成；平台页只说明如何获得和判断它们。

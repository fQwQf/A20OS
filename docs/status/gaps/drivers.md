# DRIVER 缺口

对应 V1-DRIVER。现状见 [current/drivers-implementation-status.md](../current/drivers-implementation-status.md)。

### 原生 GPU 与 AMD-Vi 基础设施

通用运行时、资源占用和 RISC-V IOMMU provider 的实现及历史验证见 [原生驱动运行时](../../drivers/guide/native-runtime.md)；本机目标与上游复用方案见 [真机 GPU 开发](../../graphics/real-hardware-gpu.md)。工作队列和固件读取已提供，不能再整体记为未实现；厂商固件部署/校验/上传仍属后续工作。RISC-V 原生/用户 domain 已在 DC 发布前使用共享 requester 登记并保留失败占用；其他未参与的 provider、raw 配置和跨驱动归属变更仍需协调，这不关闭下列 AMD 硬件接管项。

| 待完成范围 | 关闭条件 |
|---|---|
| AMD-Vi 接管前资源完整性 | 已有被动 PCI 唯一绑定、IVRS 保留、完整 aperture checked claim，以及 probe 失败/解绑/重绑的主机与 QEMU 证据。已通过独立存储保留对象导入受管 RAM 与 ACPI 表页。已有 PCI 桥路径/AMD Range 查询，以及组合 IVRS requester/alias、PCI 路径和控制器上下文租约的保留路由对象。已有完整固件声明集合查询、逐成员发布/路径/归属/BME 检查与原子占用，成员失效后继续保留。仍须补齐 PFA 以外 RAM、其他固件/DT 保留来源，通过各所属驱动停止全部 alias peer 并协调归属变更，落实实际 AMD domain 的硬件权限/Unity/Exclusion 及停止/失败保留（已有覆盖全部声明成员的独立 IVMD 约束和通用映射入口检查），并补物理平台证据；被动绑定和只读快照均不授予硬件启用权限 |
| AMD-Vi 翻译与故障处理 | 已有 IVMD → 独立 policy 与 managed 映射入口预检；仍需为实际 requester/alias 在硬件中落实权限、Unity 和排除范围，验证页表、失效完成、真实 DMA 读写与非法 IOVA fault；停止/失效超时继续保留硬件可访问页面与上下文，并有可重试关闭证据。现有 RISC-V 测试不能关闭此项 |
| 失败后的硬件回收 | 核心已在成功 probe 后的 remove/发布清理失败时保留绑定和 devres，已接入 PCI、已发布 USB 端口和已知部分 slot 的枚举/发布失败；xHCI 自身 start/probe 已接入 consumer 解绑前的可失败准备；HID/hub 独立退出已有可失败 endpoint 交还，Configure Endpoint ring 在命令前登记并在错误时保留；xHCI 同步 bulk 已改为原生 DMA 暂存对象，错误不交还硬件仍可能使用的暂存区且不保留调用者地址；仍需 storage 静态实例、BOT 恢复、变长 bulk/多 TRB 与其他 HCD 的协议；USB 拓扑已用原生生命周期协调，xHCI 锁外回调已保留并阻止端口退役期间复用 slot；仍需厂商驱动真实停止/失效失败证据、旧驱动接入已有显式失败 probe 保留协议，以及可失败 关机/kexec shutdown。不能以回调返回 0 或 metadata 离线替代硬件停止证明 |
| GPU 消费者与映射退役 | CPU 物理映射安装/失败回滚已共用 MM/Rust 事务，权限 ceiling 与 Device 属性保留已接入；该物理视图仍借用 backing；有 get_scanout 的 RAM fbdev 分支已由连续 VMO 与普通 VMA 保留存储，支持 fork/split，模块 ABI 11。fbdev 单次调用与 sysfs GPU 快照已有同绑定 class access，默认选择保留元数据且旧 publication 不跟随重绑；DRM 已固定文件 publication，隔离 GEM/CRTC/EDID/事件并在 scoped access 内调用，未知 resource/context 清理保留到成功或旧绑定停止，导出 pin/VMA 保留 CPU 存储；仍须补每文件 handle 权限、厂商 BO/context/请求/fence 与旧 get_fb 持久物理 VMA 的所有权和退休协议。virtio-gpu 内部暂存/IRQ 已要求 reset 读回确认，失败 probe 和退出保留对象，单静态实例拒绝被其他设备覆盖；仍需每设备队列与外部 GEM backing 的提交/完成寿命。一次 class active call 不保留调用返回的存储，也不能证明 GPU 已停止；默认选择的旧 virgl 函数存在性仍不是通用 GPU 能力模型 |
| GPU 内存与请求模型 | 已有独立 DMA buffer 及显式 domain 映射，失效失败保留整块分配；原生 VMO 已通过原子 canonical 页快照接入 mapper，源 resize/销毁不提前回收所映射帧，但不转移 charge 或代替 extent/驻留/访问租约；RISC-V native provider 借用该所有权；尚不代替 BO/GPUVM/驻留；仍须建立每设备 BO、GPUVM、context、提交与 fence 引用，验证进程退出、并发提交、超时/复位时请求完成和资源生命周期；现有通用 fence 不等于 DRM 同步 UAPI 已接通 |
| Phoenix 原生执行与呈现 | 固定上游/固件版本后，依次取得真实 SDMA copy/fill 回读、Mesa 离屏渲染及显示接管证据；分别记录平台、设备身份与失败行为，不以 GOP、virtio-gpu 或软件渲染结果替代 |

以上为后续验收条件，不表示已经实现或已经在本机验证。

### DRIVER-08 控制台 UART 的接收不是真正中断驱动

- 现状：8250 在 `arch_uart_init()`（`kernel/arch/riscv64/include/console.h`）里写了 `IER = 0x01`（RX 中断使能），`uart_init()` 对 `UART0_IRQ`（QEMU virt 为 10）注册了 `uart_irq_wrapper`，`kernel/arch/riscv64/trap/irqchip.c` 也在 PLIC 里按 hart enable 了该源。但实测行为是「只有读者轮询才取字节」：前台跑 `ping`（不读终端）时按 ^C，收端行规一次 ISIG 都没有触发；等 `ping` 结束、shell 重新读终端时那个 `0x03` 才被取走。也就是说接收实际靠 `uart_getc()` 里的 `arch_uart_poll_getc()` 推进，RX 中断没有在起作用（是 `uart_handle_irq` 未被调用，还是 trap 分发没有把外部中断转进来，尚未定位）。
- 缺口：控制台输入在「没人读」的时候不被处理，于是 echo/ISIG 这类收端行规事件只能发生在有读者的瞬间——提示符上按 ^C 有效，前台作业不读终端时按 ^C 无效。这与 [drivers/guide/device-classes.md](../../drivers/guide/device-classes.md) 写明的契约（"UART 硬件驱动只负责收发和中断"）不符，属于驱动未完成项，而不是 tty 层的缺陷。
- 关闭条件：在没有任务读控制台的情况下，RX 字节仍进入环形缓冲区且 `rx_irq_bytes` 在没有 poll 的情况下增长；由此前台不读终端的作业也能被 ^C 打断（SIGINT 投递到该终端的前台进程组）；有一条门禁覆盖（例如前台跑 `ping`/`sleep` 时注入 ^C，断言作业在 2 秒内结束），并在 QEMU 与至少一块真机上通过。
- 定位进展（2026-10-24）：trap/claim 路径已被排除——`smoke-proc-stress` 的 boot 日志出现`[IRQDBG] first S-ext claim irq_id=1`（virtio-blk 源经同一 claim/complete/trap 路径成功分发），而同一日志中 `uart_irq_wrapper` 的一次性探针从未触发；smoke 经串口注入的 `proc_stress`/`poweroff`由 `uart_getc()` 的轮询路径（`rx_polls_rx_bytes`）送达。即 RX 中断从未触发，根因在 16550 → PLIC 源 10 的投递段（IER 已写 0x01、MCR OUT2 已置、PLIC priority/enable 均已配置）。下一轮实验建议用 `qemu -d int` 抓 external interrupt 投递，或在校验 `arch_uart_init()` 之后 IER 是否被运行期清零。两个一次性 `[IRQDBG]` 探针保留在 `arch_handle_irq`/`uart_irq_wrapper`。
- 来源：2026-10 实测（阻塞点记录见 [abi-ipc.md](abi-ipc.md) ABI-08）、[drivers/guide/device-classes.md](../../drivers/guide/device-classes.md)、`kernel/arch/riscv64/include/console.h`、`kernel/arch/riscv64/trap/irqchip.c`、`kernel/drivers/char/uart.c`。
### DRIVER-09 STM32F103 平台文件含硬编码 MMIO 地址

- 现状：`shared_bus.c` 的 20 余处 `MMIO32(0x40…)` 已改为 `board_config.h` 里的命名寄存器宏（地址集中在该板的 memory map，平台 C 源不再含硬编码地址），`rg` 静态断言已通过，`shared_bus.c` 用本地解包的 `arm-none-eabi-gcc 14.2` 能为 `thumbv7m-none-eabi` 编译。`make check-stm32f103` 仍为红，但现在停在 `tools/mk/rust-kernel.mk:3971` 的 ARMv7-M Rust 目标断言（DRIVER-10，delivery-owned），而不是本条的地址违规。
- 缺口：门禁要等 DRIVER-10 修复后才能跑完整构建；本机没有系统级 `arm-none-eabi-gcc`，CI 容器才有完整工具链。
- 关闭条件：DRIVER-10 修复后 `check-stm32f103` 通过。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。

### DRIVER-10 ARMv7-M 档的内核 Rust 目标不完整

- 现状：`make stm32f103-bringup`（以及 `stm32f103-xuanwu`）在解析期失败，报 `tools/mk/rust-kernel.mk:3971: the Rust module has no kernel Rust target for ARCH=armv7m`。此前已修掉两处确定缺口（`RUST_WAIT4`、`RUST_WAIT_TIMER` 的 armv7m 条目——MCU profile 含 `kernel/proc/wait.c` 与 `kernel/proc/timer_heap.c`，两个对象实测能为 `thumbv7m-none-eabi` 编译），但门禁仍在 `RUST_VFS_LIFECYCLE` 处停下：它的目标继承自缺 armv7m 的 `RUST_EXT4_EXTENT_TXN`，而 VFS lifecycle 本身并不在 MCU 的源码表里。这类断言的写法是不带条件的 `$(error)`，所以对不在本次构建里的模块也会触发；`mm/mremap.c` 那一处是按 `KERNEL_SRC` 加了条件的，其余多数没有。
- 缺口：ARMv7-M 的内核固件无法构建，MCU 档没有构建证据；需要区分"MCU profile 真的要用这个 Rust 模块"（补 armv7m 目标）与"这个模块不在 MCU profile 里"（把断言按 `KERNEL_SRC` 加条件）。
- 关闭条件：`make stm32f103-bringup` 与 `make stm32f103-xuanwu` 编译通过，且 `check-stm32f103` 通过。
- 来源：同上。

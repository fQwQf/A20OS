# 混合内核核心机制参考

本文档以描述性方式说明 A20OS 混合内核各核心机制的语义、契约与代码位置。设计总览见 [00-design.md](00-design.md)，演进方向见 [02-mainstream-plan.md](02-mainstream-plan.md)。

## IPC：`channel_call` 融合 RPC

`channel_call`（syscall `0x0508`，`kernel/abi/native/sys_native_ipc.c`）在一次陷入内完成"发送请求 + 等待回复"：

- 单次句柄查找（READ|WRITE）与单次参数校验；
- 请求入队后若对端存在阻塞的 RECV 等待者，走 priority-preempt 唤醒（两个 ABI 共享）；
- SDK 封装为 `a20_channel_call[_flags]`（`user/liba20rt/a20_channel.h`）。

契约：

- 回复阶段复用 `recv_begin_donate`（UP 下含捐赠语义）；
- 句柄随请求/回复传递，接收权限按 `ρ_recv = ρ_send ∩ ρ_transfer` 收敛；
- 对端关闭返回 `CANCELED`；`NONBLOCK` 语义与普通 send/recv 一致；
- 发送方必须用 NULL 句柄数组表示"无句柄"（空数组被视为非法参数）。

## 时间片捐赠（同步 IPC 快路径）

`proc_park_commit_donate`（`kernel/proc/park.c`）在调用者因等待回复而阻塞时，若目标任务已完全 PARKED 且位于同一 CPU，则以 dispatch 引用直接 `context_switch`，绕过 runqueue 插入与选取，把剩余时间片交给被调者。

语义与限制：

- 捐赠深度恒为 1（捐赠者已 BLOCKED，不可再运行）；
- 跨 CPU、目标未停驻等不满足条件时回退普通调度路径，行为与常规 park/wake 完全一致；
- **仅限 UP**（`CONFIG_NR_CPUS == 1`）：SMP 下捐赠需要跨核唤醒簿记（`kernel/proc/current.c` 的 `PER_CPU_CURRENT_VALIDATION`），当前禁用，SMP 一律走验证过的普通 park/wake 路径；
- send 阶段的唤醒必须延迟（`a20_channel_send_dwc` 的 `defer_wake`），捐赠不可行时由 `recv_begin_donate` 补发延迟唤醒。

## 服务监管协议（svcmgr）

`user/svc/svcmgr.c` 是系统监管者（`svcman.c` 是最小自愈演示，见 [00-design.md](00-design.md) 的命名说明）：

- **清单与依赖**：声明式清单（`rtcd`、`echod` 服务名、路径、依赖顺序）拉起服务；
- **端点传递**：`task_spawn` v2 的 `target_slot` 把服务端点安装到子进程固定槽位（服务以编译期常量命名自己的端点），无需全局注册表；
- **崩溃检测**：EventQ watch 服务 TASK handle 的 `A20_EVENT_EXITED`，`ev.data0` 即退出码；
- **健康探针**：周期性 ping（默认 2s 周期、1.5s 超时），超时强杀；pong 通道随服务重启重新注册；
- **重启策略**：指数退避重启，flap 预算（5 次/30s）防止崩溃风暴；重启 = 新建 channel 对 + 重新 spawn + 重新 watch，旧端点随对端关闭退役；
- **重绑**：注册表按名解析返回当前端点；服务死亡后查找返回 `NOT_FOUND`，重启后客户端自动重绑。

## 服务注册表

`registry_claim`（syscall `0x0A03`，`kernel/abi/native/registry.c`）提供按名注册与解析：

- 引导期创建一对 well-known channel：服务端由 svcmgr 经 `sys_a20_registry_claim` claim，客户端端点安装进每个 native 任务的 `start_info.service_registry`（同样可由 Linux 程序经 `SYS_a20_registry_client` 取得为 fd）；
- 客户端按名解析得到服务端点；服务崩溃后注册条目失效，重启后重新 claim，客户端下次解析即重绑；
- claim 采用 CAS（`g_registry_owner_pid`），主人存活时返回 `ACCESS`；任务退出路径 `a20_registry_task_exit` 释放 claim 以便后继接管；
- 解析失败返回 `NOT_FOUND`（区别于"存在但忙"）。

## 共享内存 SPSC 环协议

`user/liba20rt/a20_shmring.h` 定义跨进程数据面协议：

- 全相对偏移布局（跨进程不同虚拟地址可用）；
- acquire/release 游标（单调递增，差值为可用/已用字节）；
- Dekker 门铃：`c_sleep`/`p_sleep` futex 字，以物理页为 key（`pt_translate`），跨进程有效；
- 非满非空路径零 syscall；字宽拷贝（freestanding 构建无 memcpy，字节循环在模拟器上慢一个数量级）；
- 容量为 2 的幂，数据区紧随 64 字节头部。

## 用户态驱动契约

用户态驱动文件统一命名为 `*.a20drv`（与内核模块同一制品格式），但仍是普通可执行 ELF，而不是内核模块。每个文件必须定义 `.a20drv` 描述段，声明 `placement=user-service`、设备类型、稳定驱动名和可拥有的设备身份；执行加载器会校验该段。`rtcd-<arch>.a20drv`、`ubd-<arch>.a20drv` 与 `uinputd-<arch>.a20drv` 是当前用户态驱动。

统一驱动管理器（`kernel/drivers/core/driver_manager.c`）从 DriverStore `/bin/lib/drivers` 读取用户服务包的描述符，并在其设备真实存在时直接生成该用户进程（`driver_manager_spawn_user`）；描述符声明 `SUPERVISED` 的包（如 rtcd）生命周期归服务监督者 svcmgr，管理器只记录。每个设备只有一个 owner：用户拥有的窗口（`udriver_mmio_user_owned`）只允许内核 read-only 探针绑定。完整的跨权限域 `.a20drv` 格式见 [`kernel-modules.md`](../drivers/guide/kernel-modules.md)。

`kernel/drivers/core/udriver.c` + syscall `0x0C00–0x0C09`（处理在 `kernel/abi/native/sys_native_device.c`）：

- **MMIO 授权**：`device_map_mmio` 只允许映射白名单内的设备物理窗口（窗口表按板静态注册），任务无法映射任意内存或其他设备；`user_owned` 窗口在映射前强制当前任务先 `device_claim`；
- **所有权仲裁**：`device_claim/release`（`0x0C07/0x0C08`）按窗口记录 owner pid，同一设备只有一个用户 owner；`udriver_task_cleanup` 在任务退出时自动释放 claim 与 IRQ 绑定，监管者重启的新驱动不会撞上旧注册；
- **IRQ 交付**：`device_irq_listen` 把物理 IRQ 绑定到 EventQ——内核先屏蔽该线（电平中断防风暴），再投递 `A20_EVENT_SIGNALED`；驱动处理完调用 `device_irq_ack` 重新武装（VFIO/UIO 电平协议）；
- **生命周期**：`device_irq_unlisten` 与任务退出清理（`udriver_task_cleanup`）在 EXITED 事件发出之前释放 IRQ 注册，保证监管者重启的新驱动不会撞上旧注册；
- **DMA 模型**：用户驱动不允许提供任意物理地址；DMA 缓冲只能来自内核分配的 VMO（`vm_create_object` + pin），物理连续性由内核作为契约保证，virtqueue 描述符物理地址由内核翻译后交给驱动。`device_vmo_phys`（`0x0C04`）只对已物化页返回物理地址（peek 语义，未触页报 pa=0），驱动必须先物化再翻译；`device_alloc_dma`（`0x0C09`）提供预物化的连续 DMA heap（上限 64 页，帧清零）；
- **块设备代理**（udisk）：内核块代理 + 16 槽共享环 + 门铃通道（`device_block_attach/complete`，`0x0C05/0x0C06`），零拷贝数据（`data_pa` 直写页缓存）；驱动死亡时在飞请求以 `-EIO` 失败并唤醒等待者，实例存活，重挂载恢复。

## 统一驱动框架与 drvmod

- **DriverStore**：`/boot/drivers`（early，不含用户服务）与 `/bin/lib/drivers`（runtime，含用户服务包）两个普通目录，由 `kernel/drivers/core/driver_manager.c` 扫描 `.a20drv` 描述段；
- **描述段**：`.a20drv` 段（`A20_DRIVER_DESCRIPTOR_MAGIC "A20D"`，版本 2）声明 `placement`（内核模块 / 用户服务）、设备类型、稳定驱动名、匹配表与标志；`SUPERVISED` 标志的用户包生命周期归 svcmgr，管理器只记录；
- **统一 `driver_t`**：`driver_core.c` 的模块模型——`match/probe/remove` + class 设备发布；`read_only_probe` 允许内核探针绑定 user-owned 设备（一个设备只有一个 owner，破坏性初始化归用户壳）；
- **class 设备**：char/block/net/input/display/audio 统一注册（major 240/8/0/13/29/116），命名 `char0`/`disk0`/`event0` 等；`input_mux.c` 提供传输无关的 `/dev/event0` evdev 服务；
- **drvmod 装载**：`kernel/drvmod/loader.c` 把内核态驱动模块（ET_REL）装入内核直接映射，解析重定位、生成 aarch64/riscv64 veneer 与 loongarch64 GOT，`DriverEntry()` 返回 0 后 pin 该模块；~80 个导出符号（MMIO/IRQ/DMA/park/wake/pfa/PCI/lwIP 桥）是模块唯一 API 面。

## vDSO

`kernel/vdso/riscv64/vdso.S` 提供 `__vdso_clock_gettime`/`__vdso_gettimeofday`/`__vdso_getcpu`（不支持时回退 `ecall`）：

- exec 时映射到固定 VA（代码 RX `0x3FFC0000`，数据 RO `0x3FFC2000`）+ auxv `AT_SYSINFO_EHDR`；VMA 为 `VM_PFNMAP|VM_DONTFORK`，fork 时经 `vdso_fork_map` 显式重映射；
- vvar 数据页与内核 timekeeping 读同一个 time CSR，realtime 锚点与 syscall 路径共用同一个 tick（位级一致），seqlock 保护；
- `scounteren.TM` 在每核 `timer_init` 打开；
- 内核嵌入 `.elf` 文件本身（`objcopy -O binary` 会剥掉 ELF 头导致 musl 解析器失效）；
- 目前仅 riscv64 有 vDSO；loongarch64 的 vDSO 移植（`rdtime.d` stable counter）为后续工作。

## IOMMU 硬件 DMA 隔离

`kernel/drivers/core/riscv_iommu.c` 实现 RISC-V IOMMU（QEMU `riscv-iommu-pci`）的真实硬件初始化：

- BAR/capability 解析（version 16，spec 1.0），分配 DDT(1LVL)/CQ/FQ 并编程使能（CQON/FQON/DDTP 完成）；
- devid 1（IOMMU 自身）保持 passthrough DC；devid 0 配置 SV39 翻译域（3 级页表），经 TR_REQ 验证已映射 IOVA 精确翻译、未映射 IOVA 被硬件拒绝（fault=1, cause=13）；
- fault 队列消费与 per-device 页表动态映射为后续工作；`smoke-iommu-discovery` 断言全部状态。

## 对象统计与配额

- **计数器**（`kernel/include/ipc/objstats.h`）：全局原子计数 `handles / channel_eps / eventqs / vmos / vmo_pages / irq_bindings / vfiles`（七项实时对象计数），覆盖安装/移除/销毁全部路径，只读暴露在 `/proc/a20/objects`；另有累计计数 `vmo_dirty_frames`（buddy 复用未清零帧的合法信号，永不下降，不属于泄漏基线）；
- **句柄配额**：每任务 native 句柄硬上限 4096（`A20_HT_DEFAULT_QUOTA`），三个安装入口统一以 `NO_SPACE` 拒绝超额；
- 崩溃/重启循环后七项实时计数器必须回归基线（泄漏审计）。

## Linux ABI 共享的机制

- **唤醒快路径**：pipe/AF_UNIX/futex/channel 的 wake 统一汇入 `wait_queue_wake_*` → `proc_wake_q_flush` → `proc_try_wake_locked_common`，priority-preempt 分支对两个 ABI 自动生效；
- **AF_UNIX 桥接**：socketpair/connect/accept 数据面建立在内部 channel 上（SCM_RIGHTS 走 stream 合并且回退句柄传递）；
- **channel fd 表面**：`kernel/ipc/channel_fd.c` 把 `a20_channel_ep_t` 包装为 vfile——`read()/write()` 各自映射一条 channel 消息（SOCK_SEQPACKET 语义），对端关闭 `read()` 返回 0（EOF）；`sys_a20_channel_pair`（Linux nr 900）与 `sys_a20_registry_client`（nr 901）让 Linux 程序直接使用 Native 的 channel/注册表机制（`smoke-a20-channel`）；
- **fd 化等待对象**：`eventfd`/`signalfd`/`timerfd`（`kernel/ipc/`）与 SysV `semget/semctl/semtimedop/semop`、`shmget/shmctl/shmat/shmdt` 都是 ABI 无关的 vfile/对象后端，Linux syscall 只是其上的薄翻译（见 `kernel/abi/linux/sys_eventfd_timerfd.c` 等）；
- **vfile 对象审计**：fdtable 与 fd-backed handle 共享同一 `vfiles` 计数（`a20_eventq_on_vfile_destroy` 在 fd 拆除时清理以 fd 为 key 的 watch）。

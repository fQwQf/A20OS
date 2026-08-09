# 混合内核：能力与边界清单

本文档以清单形式说明 A20OS 混合内核**当前具备的能力**与**已知边界**。设计总览见 [00-design.md](00-design.md)，机制语义见 [01-mechanisms.md](01-mechanisms.md)，演进方向见 [02-mainstream-plan.md](02-mainstream-plan.md)，**当前生效的改造路线见 [03-refactor-plan.md](03-refactor-plan.md)**（Native ABI 为研究本体、数据面/控制面分离、可移动边界），双态部署驱动框架见 [04-dual-placement.md](04-dual-placement.md)。

## 核心架构原则（已贯彻）

```
用户态 ── syscall 线格式 ──> ABI 层（薄包装）── 内部 API ──> 内部实现
```

- 内部实现（`kernel/ipc`、`kernel/mm`、`kernel/proc`、`kernel/drivers`、`kernel/include/core`）独立自包含，对 `abi/` 零依赖（全仓审计）；
- Linux ABI 线格式常量由 `kernel/include/core/` 自持，`abi/linux/*` 再导出；
- 内部 IPC 子系统头位于 `kernel/include/ipc/`；
- 任何 ABI 只需薄包装即可共享内部混合内核机制。

## 能力清单

| 能力 | 状态 | 验证方式 |
|------|------|----------|
| IPC 融合快路径（`channel_call` + 时间片捐赠） | 已实现（捐赠仅 UP） | `smoke-native-ipc` |
| 共享内存 SPSC 环数据面 | 已实现 | 16MiB 跨进程完整性，`smoke-native-shmring` |
| 服务监管者（清单 + 依赖 + 崩溃重启） | 已实现 | 崩溃自愈 + 重启预算，`smoke-native-svc` |
| 服务注册表 + 按名解析 + 崩溃重绑 | 已实现 | 独立进程解析、崩溃后重绑，`smoke-native-registry` |
| 健康探针（ping + 超时强杀） | 已实现 | pong 通道随重启重注册 |
| 资源硬隔离（句柄配额 + 对象计数审计） | 已实现 | 崩溃循环计数零漂移，`smoke-native-isolation`，`/proc/a20/objects` |
| 用户态驱动框架（MMIO 授权 + IRQ→EventQ） | 已实现 | goldfish RTC 整体用户态化，`smoke-native-rtcd` |
| virtio-blk 用户态驱动（零拷贝 DMA） | 已实现 | FAT32 挂载 `/ubd`，`smoke-native-ubd` |
| 驱动崩溃恢复（在飞请求失败传导 + 重挂载） | 已实现 | `ubd_recover` |
| Linux ABI 透明获益（vDSO、唤醒快路径、AF_UNIX 桥接） | 已实现 | `smoke-clock-vdso` + unix 测试 |
| TCP/IP 协议栈（内核态 lwIP 单一实现） | 已实现 | lwIP 源码位于 `kernel/external/lwip`，由内核 `kernel/net` 编译；`smoke-network-suite` |
| 统一等待对象层（futex/EventQ/channel/pipe/socket/mutex 共用 `wait_queue_t`） | 已实现 | futex 迁移到共享 `wait_queue_t`（谓词匹配/requeue/purge），`smoke-futex-stress`、`smoke-sched-stress` |
| Channel IPC 从 Linux ABI 消费（fd 表面） | 已实现 | `SYS_a20_channel_pair`(900)/`SYS_a20_registry_client`(901)：Linux 程序经 read/write 使用同一 channel 机制与服务注册表，`smoke-a20-channel` |
| 统一对象计量（fd/vfile 纳入对象审计） | 已实现 | `/proc/a20/objects` 增加 `vfiles` 计数（fdtable + fd-backed handle 共享），崩溃循环零漂移审计覆盖两 ABI |
| 核心原语契约测试（rights 代数 / 背压 / EventQ / VMO 生命周期） | 已实现（改造阶段一） | `smoke-native-contract`，`test_native_contract.c` 四分区 |
| 句柄类型掩码 STAT 一致性（端点/队列可 query） | 已实现（阶段一副产品） | `handle_table.c` 类型掩码 + `ralg` 分区 |
| 双态部署驱动框架（drv_env + 共享协议层） | 已实现（改造阶段三） | `kernel/include/drivers/dual/drv_env.h` 三后端（KERNEL/USER/DRVMOD），goldfish RTC 同源码双态：内核壳 boot probe + `smoke-native-rtcd`，见 [04-dual-placement.md](04-dual-placement.md) |
| 双态语义一致性验证（virtio-input 第二样板） | 已实现 | `smoke-dual-input`：同一共享协议在两种部署下读出相同设备身份；DMA ops 已进 drv_env（信任模型） |
| 功能态用户驱动（virtio-input 事件面） | 已实现 | `smoke-dual-input`：全权初始化 + 共享 virtq + IRQ→EventQ，monitor `sendkey` 注入验证真实按键事件解码 |
| 连续 DMA heap | 已实现 | `device_alloc_dma` 预物化连续 VMO，`test_native_contract` 的 `dma` 分区验证连续物理地址与零填充 |
| 动态设备所有权 | 已实现 | `device_claim/release`，uinputd 两次启动验证自动释放 |
| IOMMU PCI 发现 | 已实现 | `smoke-iommu-discovery` 识别 QEMU `riscv-iommu-pci` |
| IOMMU 硬件初始化 | 已实现 | `smoke-iommu-discovery` 断言 BAR、capability（version 16）、DDT/DC/CQ/FQ 编程、使能及 CQON/FQON/DDTP 完成；PCI 叶子设备 passthrough DC |
| 服务协议 IDL 常量/固定消息层 | 已实现（阶段四起步） | `a20_services.idl` + `tools/a20idl.py`，rtcd payload 已生成；`make check-a20-idl` |
| IDL 版本化请求/响应信封 | 已实现 | rtcd 请求/响应独立 wire type + version/size 校验，`smoke-native-rtcd` |
| Linux pipe 人格层 PoC | 已实现（阶段五起步） | `smoke-native-personality`：channel/EventQ 的 pipe-shaped facade |
| Linux personality facade（fd/mmap/pipe/socketpair/futex/epoll） | 已实现（阶段五第二块） | `smoke-native-linux` 六分区 PASS，`a20_linux.h` |
| 统一驱动框架（`driver_t`/class 设备/DriverStore） | 已实现 | `kernel/drivers/core/driver_core.c` + `driver_manager.c`，`smoke-drvmod`、`smoke-evdev-stress` |
| drvmod 内核模块装载 | 已实现 | `kernel/drvmod/loader.c` ET_REL + `.a20drv` 描述段 + veneer/GOT，`smoke-drvmod-*` |
| fd-IPC 后端（channel_fd/eventfd/signalfd/timerfd/SysV shm/sem） | 已实现 | `kernel/ipc/*.c` 是 ABI 无关 vfile 后端，Linux `eventfd2/signalfd4/timerfd_*/sem*/shm*` 建立其上 |

## 正确性状态（SMP）

混合内核的 IPC/MM 路径在 SMP 下已收敛的已知问题（供参考，不构成开发记录）：

- **句柄表与 I/O scratch buffer 分离**：Native 句柄表使用独立 `task->a20_ht` 字段，不再与 Linux ABI 的 `proc_scratch_buffer()` 共用存储；
- **VMA 链表统一持锁**：`mm_mmap/munmap/brk/mprotect/mremap` 及 Linux ABI 对应 syscall 全部在 `mm->lock` 下修改 VMA 与页表；
- **远程 TLB 刷新**：IPI-based 远程刷（替代 SBI REMOTE SFENCE，TCG 下全量服务），且所有远程刷在解锁后发布、页释放前完成；
- **buddy 分配器**：脏块（内部帧仍被引用）不再回填空闲链表，`fl_push_clean` 拆解后只回填干净子块；
- **channel 入队**：`ch_try_enqueue` 对 peer 持引用，避免并发释放下的悬空入队。

## 已知边界

- **时间片捐赠仅限 UP**：SMP 捐赠依赖跨核唤醒/IPI 簿记（`PER_CPU_CURRENT_VALIDATION`），未完成前不开放；
- **Native ABI SMP 破坏（已收敛，2026-08-06 复验）**：此前 SMP=2/8 下 `native-shmring` 约 30% 概率的偶发内存破坏，经 M5 修复（`98a1260`、`1af0d02`：buddy 脏块拆分、页释放 TLB 顺序、peer 引用）后，在本分支复验为 **SMP=2 连续 20 轮 + SMP=8 连续 20 轮零失败、零挂起**（复现脚本：循环 QEMU 注入 `/bin/native-shmring-rv`，smp=2/8，日志归档于 `.kernel-build/smoke/shmring-smp{2,8}/`）。残余信号：`vmo_dirty_frames`（buddy 复用未清零帧，VMO 侧 memset 兜底，合法行为）已从串口 printf 降级为 `/proc/a20/objects` 累计计数器，不再干扰用户输出解析；阶段三起若需恢复帧级追踪，可在此计数器非零增长时重新挂 `frame_trace_dump_pfn`；
- **网络协议栈**：lwIP 作为内核态唯一实现（源码 `kernel/external/lwip`，由 `kernel/net` 编译）。此前的用户态 netd 服务（`user/svc/netd.c`、`kernel/net/netd_ring.c`、socket 代理）已整体移除：其 recv 数据段始终未通（子连接 PCB 在 accept 后消失），且 netd 进程并未被拉起，导致 AF_INET 被代理到一个不存在的进程。内核 lwIP 单一路径由 `smoke-network-suite` 验证（TCP/UDP 回环、DNS、ICMP、AF_UNIX 全通过）。
- **loongarch64**：内核与 native 测试均构建通过，运行时复测受工具链/镜像条件所限未像 riscv64 那样完整执行；
- **性能数据**全部来自 QEMU TCG 模拟器，真实硬件基准待测；
- **IOMMU/DMA 安全**：DMA 隔离已升级为真实 IOMMU 硬件强制——DDT(1LVL)、 CQ/FQ 使能，devid 0 配置 SV39 翻译域并经 TR_REQ 验证（已映射 IOVA 精确翻译、未映射 IOVA 被硬件拒绝 fault=1/cause=13），devid 1 保持 passthrough。fault 队列消费与 per-device 页表动态映射为后续工作。

## 基线回归观察（2026-08-06，分支 hybrid-kernel-refactor 记录）

以下为在干净 main HEAD（a7eb6d2）上观察到的问题，**不是**本分支改动引入：

- **HEAD 构建破损**：`virtio_input.c` 引用 `virtio_transport_t.shared_irq`、 loongarch64 缺少 `arch_tlb_flush_page_local`，两者均为进行中的 IRQ/驱动 重构的半成品（主工作区未提交修改包含对应完整实现）。本分支以单行 shim 补齐（`virtio_transport.h` 增字段、la64 `cpu.h` 增 local flush 包装）， 与进行中重构同形，合并时应自然消解；
- **HEAD 线程/阻塞路径挂起（riscv64）**：所有使用 `a20_thread_create` 的 native smoke（handle 的 `bch` 分区起、ipc、svc 等）在 SMP=1 下挂起至 超时；单线程路径（`smoke-native-contract` 全四分区）完整通过。已用 stash 对照实验证明与本分支的 STAT 改动无关。阻塞中的原生回归验证 （handle/ipc/svc 等）在此问题收敛前无法执行；
- **[VMO-PAGE] 诊断（已处理）**：契约测试与 shmring 复验期间观察到 `[VMO-PAGE] new pfn ... had content`（buddy 返回未清零复用帧，VMO 侧 memset 兜底，用户可见行为正确）。已降级为 `/proc/a20/objects` 的 `vmo_dirty_frames` 累计计数器，消除串口输出交错；契约测试已把 "新 VMO 页读零"固化为用户态契约（`vmol-zero`）；
- **mm_stress 45 秒门禁预算不足（HEAD 观察）**：`smoke-mm-stress` （SMOKE_TIMEOUT_MM_ST=45s）在 TCG 下于 `evict-mmap` 段超时，同一镜像 以 180s 预算完整 PASS。非本分支改动引入（本分支 MM 改动仅为 printf→计数器降级，严格更快），门禁预算需按当前 TCG 耗时重校。

## 复现入口

```bash
make smoke-native-ipc          # IPC 快路径
make smoke-native-svc          # 服务崩溃自愈（svcman 最小演示）
make smoke-native-registry     # 注册表 + 重绑（svcmgr）
make smoke-native-isolation    # 泄漏审计（崩溃循环零漂移）
make smoke-native-rtcd         # 用户态 RTC 驱动
make smoke-native-ubd          # 用户态 virtio-blk + 文件系统
make smoke-native-shmring      # 共享环数据面
make smoke-native-contract     # 核心原语契约（改造阶段一）
make smoke-native-linux        # Linux personality 六分区（阶段五）
make smoke-native-personality  # 人格层语义对照（pipe_ref）
make smoke-dual-input          # 双态部署语义一致 + 功能态用户驱动
make smoke-iommu-discovery     # IOMMU 硬件初始化与 TR_REQ 验证
make smoke-a20-channel         # Linux ABI 经 fd 消费 channel/registry
make smoke-clock-vdso          # vDSO
make smoke-mm-stress smoke-vfs-stress   # Linux ABI 回归
# 驱动崩溃恢复（手动）：QEMU 加 bus.3 scratch 盘，运行 /bin/ubd_recover
```

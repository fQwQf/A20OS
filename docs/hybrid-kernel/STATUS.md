# 混合内核：能力与边界清单

本文档以清单形式说明 A20OS 混合内核**当前源码具备的能力**与**已知边界**（最后核实：2026-08）。能力以源码和测试入口为准；运行类结论需在当前提交上按文末复现入口重新执行，历史里程碑记录见 [../archive/](../archive/)。设计总览见 [00-design.md](00-design.md)，机制语义见 [01-mechanisms.md](01-mechanisms.md)，演进路线见 [03-refactor-plan.md](03-refactor-plan.md)。

## 核心架构原则（总体已贯彻，含已知债务）

```
用户态 ── syscall 线格式 ──> ABI 层（薄包装）── 内部 API ──> 内部实现
```

- `kernel/ipc`、`kernel/mm`、`kernel/proc` 与 `kernel/include/core` 按 ABI 无关内部层组织；
- `kernel/drivers` 目前不是零 ABI 依赖：`kernel/drivers/core/driver_manager.c` 直接包含 `abi/native/types.h` 和 `abi/native/rights.h`，用 Native 对象类型与 rights 安装服务启动句柄。这是需要改为 core-owned 类型/API 的显式架构债务，因此不能宣称驱动目录或全仓已通过“对 `abi/` 零依赖”审计；
- Linux ABI 线格式常量由 `kernel/include/core/` 自持，`abi/linux/*` 再导出；
- 内部 IPC 子系统头位于 `kernel/include/ipc/`；
- 目标是让新增 ABI 只需薄包装即可共享内部混合内核机制；上述驱动管理器例外尚未收敛。

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
| 用户态文件系统服务（uxfs 代理 + ufsd FAT32） | 已实现源码与入口 | scratch 盘 FAT32 经 ufsd 服务，`smoke-native-ufs` |
| Linux ABI 透明获益（vDSO、唤醒快路径、AF_UNIX 桥接） | 已实现 | `smoke-clock-vdso` + unix 测试 |
| TCP/IP 协议栈（内核态 lwIP 单一实现） | 已实现 | lwIP 源码位于 `kernel/external/lwip`，由内核 `kernel/net` 编译；`smoke-network-suite` |
| 统一等待对象层（futex/EventQ/channel/pipe/socket/mutex 共用 `wait_queue_t`） | 已实现 | futex 迁移到共享 `wait_queue_t`（谓词匹配/requeue/purge），`smoke-futex-stress`、`smoke-sched-stress` |
| Channel IPC 从 Linux ABI 消费（fd 表面） | 已实现 | `SYS_a20_channel_pair`(900)/`SYS_a20_registry_client`(901)：Linux 程序经 read/write 使用同一 channel 机制与服务注册表，`smoke-a20-channel` |
| 统一对象计量（fd/vfile 纳入对象审计） | 已实现 | `/proc/a20/objects` 增加 `vfiles` 计数（fdtable + fd-backed handle 共享），崩溃循环零漂移审计覆盖两 ABI |
| 核心原语契约测试（rights 代数 / 背压 / EventQ / VMO 生命周期） | 已实现（改造阶段一） | `smoke-native-contract`，`test_native_contract.c` 四分区 |
| 句柄类型掩码 STAT 一致性（端点/队列可 query） | 已实现（阶段一副产品） | `handle_table.c` 类型掩码 + `ralg` 分区 |
| 双态部署环境层（drv_env） | 骨架已实现 | 头文件有 KERNEL/USER/DRVMOD 三后端；活跃样板使用 USER/DRVMOD，尚无 `DRV_ENV_KERNEL` 同源样板 |
| 双态共享协议 probe（virtio-input） | 已实现源码与入口 | DRVMOD 只读 probe 与 USER uinputd 共享配置读取头；`smoke-dual-input` 设计为比较设备身份 |
| 功能态 virtio-input | 两条独立路径 | USER uinputd 有共享 virtq + IRQ→EventQ；DRVMOD `vinput.c` 有独立完整实现，尚非同源完整双态 A/B |
| 连续 DMA heap | 已实现 | `device_alloc_dma` 预物化连续 VMO，`test_native_contract` 的 `dma` 分区验证连续物理地址与零填充 |
| 动态设备所有权 | 已实现 | `device_claim/release`，uinputd 两次启动验证自动释放 |
| IOMMU PCI 发现 | 已实现 | `smoke-iommu-discovery` 识别 QEMU `riscv-iommu-pci` |
| IOMMU bring-up/静态翻译探测 | 已实现源码与入口 | `smoke-iommu-discovery` 检查 DDT/DC/CQ/FQ 与静态 TR_REQ；动态 per-device DMA map/fault 消费未实现 |
| 服务协议 IDL 常量/固定消息层 | 已实现（阶段四起步） | `a20_services.idl` + `tools/a20idl.py`，rtcd payload 已生成；`make check-a20-idl` |
| IDL 版本化请求/响应信封 | 部分已实现 | rtcd 与 svcmgr/echod 使用 version/size envelope；ubd 仅使用生成常量，数据面走共享环 |
| Linux pipe 人格层 PoC | 已实现（阶段五起步） | `smoke-native-personality`：channel/EventQ 的 pipe-shaped facade |
| Linux personality facade（fd/mmap/pipe/socketpair/futex/epoll） | 已实现（阶段五第二块） | `a20_linux.h` + `test_native_linux.c` 六分区；运行入口 `smoke-native-linux` 本次未执行 |
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
- **Native ABI SMP 历史回归**：SMP=2/8 的 `native-shmring` 偶发破坏曾由 M5 修复（2026-08 在当时的分支上连续 20 轮零失败零挂起，属历史记录）；`vmo_dirty_frames` 仍提供复用帧观察点，复验入口 `smoke-native-shmring`；
- **网络协议栈**：lwIP 是当前内核态唯一实现，旧 netd 路径已移除。`smoke-network-suite` 聚合 TCP/UDP/ICMP loopback、DNS、AF_UNIX、AF_ALG 和 timeout，其中 DNS/AF_ALG 可跳过；当前仅 RISC-V64 有运行入口；
- **loongarch64**：双架构发布流程曾整体通过（历史记录），但当前多个 Native/dual/mlibc smoke 仍只有 RISC-V64 运行入口，LoongArch64 的结论需逐项复验；
- **性能数据**全部来自 QEMU TCG 模拟器，真实硬件基准待测；
- **IOMMU/DMA 安全**：DDT/CQ/FQ 与 devid 0 静态 TR_REQ 探测已实现，但用户驱动 DMA 尚未接入动态 per-device domain，fault 队列也未消费；当前仍不能宣称端到端硬件强制隔离。

## 历史诊断记录（2026-08-06，hybrid-kernel-refactor 分支）

以下记录当时 `a7eb6d2`/`hybrid-kernel-refactor` 分支的诊断背景，用于理解相关子系统曾出现的问题形态；它们不是当前缺陷清单，也不能作为当前测试结果：

- **当时分支的构建破损**：`virtio_input.c` 引用 `virtio_transport_t.shared_irq`、loongarch64 缺少 `arch_tlb_flush_page_local`，两者均为当时 IRQ/驱动重构的半成品；诊断分支用单行 shim 补齐；
- **当时分支的线程/阻塞路径挂起（riscv64）**：使用 `a20_thread_create` 的 native smoke（handle 的 `bch` 分区起、ipc、svc 等）在 SMP=1 下挂起至超时；单线程 `smoke-native-contract` 四分区当时通过。stash 对照显示与当时的 STAT 改动无关；
- **[VMO-PAGE] 诊断（已处理）**：契约测试与 shmring 复验期间观察到 `[VMO-PAGE] new pfn ... had content`（buddy 返回未清零复用帧，VMO 侧 memset 兜底，用户可见行为正确）。已降级为 `/proc/a20/objects` 的 `vmo_dirty_frames` 累计计数器，消除串口输出交错；契约测试已把 "新 VMO 页读零"固化为用户态契约（`vmol-zero`）；
- **当时的 mm_stress 45 秒门禁预算不足**：`smoke-mm-stress`（`SMOKE_TIMEOUT_MM_ST=45s`）在 TCG 下于 `evict-mmap` 段超时，同一镜像以 180s 预算曾完整 PASS。当前默认仍是 45s；如需调整预算，应在当前提交上以两种预算对照复验后再决定。

## 复现入口

```bash
make smoke-native-ipc          # IPC 快路径
make smoke-native-svc          # 服务崩溃自愈（svcman 最小演示）
make smoke-native-registry     # 注册表 + 重绑（svcmgr）
make smoke-native-isolation    # 泄漏审计（崩溃循环零漂移）
make smoke-native-rtcd         # 用户态 RTC 驱动
make smoke-native-ubd          # 用户态 virtio-blk + 文件系统
make smoke-native-ufs          # 用户态文件系统服务（uxfs + ufsd）
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

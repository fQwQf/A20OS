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
| netd 帧面 + socket 代理（lwIP 用户态 + 帧环 + RPC 代理） | host→guest 与 guest→host TCP echo 数据面已通 | `netd=1` 下 hostfwd echo（`HOST_GOT b'netd-sock-echo' len=14`）与 `NETD_SOCK_TEST: PASS` |
| 核心原语契约测试（rights 代数 / 背压 / EventQ / VMO 生命周期） | 已实现（改造阶段一） | `smoke-native-contract`，`test_native_contract.c` 四分区 |
| 句柄类型掩码 STAT 一致性（端点/队列可 query） | 已实现（阶段一副产品） | `handle_table.c` 类型掩码 + `ralg` 分区 |
| 双态部署驱动框架骨架（drv_env + 共享协议层） | 骨架已实现（改造阶段三起步） | goldfish RTC 同源码双态：内核壳 boot probe + `smoke-native-rtcd`，见 [04-dual-placement.md](04-dual-placement.md) |
| 双态语义一致性验证（virtio-input 第二样板） | 已实现 | `smoke-dual-input`：同一共享协议在两种部署下读出相同设备身份；DMA ops 已进 drv_env（信任模型） |
| 功能态用户驱动（virtio-input 事件面） | 已实现 | `smoke-dual-input`：全权初始化 + 共享 virtq + IRQ→EventQ，monitor `sendkey` 注入验证真实按键事件解码 |
| 连续 DMA heap | 已实现 | `device_alloc_dma` 预物化连续 VMO，`test_native_contract` 的 `dma` 分区验证连续物理地址与零填充 |
| 动态设备所有权 | 已实现 | `device_claim/release`，uinputd 两次启动验证自动释放 |
| IOMMU PCI 发现 | 骨架已实现 | `smoke-iommu-discovery` 识别 QEMU `riscv-iommu-pci`；翻译尚未启用 |
| 服务协议 IDL 常量层 | 已实现（阶段四起步） | `a20_services.idl` + `tools/a20idl.py` + `make check-a20-idl` |
| Linux pipe 人格层 PoC | 已实现（阶段五起步） | `smoke-native-personality`：channel/EventQ 的 pipe-shaped facade |

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
- **网络协议栈**：lwIP 已编译为用户态 netd 服务（bootarg `netd=1` 激活；未激活时内核 lwIP 行为不变）。帧环（RX/TX）与 socket 代理 RPC（create/bind/listen/accept/connect/send/recv/close/poll/getsockname/setsockopt）已实现；QEMU hostfwd 验证了完整代理链路与 TCP 握手（SYN-ACK 出帧面、accept 回调触发）。**剩余**：数据段在 lwIP 侧被丢弃（子连接 PCB 在 accept 后从 active 列表消失，`lookup pcb=0`），recv 数据回传未通——根因锁定在 tcp_process 的 accept/abort 路径，待续；
- **loongarch64**：内核与 native 测试均构建通过，运行时复测受工具链/镜像条件所限未完整执行；
- **性能数据**全部来自 QEMU TCG 模拟器，真实硬件基准待测；
- **IOMMU/DMA 安全**：DMA 契约是"内核分配 + pin + 物理地址上报"的信任模型，无硬件 IOMMU 强制。

## 基线回归观察（2026-08-06，分支 hybrid-kernel-refactor 记录）

以下为在干净 main HEAD（a7eb6d2）上观察到的问题，**不是**本分支改动引入：

- **HEAD 构建破损**：`virtio_input.c` 引用 `virtio_transport_t.shared_irq`、
  loongarch64 缺少 `arch_tlb_flush_page_local`，两者均为进行中的 IRQ/驱动
  重构的半成品（主工作区未提交修改包含对应完整实现）。本分支以单行 shim
  补齐（`virtio_transport.h` 增字段、la64 `cpu.h` 增 local flush 包装），
  与进行中重构同形，合并时应自然消解；
- **HEAD 线程/阻塞路径挂起（riscv64）**：所有使用 `a20_thread_create` 的
  native smoke（handle 的 `bch` 分区起、ipc、svc 等）在 SMP=1 下挂起至
  超时；单线程路径（`smoke-native-contract` 全四分区）完整通过。已用
  stash 对照实验证明与本分支的 STAT 改动无关。阻塞中的原生回归验证
  （handle/ipc/svc 等）在此问题收敛前无法执行；
- **[VMO-PAGE] 诊断（已处理）**：契约测试与 shmring 复验期间观察到
  `[VMO-PAGE] new pfn ... had content`（buddy 返回未清零复用帧，VMO 侧
  memset 兜底，用户可见行为正确）。已降级为 `/proc/a20/objects` 的
  `vmo_dirty_frames` 累计计数器，消除串口输出交错；契约测试已把
  "新 VMO 页读零"固化为用户态契约（`vmol-zero`）；
- **mm_stress 45 秒门禁预算不足（HEAD 观察）**：`smoke-mm-stress`
  （SMOKE_TIMEOUT_MM_ST=45s）在 TCG 下于 `evict-mmap` 段超时，同一镜像
  以 180s 预算完整 PASS。非本分支改动引入（本分支 MM 改动仅为
  printf→计数器降级，严格更快），门禁预算需按当前 TCG 耗时重校。

## 复现入口

```bash
make smoke-native-ipc       # IPC 快路径make smoke-native-svc       # 服务崩溃自愈make smoke-native-registry  # 注册表 + 重绑make smoke-native-isolation # 泄漏审计（崩溃循环零漂移）make smoke-native-rtcd      # 用户态 RTC 驱动make smoke-native-ubd       # 用户态 virtio-blk + 文件系统make smoke-native-shmring   # 共享环数据面make smoke-clock-vdso       # vDSOmake smoke-mm-stress smoke-vfs-stress   # Linux ABI 回归
make smoke-native-contract  # 核心原语契约（改造阶段一）
# 驱动崩溃恢复（手动）：QEMU 加 bus.3 scratch 盘，运行 /bin/ubd_recover
```

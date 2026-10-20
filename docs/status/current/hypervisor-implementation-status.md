# VM 平台状态与目标（2026-10-13）

1.0 须完成 [平台设计](../../hypervisor/04-platform-design.md) 已约定的首批平台目标，纳入
[发布计划](../../releases/v1.0.md) 的 V1-HYP；重复运行失败、SMP、设备模型、既定硬件后端
和 /dev/kvm 集成均继续作为开发与验收工作，不能因 Rust 重写只保留单 guest 演示。
发布还须满足 [功能与性能对照](../../releases/v1.0-validation.md)，本页当前失败记录不因此改变。

**当前为实验平台，尚不能认定为稳定可投入使用。** 新源码已全量构建，并完成单次双 guest 和终端交互验证；同一宿主重复创建、暂停恢复后的退出，以及 SMP 场景仍有失败。操作入口见 [使用指南](../../hypervisor/03-usage.md)。本阶段以验证和必要缺陷修复为主，没有扩展其它架构或 KVM 功能。

## 当前实现

Native 与 Linux 扩展 syscall 包装共享 VM/vCPU 核心，统一引用、生命周期、配置、控制和统计。Linux 扩展 syscall **不是 KVM ABI**。实际后端只有 RISC-V H，要求 Sstc 和 firmware 权限；其它架构返回不支持。

Sv39x4 根表 16 KiB 对齐，开放低 512 GiB GPA，使用 VMID 0 并在激活时刷新两阶段翻译。RAM 按页分配记账，设备通过 pin/copy 访问。vCPU 激活为 CPU-local，保存 VS CSR、FP、计时器和 IRQ 状态。

hypvm managed 模式提供独立输入输出队列和可恢复 RUN。a20-vmd 用独立 pipe 监督 guest，专用 vport0 与宿主 UART 分开。tools/a20 提供命名 QEMU 宿主、私有磁盘 overlay、QMP 和 attach；tools/a20 vm 提供 guest 生命周期和可重连 console。

已加入实验性多 vCPU、SBI HSM/IPI/RFENCE、远程 stage-2 失效和 SMP FDT。profile 接受 1–8 vCPU，实际并行受宿主 CPU 和后端限制；**参数上限不代表已经验收**。supervisor 最多 8 个记录、512 MiB 运行 guest RAM 请求预算，每台 8–256 MiB；预算尚未覆盖完整内核、VMM、pipe 和设备开销。

## 实际验证

环境：Linux 开发机，QEMU 10.0.13，RISC-V GCC，TCG；ABI=both、启用抢占。宿主、用户程序、RAMFS guest 和镜像均从本 worktree 新构建，不用旧预编译内核作为通过证据。

| 场景 | 结果 |
|---|---|
| 单 CPU、双 CPU 宿主完整构建 | 通过编译、汇编、链接和打包 |
| 专用管理通道 | READY protocol=2，HELLO/LIST/STATS 应答通过 |
| H 扩展 stage-2 selftest | HYP_SELFTEST: PASS、hyp_selftest=PASS |
| 无 H 扩展 | selftest 明确 SKIP；hypvm 返回 errno=95、退出码 4，宿主仍可交互 |
| 单次双 guest 生命周期 | 两台 shell 执行不同命令、无串线、重连、暂停隔离、正常关机和 destroy 全部通过 |
| 实际 PTY console 前端 | 输入拼接 echo 得到 FRONTEND_WORKS，Ctrl-] 断开通过 |
| 同宿主重复双 guest | **失败**：第一轮可完整通过，第二轮 START/管理请求仍可能超时 |
| 暂停恢复后正常退出 | **不稳定**：有通过记录，也复现收到 exit 后未结束 |
| 两台双 vCPU guest | **未通过验收**：管理请求超时，未取得完整 2/2 CPU online 和生命周期证据 |

通过流程的 VM/vCPU/映射/硬件页表计数从 [0,0,0,0] 回到 [0,0,0,0]。宿主空闲页数仍有变化；这不能证明全系统无泄漏，也未完成所有 cgroup 和元数据回收验收。工具测试结果及可审阅的 JSON/日志见 [验证记录](../evidence/hypervisor/2026-10-13/README.md)。原始较长日志在本机 /tmp/a20-hyp-verify/ 和 /tmp/a20-hyp-*.log，临时文件不是长期归档。

## 本阶段修复

- 修正 build INSTANCE -- -j4 参数转发，以及资源门禁忽略磁盘/并发环境覆盖的问题。
- 修正 guest 入口在 sret 前开启中断导致上下文损坏的问题。
- 保存恢复 guest VU/VS 权限，避免 RUN 恢复时把用户态当作 supervisor。
- managed WFI 返回用户态，RUN 后主动让出调度，激活时重置宿主调度期限；禁用 RUN 后不适用的宿主 FP 快速返回假设。
- vCPU 内部失败保持 VM 故障并请求停止其它 vCPU，避免被后续正常关机覆盖。
- 探针要求行首 shell 提示符，避免启动图案的 # 字符误判。

**这些修复没有消除重复运行和 SMP 阻塞项，剩余问题尚未完整定位。**

## 使用边界与目标

适合隔离的 QEMU 实验环境。建议每次实验新建宿主并保留日志；管理超时且 guest stop 无效时退出并重建外层宿主。这是恢复办法，不是稳定性保证。

没有完整 guest virtio 磁盘/网络模型、通用 Linux guest 验收或 Native/Linux 两套 ABI 的独立运行矩阵。machine.accel=kvm 仅选择开发机外层 QEMU 加速器，不表示 A20 有 /dev/kvm；本阶段没有实测外层 KVM。

近期先消除重复创建/退出和 SMP 超时，再完成同宿主至少 100 次创建销毁、关闭竞态、低内存回滚、FP/CSR/计时器隔离、资源账目和长稳验收。只有可重复的证据才能提升可用状态。

平台目标仍是 [总体设计](../../hypervisor/04-platform-design.md) 的共享核心加多 ABI 包装：完善资源模型和用户态设备，实现并分别验收 AArch64 EL2、Intel VMX、AMD SVM；实现 /dev/kvm fd/ioctl/mmap run page 和 exit/completion，以固定版本 QEMU 验收。其它架构、真机、性能和故障隔离留作后续阶段。以上均为目标，不能列作当前支持。

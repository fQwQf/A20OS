# VM 平台状态与目标

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

1.0 只验收既有 RISC-V H 后端的生命周期；其余硬件后端、A20 `/dev/kvm`、新设备模型与完整 Linux guest 已按 [发布范围 §2.2](../../releases/v1.0.md) 移至后续版本。

**RISC-V H 的 1.0 生命周期门禁已通过。** 2026-11-16 在同一宿主完成 50 轮双 VM × 双 vCPU，共 100 次创建、命令执行、双方暂停/恢复、关机和销毁，另通过运行中的双 guest 管理 STOP。结果仅对应下面的 QEMU/TCG 档位，不能据此宣布整个 OS 已完成 1.0。操作入口见 [使用指南](../../hypervisor/03-usage.md)。

## RISC-V SBI 增补与验证

RV32/RV64 已共享虚拟 SBI 分发器，补齐 legacy getchar、未知调用错误返回、 HSM 启动地址权限检查、TIME 清 pending 与 SRST 参数校验，并新增 DBCN 批量控制台收发。两种架构在 QEMU 下各通过 40 项 VS/VU 调用检查、双 vCPU 的远程 IPI/RFENCE/HSM 检查及无 H 负控；资源计数回到基线。接口限制、复现和证据见 [RISC-V SBI](../../hypervisor/10-riscv-sbi.md)。仍报告 SBI 0.2，ID `0xa20`未获上游分配，HSM suspend 与 guest reboot 未实现；完整 OS guest 的既有阻塞项不因这些小载荷检查通过而关闭。

## 当前实现

Native 与 Linux 扩展 syscall 包装共享 VM/vCPU 核心，统一引用、生命周期、配置、控制和统计。Linux 扩展 syscall **不是 KVM ABI**。既有 RISC-V H 后端要求 Sstc 和 firmware 权限。2026-10-25 增加并实际运行验证了 AArch64 EL2、ARM32 HYP、RV32 H、x86_64 AMD SVM/Intel VMX 后端，以及每种可执行后端的双宿主 CPU / 双 vCPU 并行、按需映射、权限拒绝、抢占、停止和回收。VMX 使用 Bochs 软件 CPU 实际执行，SVM 另有嵌套 KVM 记录；没有 Intel 实机验收。LoongArch LVZ 仅提供默认关闭的实验实现，尚无 LVZ 正控运行证据。RISC-V 完整 OS guest SMP 的本轮证据见下节；virtual GIC/APIC、其它后端完整 OS 与更长时间运行没有因此验收。见 [多架构后端](../../hypervisor/08-multiarch-backends.md) 与 [SMP 验证记录](../../hypervisor/09-smp-validation.md)。

Sv39x4 根表 16 KiB 对齐，开放低 512 GiB GPA，使用 VMID 0 并在激活时刷新两阶段翻译。RAM 按页分配记账，设备通过 pin/copy 访问。vCPU 激活为 CPU-local，保存 VS CSR、FP、计时器和 IRQ 状态。

hypvm managed 模式提供独立输入输出队列和可恢复 RUN。a20-vmd 用独立 pipe 监督 guest，专用 vport0 与宿主 UART 分开。tools/a20 提供命名 QEMU 宿主、私有磁盘 overlay、QMP 和 attach；tools/a20 vm 提供 guest 生命周期和可重连 console。

已加入实验性多 vCPU、SBI HSM/IPI/RFENCE、远程 stage-2 失效和 SMP FDT。profile 接受 1–8 vCPU，实际并行受宿主 CPU 和后端限制；**参数上限不代表已经验收**。supervisor 最多 8 个记录、512 MiB 运行 guest RAM 请求预算，每台 8–256 MiB；预算尚未覆盖完整内核、VMM、pipe 和设备开销。

## RISC-V 平台验证

环境：Linux 开发机，QEMU 10.0.13，RISC-V GCC，TCG；ABI=both、启用抢占。宿主、用户程序、RAMFS guest 和镜像均从本 worktree 新构建，不用旧预编译内核作为通过证据。

| 场景 | 结果 |
|---|---|
| 单 CPU、双 CPU 宿主完整构建 | 通过编译、汇编、链接和打包 |
| 专用管理通道 | READY protocol=2，HELLO/LIST/STATS 应答通过 |
| H 扩展 stage-2 selftest | 通过；含映射拒绝、坏 pfn、设备 GPA、部分 load 回滚和低内存 charge 失败的负控 |
| 无 H 扩展 | selftest 明确 SKIP；hypvm 返回 errno=95、退出码 4，宿主仍可交互 |
| 单次双 guest 生命周期 | 两台 shell 执行不同命令、无串线、重连、暂停隔离、正常关机和 destroy 全部通过 |
| 实际 PTY console 前端 | 输入拼接 echo 得到 FRONTEND_WORKS，Ctrl-] 断开通过 |
| 同宿主重复双 guest | 通过 50 轮、100 次完整生命周期，不重启宿主；每轮 VM/vCPU/映射/页表计数回到基线 |
| 暂停恢复后正常退出 | 每轮两台均暂停/恢复；随后正常关机、输出排空、记录销毁通过 |
| 两台双 vCPU guest | 每轮均到 2/2 CPU online、执行独立命令，并完成上述生命周期 |

通过流程的 VM/vCPU/映射/硬件页表计数从 [0,0,0,0] 回到 [0,0,0,0]。宿主空闲页数仍有变化；这不能证明全系统无泄漏，也未完成所有 cgroup 和元数据回收验收。工具测试结果及可审阅的 JSON/日志见 验证记录。原始较长日志在本机 /tmp/a20-hyp-verify/ 和 /tmp/a20-hyp-*.log，临时文件不是长期归档。

## 本阶段修复

- 修正 build INSTANCE -- -j4 参数转发，以及资源门禁忽略磁盘/并发环境覆盖的问题。
- 修正 guest 入口在 sret 前开启中断导致上下文损坏的问题。
- 保存恢复 guest VU/VS 权限，避免 RUN 恢复时把用户态当作 supervisor。
- managed WFI 返回用户态，RUN 后主动让出调度，激活时重置宿主调度期限；禁用 RUN 后不适用的宿主 FP 快速返回假设。
- vCPU 内部失败保持 VM 故障并请求停止其它 vCPU，避免被后续正常关机覆盖。
- 探针要求行首 shell 提示符，避免启动图案的 # 字符误判。
- `hyp_arch_vcpu_setup()` 不再每次 RUN 都把宿主 `stimecmp` 重设为 now+10ms；managed guest 每输出一个字符就产生一次 RUN，旧行为让宿主 tick 永远不到期。现在只在没有或过远的 deadline 时缩短它。
- managed RUN 在 guest trap 边界检查宿主时间片；到期主动 YIELD，让挂起的宿主 tick 在宿主上下文处理。诊断曾观测到 HS 定时中断迟到，这条预算限制了有 guest trap 边界时的单次 RUN 占用；尚未证实迟到由 guest 的中断屏蔽导致。
- `a20-vmd` 的主循环改为 `poll()` 等待控制通道与 guest 输出，而不是只做 `nanosleep`；请求数据到达即可唤醒管理进程，不再依赖可能被推迟数秒的定时器唤醒。
- `hyp_selftest` 增加失败路径负控：越界 GPA、坏 pfn、设备 GPA fill、与已有映射冲突的两页 load（含部分回滚）、零限额 cgroup 下的 bulk load 与 demand fill。每项都校验 `mapped_pages` 不增长。

### 2026-11-16 生命周期收口

新增修复包括 RUN 入口恢复 `t0`、驱动模块的 `__ctzdi2` 导出、guest FDT 内存字段、无 IRQ 虚拟 UART 的定时 readiness、空 EEVDF 队列中新任务的有界信用和运行任务权重、整个线程组的停止/继续与完成报告，以及主/副 VMM 线程共用的正常关机判定。关机竞态的失败轮次保留，未计入通过数量。

公开入口：先构建并启动 `hyp-host-riscv64-acceptance`，再运行 `python3 tools/probes/hyp_lifecycle_probe.py --host <name> --cycles 100 --output <evidence-dir>`。该档位显式启用管理进程 5 ms nanosleep 审计；正常管理档位仍使用事件等待。每轮两台 RAMFS guest 都执行独立命令、重连、暂停/恢复、关机和销毁，并核对四类 VM 资源计数。

本轮报告为 `build/evidence/v1-closeout-20261116/hyp-acceptance-final/report.json`：100 次通过，98,304 个睡眠样本最大耗时 139 ms（最大超出请求约 134 ms），预设上界 150 ms。内核 SHA-256 为 `ecc7844b5d7b026a8054ddd41c6457dc338adf8cdd9200c8ae86e4333ee2217c`。管理 STOP 另有 `hyp-management-stop.json`，两台均被取消、输出排空、销毁且计数复原。

负控包括实际 VS `t0` 哨兵的旧入口失败、确定性的 EEVDF 空队列/运行权重回退，以及修复前管理进程已在 ready queue 却长时间未执行的快照。它们区分任务已可运行但未获调度和仍在定时等待中的状态。独立的 `smoke-sigstop-group-smp2` 连续 20 轮核对停止报告到达时两个线程的计数器均冻结，SIGCONT 后两者继续。

进一步的同宿主回归（`hyp-acceptance-list-rtt/report.json`）在第 36 轮完成正常生命周期后，检测到 152 ms 的 5 ms 睡眠耗时，超出预设 150 ms；该轮 RPC 均未超时。首轮结果保留，当时将 RUNTIME-06 重新列为开放，门限没有放宽。

2026-11-16 进一步修复 CPU 迁移时虚拟运行时间未换算的问题，并以 QMP `query-cpus-fast` 的实际 Linux 线程 ID 分配 host CPU：两个 vCPU 分别绑定 10/12，I/O 线程绑定 14，避免依赖未开启的线程名称。内核 `3a491a6fa42c274057e4e31ced1522feef089fb640015b1e28f57d09f71af9b3` 在该配置下首轮 100 次生命周期通过，124,928 个睡眠样本最大 148 ms；继续同宿主运行时，在完成另外 26 个生命周期后出现 322 ms，仍超过 150 ms。报告分别是 `hyp-rebase-qmp-affinity-final/report.json` 和 `hyp-rebase-qmp-affinity-repeat/report.json`。该次首轮通过未关闭 RUNTIME-06。

已修复 guest-trap 绕过通用 trap 路径时缺失的 hardirq 记账，真实 IRQ 片段与负控通过。但 `hyp-hardirq-diag` 仍出现 187 ms；进一步的 `hyp-host-observed` 与 `hyp-host-wchan` 分别在 40 / 42 次生命周期后出现 181 / 193 ms。约 1 ms 采样的 Linux 线程观测记录了实际 vCPU/IO 线程运行、runqueue 等待和 wchan，未证明这些超界可以归因于外部宿主抢占。临时 IRQ/长 RUN printf 已撤回，不进入最终内核；带 LD_PRELOAD 等待追踪的单独试验只作诊断，不计验收通过。错误的按线程名称绑定试验曾把全部线程绑定到一个核并导致启动超时，保留在 `hyp-rebase-controlled-final`，不计作有效固定配置运行。

后续通过预加载的只读寄存器采样与宿主指令位置/栈采样，在 `hyp-poll-rip-unblocked` 的 42 次生命周期后复现 154 ms：一个 vCPU 位于 `tcg_region_reset_all` 调用的 GLib tree 清理中，另一线程等待；实际二进制反汇编确认返回位置紧跟 `g_tree_destroy`。这批采样改变线程信号掩码，只用于定位，不计验收。QEMU 的 [TCG region 实现](https://raw.githubusercontent.com/qemu/qemu/v10.0.13/tcg/region.c) 与实际栈一致；缓存参数按[加速器源码](https://raw.githubusercontent.com/qemu/qemu/v10.0.13/accel/tcg/tcg-all.c)以 MiB 转换。

实现 `60aa316b1` 增加有类型校验的 `machine.tcg_tb_size_mb`，仅在显式 `accel="tcg"` 时可设置（1..4096 MiB）。验收实例固定 128 MiB，限制单次整体清理的工作量；其它实例保持默认。去除所有预加载诊断后，同一宿主进程完成两批各 100 次生命周期，分别最大 73 / 79 ms，RPC 最慢 684.604 / 92.489 ms，四类 VM 计数每轮均复原。缓存 flush 计数从首批后的 25 增到第二批后的 50，验收确实经历多次缓存重置。第二批累计审计样本包含两批之间的宿主空闲期，不能当作第二批负载样本数。证据为 `hyp-tcg128-first/report.json`、`hyp-tcg128-repeat/report.json`、对应 `jit-after-*.txt` 与 `hyp-tcg128-inputs.json`。RUNTIME-06 在该固定配置范围关闭；150 ms 和 RPC 3 s 门限未调整。

宿主空闲页从 510,501 到 506,005，包含首次文件页缓存及仍待分类的 task/allocator 元数据变化；本门禁不把四类 VM 计数复原等同全系统或全部 cgroup 无泄漏。一般任务/VMO 资源验收继续按对应工作包处理。

## 使用边界与目标

适合隔离的 QEMU 实验环境。生命周期门禁在同一宿主重复执行，不以重启掩盖计数变化；保留日志和私有磁盘 overlay。

没有完整 guest virtio 磁盘/网络模型、通用 Linux guest 验收或 Native/Linux 两套 ABI 的独立运行矩阵。machine.accel=kvm 仅选择开发机外层 QEMU 加速器，不表示 A20 有 /dev/kvm；本阶段没有实测外层 KVM。

同宿主两批共 200 次生命周期及固定配置的管理进程定时上界已完成本轮验收。低内存、映射失败回滚与 CSR/FP 小载荷检查保留既有范围。更长稳态、其它设备/后端与物理平台需各自证据。

[总体设计](../../hypervisor/04-platform-design.md) 仍保留共享核心加多 ABI 包装的长期路线；其中 AArch64 EL2、Intel VMX、AMD SVM、A20 `/dev/kvm`、用户态设备和完整 Linux guest 不属于 1.0 承诺。1.0 只要求本页所列 RISC-V H 生命周期的正常/故障停止、重复循环和资源回收。

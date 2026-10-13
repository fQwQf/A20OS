# VM 平台实现状态（2026-10-13）

本轮在 `codex/hyp-platform` 独立 worktree 中实施。[总体设计](04-platform-design.md)仍是目标；本页区分代码进度和已验证能力。按用户要求，本阶段没有全量编译、链接、QEMU 启动或运行测试。

## 本轮代码

- `hyp_backend` 把 VM 初始化/销毁、stage-2 映射/解除/查询/审计、启动参数及 vCPU load/enter/put 连接到同一核心。RISC-V SBI、指令解码、Sv39x4 实现已移到 `kernel/arch/riscv64/hyp/`。
- Native VM/vCPU 正式对象类型为 18/19，接入句柄权限、复制、转移和释放。Native 与 Linux 包装均在查找时持有临时引用；RUN 独立持有引用，结果在释放运行占用前取得快照。
- 两套 ABI 共用创建、加载、RAM 窗口设置、启动参数、marker、状态、控制台和销毁逻辑。Linux 扩展新增 915=vCPU destroy、916=console、917=set RAM；Native 对应新增 0x0F05..0x0F0A，详见 syscall_nr.h。**Linux 扩展 syscall 不是 KVM ABI。**
- Sv39x4 根表分配为 16 KiB、16 KiB 对齐；当前仍只开放低 512 GiB GPA。VMID 使用 0，每次激活刷新两阶段翻译，消除未探测 VMID 位宽与计数回绕假设。子页表分配会重查并释放竞争失败者的页。
- RISC-V 进入/退出保存恢复 VS CSR、Sstc comparator 和 FP bank；首次进入的 FP bank 为零。load/enter/put 之间禁止宿主 IRQ 窗口，guest 期间仍接收宿主 IRQ。当前要求 H、Sstc，并在 load 中确认 firmware 允许 STCE。
- VM 配置更新与 RUN 互斥，分配期间不持有 spinlock。RAM 窗口不得超出创建上限，第一次分配 RAM 后不能搬移；镜像加载和 demand-fill 都记账到创建者 cgroup，VM 最终释放时归还账目。
- 每个 VM 的托管控制台有独立输入/输出队列；托管模式不会抽取宿主 UART。控制台输出或宿主 IRQ 产生 `HYP_EXIT_YIELD=4`，保留状态供下一次 RUN 恢复。待输出未读完时 RUN 不进入 guest。
- `hypvm --managed` 接通 poll、短读写、输入保留、输出排空、EBUSY 重试和 RUN 恢复。显式关闭 vCPU，检查入口范围和 RAM 算术；PASS 必须是正常关机且满足 banner 判据。

## 多实例边界

代码支持创建多个 VM、各自持有状态和控制台，并为托管模式提供串行时间片运行路径。已改为 CPU-local 激活；不同 VM 可以占用不同宿主 CPU，但尚未运行验收。**同一 VM 仍只允许一个正在执行的 vCPU**，不能宣称 guest SMP 可用。传统 `hypvm` 的直通 UART 模式不适合作为多个 VM 的控制台管理方式。

`tools/a20` 的多个命名 QEMU 宿主实例与 A20 内核中的多个 guest 是两个层次。宿主工具用法见 [06-host-runtime.md](06-host-runtime.md)。现已接入 `a20-vmd` 和 `tools/a20 vm start --host`，管理通道使用专用 vport0，源码和协议见 [07-guest-supervisor.md](07-guest-supervisor.md)，运行证据仍待授权。

## 验证证据

- RISC-V：修改后的共享核心、后端、IPC 对象和 ABI 包装执行 `-fsyntax-only -Wall -Wextra -Werror`。
- AArch64、x86_64：共享 hyp 源文件与现有不支持后端执行同等级单文件语法检查；这不能证明对应架构虚拟化可运行。
- RISC-V NOMMU：共享 hyp 与后端源文件通过语法检查，VM 创建仍返回不支持。
- RISC-V 后端 C 单独编译为 `/tmp/a20-hyp-arch.o`；vCPU 汇编单独生成 `/tmp/a20-hyp-vcpu-asm.o`；未参与链接。
- `hypvm.c`、`hyp_guest.c` 通过用户态 C 语法检查。
- Python 源文件仅做 AST 解析，实例 TOML 仅做语法解析；未执行 CLI 或 QEMU。
- `git diff --check` 检查修改空白。

以上证据只说明静态检查结果，不代表 guest 已启动或退出路径已经过实测。

## 仍需实现

1. Arm EL2、Intel VMX、AMD SVM、LoongArch LV、POWER HV 等实际后端；当前只有 RISC-V 后端。现有其它架构 stub 仍返回不支持。
2. 完成已加入的 CPU-local 激活、kick/pause/stop、中断注入与设备复制的运行验证，并实现同步远程 TLB 回收、VMID 分配器、同一 VM 多 vCPU、SBI HSM/IPI 和完整 IRQ controller。
3. 正式 memslot/VMO、HVA/mm 失效通知、共享页面借用账本、stage-2/device 内存与 CPU 完整配额。
4. `/dev/kvm` device/VM/vCPU fd、ioctl、mmap run page、寄存器 ABI、MMIO/PIO exit/completion，以及固定 QEMU profile 的 Linux ABI 依赖审计。不能报告 KVM API 12 或 QEMU/KVM 可用。
5. 用户态设备模型、virtio，以及已加入的 guest 监督/独立控制协议的故障与重连验证。
6. 授权后做全量构建，以及重复启动、双 VM 控制台隔离、长时间运行、关闭竞态、资源回收、FP/CSR 隔离和真实硬件验证。

## 授权后运行门禁

先执行原有 hypervisor smokes，特别是无 H 拒绝与 guest shell；再做同一宿主连续启动/销毁 100 次并检查 cgroup 和帧回收。随后用两份 `hypvm --managed` 进程配独立 PTY，交替输入不同标记，检查输出串线和 FP/CSR 交叉污染。最后测并发 close/RUN/config、低内存失败回滚和托管 guest 忙循环时宿主调度。未完成这些门禁前，不把静态实现标为生产可用。

## 后续代码进度（仍无运行验收）

- 运行占用改为每 CPU 的 vCPU 指针，显式禁止 load/enter/put 之间迁移，移除汇编全局 vCPU 指针。每 vCPU/每 VM 的执行占用仍互斥。
- Native/Linux 新增共享控制：918/0x0F0B 为 kick/pause/resume/stop，919/0x0F0C 为 software/timer/external IRQ 注入，920/0x0F0D 为回收统计。远程控制只排队，所属 CPU 在 trap/entry 时处理，使用宿主 reschedule IPI 唤醒。
- RISC-V 保存恢复 hvip，修正 guest SFENCE emulation 为 HFENCE.VVMA，移除不存在的 hstatus.VIE 并设置 VSXL=RV64。
- 设备访问 RAM 通过 backend pin 在叶锁下持有 frame 引用，再执行分段复制；不把可变 stage-2 返回的裸地址保留到复制之外。
- `hyp_stats` 返回 VM/vCPU/映射/page-table 页数和宿主空闲页数，供静止点回收门禁使用。多个字段不是事务快照，活动并发期间不能据此断言泄漏。
- 新增 `a20-vmd`、init opt-in 启动、专用 virtio-serial 通道、VM profiles 和 `tools/a20 vm` 操作。manager 资源上限为 8 个记录、512 MiB 总 guest RAM；单 VM 8–256 MiB。
- 新增 `tools/hyp_pair_probe.py` 真实场景门禁源码；本轮仅做 AST 检查，未执行。它覆盖两台 RAMFS guest、命令执行、串线检测、重连、pause/resume、关机、destroy 和四类资源计数回归，不覆盖 SMP、KVM 或长稳。

| 阶段 | 当前证据与欠缺 |
|---|---|
| P0 | 根表、CSR/FP、释放与统计代码已修改并静态检查；全量构建、100 次重复启动和回收实测缺失 |
| P1 | 共享核心、CPU-local 激活和独立队列已编码；双 guest 真实运行证据缺失 |
| P2 | 启动宿主/创建双 guest/独立交互/重连的代码路径和运行门禁已加入；端到端验收缺失 |
| P3 | 控制、IRQ 注入、设备 RAM pin/copy 的基础已编码；同 VM 多 vCPU、HSM/IPI、IRQ controller 与时序验收缺失 |
| P4 | AArch64、Intel VMX、AMD SVM 实际后端与分别验收尚未完成 |
| P5 | A20 `/dev/kvm` 与 QEMU/Linux disk/network guest 尚未完成 |
| P6 | 其它架构和真机、性能、长稳、故障隔离验收尚未完成 |

完整 P0–P6 目标保持开放；任何阶段都没有仅凭单文件检查标为完成。

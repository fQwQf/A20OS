# DELIVERY 缺口

对应 V1-DELIVERY。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### DELIVERY-01 扫描候选与台账的关系

- 现状：`tools/v1_requirements.py` 扫描出约 2,500 条候选（源码注释、TODO 字样等），其中 11 条人工映射；真正的待办来源已经是 `docs/status/gaps/`。
- 缺口：候选清单没有逐条归类。但**这不是 1.0 缺口**——扫描器按关键词抓取，不等于未完成需求；逐条 triage 是记账工作，不改善系统。
- 关闭条件：不要求清空候选。只需保证：(a) `gaps/` 是唯一待办来源；(b) 扫描器不把台账本身当候选（已按 `gap_ledger` 分类）；(c) 需要新增缺口时从候选里人工挑选并写明依据。其余候选保持 `needs_triage`，不阻塞发布。
- 来源：`v1.0-requirements.json`、[requirements-2026-10-14.md](requirements-2026-10-14.md)。

### DELIVERY-02 没有可复现的干净候选

- 现状：`tools/a20 run qemu-riscv64` 的 shell 门禁在带未提交改动的工作树上通过；没有干净候选。
- 缺口：缺一个从干净提交出发、可重复构建并经门禁验收的候选。
- 关闭条件：在干净树上重建镜像与内核，普通 shell 门禁（进入 shell、执行真实命令、`poweroff` 自然 rc=0）通过，并记录重建输入。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### DELIVERY-03 云端 CI 未实跑

- 现状：本地门禁可运行；Native CI 与发布前置工作流没有在配置的云端 CI 上取得完整结果。
- 缺口：候选版本的一次完整工作流结果未知；被跳过的硬件专属 job 未分类。
- 关闭条件：候选提交上的工作流完整跑完；每个 skipped job 都有分类，且 skipped 不计入覆盖。
- 来源：requirements N02。

### DELIVERY-04 保留平台构建矩阵未复核

- 现状：宿主构建矩阵只覆盖 AArch64、ARM32、LoongArch64、RISC-V32、RISC-V64、x86_64 的一部分组合。
- 缺口：64 位主线与 ARM32/RISC-V32/ARMv7-M 的工具链档位没有按固定版本复核；构建与运行证据未区分。
- 关闭条件：从记录的工具链版本跑一遍保留矩阵，逐项标注 build/runtime/N/A。
- 来源：requirements N20。

### DELIVERY-05 smoke 门禁仍以单 CPU 为主

- 现状：多数 smoke 默认 `NR_CPUS=1`；SMP 变体只覆盖部分用例。
- 缺口：适用门禁没有推进到 SMP2+，并发的失败路径缺少门禁。
- 关闭条件：代表性 smoke 有 SMP2 变体并单独保留结果，原单 CPU target 语义不变。
- 来源：requirements N18。

### DELIVERY-06 测试基线未建立

- 现状：`check-a20-tests` 在带改动的工作树上报告过 failures/errors/skip，未在干净基线上重跑。
- 缺口：不知道这些失败是既有的还是新引入的，无法作为回归基线。
- 关闭条件：在干净提交上跑一次并记录分类（既有/新引入/环境）；之后每轮只比较增量。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### DELIVERY-07 服务端运维项的范围决策

- 现状：没有 kdump 后端、硬件 watchdog、A/B 分区、dm-verity、硬件 PMU/ftrace、virtio-fs/DAX、真 RTC/paravirt clock、NUMA/热管理/C-states。
- 缺口：这些来自服务器就绪度评估的 P1–P3 排序，**不自动属于 1.0 承诺**。
- 关闭条件：先做范围决策——纳入的项拆成可实现、可门禁的条目并保留在本台账；不纳入的项从台账删除并写入 [releases/v1.0.md](../../releases/v1.0.md) 的范围说明。不允许既不实现也不表态。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八。

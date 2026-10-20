# DELIVERY 缺口

对应 V1-DELIVERY。现状见 [current/delivery-baseline.md](../current/delivery-baseline.md)。

### DELIVERY-03 云端 CI 未实跑

- 现状：`ci.yml` 的宿主侧门禁 job 已在候选上按 job 的 `run:` 步骤本地复现，内核构建矩阵也已逐架构跑过（arm32 与 ARMv7-M 在固定 CI 容器里跑）；`build`/`native`/`cluster-runtime`/`smoke-riscv64` 四个重量级 job 只在宿主机上单独验证了其中一部分（riscv64 dev-build 与 shell 门禁、arch/MMU 矩阵），没有整体跑过。云端派发本身没有发生：它需要把候选推到远端，而本工作包的规则明确禁止 push。逐 job 状态与被跳过的步骤分类见 [current/delivery-baseline.md](../current/delivery-baseline.md) §6。
- 缺口：候选版本的一次**完整**工作流结果仍然未知，也没有任何一条结果来自真实云端运行。此外，即使云端派发成功，`toolchain-gates` job 也会在候选上失败——该 job 的 24 个门禁有 11 个为红，其中 9 个属于其他领域（见各领域台账与本页下方"跨领域红灯"），不是本工作包能单方面修好的。
- 关闭条件：候选提交上的工作流完整跑完；每个 skipped job 都有分类，且 skipped 不计入覆盖。这需要一个有 push 权限的 CI 操作者，并且 §6 列出的跨领域红灯先被各自 owner 修掉。
- 来源：requirements N02。

## 跨领域红灯（本包只报告，不改判定）

`toolchain-gates` job 本地复现时失败、且根因在别的领域的门禁，已按领域追加到对应台账：

| 门禁 | 根因领域 | 台账 |
| --- | --- | --- |
| `check-a20-tests` 的部分用例 | MM / NET / CORE | [mm.md](mm.md)、[net.md](net.md)、[core.md](core.md) |
| `check-honesty-policy` | NET | [net.md](net.md) |
| `host-tests`（`check-proc-wait-window`）、`check-task-lifetime-boundary` | CORE | [core.md](core.md) |
| `check-abi-boundary`、`check-envelope-coverage`、`check-task-state-boundary` | ABI / IPC | [abi-ipc.md](abi-ipc.md) |
| `check-arch-boundary` | CORE / MM / ABI | [core.md](core.md) |
| `check-stm32f103`（硬编码 MMIO 地址） | DRIVER / 平台 | [drivers.md](drivers.md) |
| `check-doc-drift` 的剩余引用、`check-stm32f103` 缺 `rg` | 文档与门禁自身 | [docs.md](docs.md) |

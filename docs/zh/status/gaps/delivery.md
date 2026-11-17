# DELIVERY 缺口

对应 V1-DELIVERY。现状见 [current/delivery-baseline.md](../current/delivery-baseline.md)。

### DELIVERY-03 当前候选尚未完成云端 CI

- 现状：当前工作树尚未冻结成发布候选，也没有该候选的完整云端结果。2026-11-17 已只读核对旧远端提交 `4031cb9c` 的真实 CI：整体 failure，跳过步骤不计覆盖；同时确认 8 个构建镜像发布/自检与 STM32 云端依赖门禁早已完成，见[远端核对记录](../current/buildenv-publication.md)。历史本地重现、旧云端运行都不能认证当前候选；本包继续遵守不 push 的约束。
- 缺口：当前工作树尚未形成具有完整云端结果的候选提交。本地复验不计作云端执行，镜像发布依赖已按远端记录关闭，后续候选仍须记录实际使用的镜像身份。
- 本地现状（2026-11-16）：`check-a20-tests` 最新运行 1628 项，其中 1626 通过、2 跳过（`check-a20-tests-uxfs-root-cache-final.log`）；`host-tests`、`check-abi-boundary`、`check-smoke-cases`、`test-signal-core` 均通过。历史的 24 门禁 / 11 红灯统计不再代表当前工作树；`check-arch-boundary` 等历史入口已退出当前 Make/CI，不能继续把它们写成当前 CI 失败。保留故障与覆盖边界见 [本轮进度](../current/v1.0-progress.md#本轮完成与验证2026-11-16)。
- 关闭条件：候选提交上的工作流完整跑完；每个 skipped job 都有分类，且 skipped 不计入覆盖。需保存远端候选提交、工作流和各 job 的真实结果。
- 来源：requirements N02。

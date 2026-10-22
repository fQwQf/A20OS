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
| `check-doc-drift` 的剩余引用、`check-stm32f103` 缺 `rg` | 文档与门禁自身 | [docs.md](docs.md) DOCS-04/05 |

## DELIVERY-05 已发布的构建镜像缺 armhf 开发头与 rustc

- 现状：`tools/ci/Dockerfile` 已改为“共享 base + 每架构一个 target”，并显式装上
  每个交叉架构的 `libc6-dev-*-cross`（交叉 gcc 只把它列为 Recommends，而构建用
  `--no-install-recommends`，所以旧镜像里编译器在、`#include <limits.h>` 就死）与
  Rust（`rustc`/`cargo`/`rust-src`/宿主 std，以及各架构的 no_std target）。
  本地 8 个 target 全部构建并通过 `tools/ci/image-selftest.sh`；用新的 `arm32`/`riscv`
  镜像、不挂宿主工具链，已经复现出 `check-arm32-bringup` 与
  `check-riscv32-bringup` 与 `check-arm32-bringup`（当时它们的真实错误清单在 MM/net/fs
  的 32 位条目里，这些条目在本轮已关闭）。
  **但 ghcr 上的 `:latest` 仍是旧内容**——推送要靠 `buildenv.yml`，本轮没有执行。
- 缺口：在重新发布之前，CI 里任何容器内内核构建仍会因缺 `rustc` 直接失败（Error 127），
  而 arm32 仍会停在 `bits/libc-header-start.h`；也就是说“镜像齐全”目前只是仓库内的
  事实，不是 CI 拿到的事实。
- 关闭条件：跑一次 `buildenv.yml`（或用 `tools/ci/build-image.sh` 构建后推送）刷新
  ghcr 上的 8 个 tag，然后在一个容器 job 里确认 `check-<arch>-bringup` 不再报工具链缺失
  （源码本身的失败另计）。
- 来源：32 位 bring-up 的 arm32/riscv32 复现。

## DELIVERY-06 armv7m flat-user 缺少 Rust target

- 现状：`make check-mcu-program-rtl` 的 Verilator 构建完成后，ARMv7-M 固件
  配置在 `tools/rust-kernel.mk:1613` 失败：`CONFIG_FLAT_USER_MEMORY=1` 时
  没有 `RUST_FLAT_POLICY_TARGET_armv7m`，构建直接 `$(error ...)`。
- 缺口：MCU 原生应用/RTL 联合验收因此无法在当前树构建；这不是 cluster
  源码或测试断言失败，而是全局构建规则缺少 ARMv7-M 的 Rust flat 内存策略
  目标。
- 关闭条件：为 `ARCH=armv7m` 提供可链接的 flat-policy Rust target（或在该
  配置下明确排除 Rust flat 策略并由 owner 记录等价保证），然后重新运行
  `make check-mcu-program-rtl` 并保存实际 guest/RTL 结果。
- 来源：CLUSTER 1.0 验收尝试；本包不修改全局构建规则。

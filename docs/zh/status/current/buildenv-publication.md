# 构建镜像与 STM32 云端依赖核对

2026-11-17 只读核对 GitHub Actions、固定提交的工作流/自检源码及 GHCR manifest。
原 DELIVERY-05、DOCS-04 的“镜像未发布、真实 job 尚未执行”已不符合远端事实。
本页关闭这两项环境缺口，当前工作树的候选 CI 仍归 N02/DELIVERY-03。

## 已发布镜像

[buildenv 42084555154](https://github.com/fQwQf/A20OS/actions/runs/42084555154)
于 2026-11-05 18:28–18:34 UTC 完成，提交为
`4031cb9cded435e39630fb2b1e9bda55816fb920`。

| target / tag | job | 推送 | 拉回镜像自检 |
| --- | --- | --- | --- |
| base | 127446045872 | success | success |
| riscv | 127446045760 | success | success |
| aarch64 | 127446046022 | success | success |
| x86_64 | 127446045924 | success | success |
| loongarch64 | 127446045768 | success | success |
| arm32 | 127446045362 | success | success |
| armv7m | 127446046221 | success | success |
| multi / latest | 127446046573 | success | success |

[该提交的工作流](https://github.com/fQwQf/A20OS/blob/4031cb9cded435e39630fb2b1e9bda55816fb920/.github/workflows/buildenv.yml)
在 push 后执行 docker pull，再在发布镜像里运行自检，结果来自云端而非本地构建缓存。
[自检源码](https://github.com/fQwQf/A20OS/blob/4031cb9cded435e39630fb2b1e9bda55816fb920/tools/ci/image-selftest.sh)
要求 `rg`、rustc/cargo，实际编译目标 C 对象、预处理 `limits.h`/`stdint.h`，并检查
对应 Rust core/target；RISC-V32 还实际构建自定义 target 的 core。

arm32 日志明确记录 Rust 1.96.0、armhf libc headers 和 armv7a Rust target 成功，
multi 日志同样记录各架构头文件检查及最终 PASS。2026-11-17 的只读 registry 查询
确认 8 个 tag 均存在；arm32 与 latest 的 amd64 镜像 manifest 分别仍是
`sha256:e5278a023f1bfde08db7fa2e1c046759d194bf7dfc73d95611b2ce7a80dfe503`、
`sha256:32cae0bc33938e8eebc638fc6d25ec53ba4d266c49e86b92d879c24608d6897f`，
均与对应发布日志中的 manifest 一致。它们是交叉构建容器的宿主架构，不是目标板 ISA。

## 实际 STM32 job

[CI 42084555349 的 kernel-build-arm32](https://github.com/fQwQf/A20OS/actions/runs/42084555349/job/127446193221)
实际完成了 `STM32F103 source and firmware gate`，日志于
2026-11-05 18:31:13 UTC 输出 `check-stm32f103: PASS`。

随后 `check-arm32-bringup` 失败于源码构建的 `lwip/tcp.h` 缺失；该 job 的失败不改写，
但已不再是 rustc、armhf libc 开发头或 ripgrep 缺失。该 CI job 与 buildenv 并行，
拉取的是当时较早的 latest index `sha256:aea8ac96ec586623de4832e5c83e8612136164157b17786771c5c9b819976df3`，
不声称它使用了稍后才发布完成的新 index。

## 仍保留的候选验收

上述 CI 的整体 conclusion 是 failure，后续被跳过的步骤不计通过；它验证的是旧远端
提交，不包含 2026-11-17 本地修复。当前候选的 N18/N20 本地矩阵已按[候选记录](v1.0-candidate.md)关闭，仍须取得
自己的云端结果（N02）。此轮没有 push、派发 workflow、推送镜像或重跑整个 CI。

原始 job 元数据、选定日志、固定源码及 8 个 registry manifest 保存在
`build/evidence/v1-closeout-20261116/cloud-image-audit/`；索引与哈希见其 `report.json`。

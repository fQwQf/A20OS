# 碎片提交进一步整理（2026-10-08）

按用户继续 squash 的要求，对 `v0.13..main` 中内容属于同一问题且拓扑允许的碎片提交重新审阅。本轮将 19 组、41 个提交整理为 19 个提交，减少 22 个中间节点。按独立问题保留其他提交；没有把全部线性历史压成大提交。

输入 main 为 `3f31cafd853345cd334814bc8077d13f391eb75f`，应用后的映射 main 为 `d6db006828c13a6555f4a55cf844aa74f1f575a9`。这里的统计不包括随后新增的本次迁移文档提交；该维护提交使用实际时间。

## 分组与取舍

| 组 | 输入提交（从旧到新） | 输出提交 | 统一消息标题 |
| --- | --- | --- | --- |
| 1 | `103bd0ce011b` → `93774479643e` | `7014c5d4c3e9` | fix(unix): wake stream readers after each delivered chunk |
| 2 | `3e5e9672fc5a` → `a8b69c7b031c` | `6a36acae7104` | fix(futex): break COW before PI futex stores |
| 3 | `7d9bea22f384` → `d7536324c753` | `3f698c4bbbc0` | feat(audio): expose PCM ioctls and attach HDA to XFCE |
| 4 | `6fc7f01e5cea` → `70c80c202d68` | `7e6db86a3806` | fix(drm): assign framebuffer IDs independently of GEM handles |
| 5 | `fc86c94f53dc` → `4a0f2fc7240d` | `6a951eb488c8` | test(gates): migrate source assertions into declarative gates |
| 6 | `94fb71e13863` → `42de401c1532` | `5d7eef30cc78` | feat(mm): provision anonymous status and preserve mprotect permissions |
| 7 | `70b54650ecbe` → `1223551ac5d4` | `9f4ff32bf934` | fix(mm): refresh per-PTE protection through the leaf-table lookup |
| 8 | `11e4c7093664` → `e92bbc051cc1` | `1be5e4d0799d` | build(tools): retire source-built userland in favour of apk worlds |
| 9 | `7b00ddc1386c` → `58f3f43c731a` → `b57506128449` → `3c92a690eb3d` | `dd3c9e116d10` | docs(drm): record the timestamp capability rollback |
| 10 | `8d5fc6f5e119` → `5cf0a883cdd5` | `638bc08a4006` | test(smoke): add x86_64 and aarch64 virgl readback gates |
| 11 | `1c84f256da80` → `1702e7646c89` | `0c0b356cbf97` | docs(graphics): record 3D coverage and the Mesa attach status |
| 12 | `a396a2a3f0cd` → `6e34909185ff` → `02cc8cf9a48f` | `19a40fd94ec6` | fix(gfx): enable and verify Mesa virgl attachment |
| 13 | `226b4383b977` → `73cb74c03537` | `fe5dbf3082cf` | docs(net): consolidate the loopback pbuf lifetime investigation |
| 14 | `a4d253180507` → `3ae34ba13b85` | `72767606fb8c` | docs(server-readiness): record server boot evidence and corrected TCP findings |
| 15 | `94ca523174f2` → `fbbcbbd9797c` | `13e9d1f2ca4c` | fix(mm): synchronize PTE status and validate page-table metadata |
| 16 | `a971eaa58e8b` → `1c66d9bef2b1` | `ed4e4a1b00c7` | feat(mm): provision segment indexes and preserve mapping policy |
| 17 | `2ac869003f66` → `8830ff54894a` | `9436f0b4b6d1` | docs(net): align network guidance and correct embedded budgets |
| 18 | `0eda885b32ec` → `9c75c02ba53a` | `160a7b2376c6` | docs(recovery): record integration acceptance and merged-worktree cleanup |
| 19 | `26a732ae52df` → `600f6ac86fdf` | `b443474a4849` | docs(recovery): record lane-lock and guest-shell acceptance and synchronization |

完整旧、新 SHA 见 [map.tsv](map.tsv)，逐组理由与完整消息见[显式计划](plan.json)。每组均审阅实际补丁，检查全部本地 refs 的孩子节点、作者身份及标签/ref 端点，而非只看 main 的第一父链。

这些分组包括诊断与对应修复、实现后的即时正确性补完、同类架构门禁、连续文档纠错，以及一次 DRM 时间戳能力尝试及回退。最后一种合并后保留工作状态和实验结论，不保留导致桌面无法启动的中间状态作为独立提交。

没有合并的典型例子：

- memp accounting 的前节点 `81118962ecc8` 是分叉点；TCP 调查的 `73cb74c03537` 也保留为末尾分叉节点，没有与其后 `a2dc01ff131c` 合并。本次允许将 `226b4383b977` 折入 `73cb74c03537`，但保留后者的两条出边。
- 通用 QEMU 命令解析修复 `fa2f73f73d00` 影响其他 smoke 目标，保持独立；只合并前面的两个架构门禁。
- SUBMIT_3D 截断、资源 attach/readback 与 mutex teardown 分别解决不同缺陷，保持独立。Hypervisor runner 与交互 shell 也是不同能力阶段。
- 综合 RAM/video/GBM 状态检查点 `f5492950eb85` 与 sysfs 实现保持独立；NAT 两个相关提交的作者身份不同，也没有跨身份合并。
- 历史清理、消息规范化、身份更正三个阶段有各自迁移证据，保持独立维护提交；不为减少数量而丢失操作边界。

本节输入 SHA 对应仓库外的 `before.bundle`；当前 SHA 通过完整映射查询。逐组图检查见 [group-preflight.json](group-preflight.json)，全图保护清单见 [topology.json](topology.json)。

## 不变条件与验证

全部本地可达历史从 2197 个节点变为 2175 个；92 个 merge、69 个分叉、31 个 refs 和 16 个 annotated tags 保留，父节点顺序按计划映射。`v0.13` 及此前共 683 个节点的 SHA 完全不变。

每组最后节点提供原始 author、committer、作者时间、提交时间、时区和最终 tree。未 squash 的保留节点同样保持原始两种身份和时间；只允许父 SHA 的传播变化，正文只在 19 个合并节点上按计划重写。被删节点的原信息在离线 bundle 与映射中保留，不声称已删除节点的日期仍存在于新图。

所有保留节点的 tree 及全部引用端点的最终 tree 均与输入相同。源代码没有改变；本次维护提交只更新迁移文档和工具使用说明。没有重跑运行时启动测试，验证集中于历史对象和文档：

- [引擎校验](validation.json)验证声明分组、父边、身份/时间、树和 refs。
- [独立递归复核](independent-validation.json)核对全部保留节点元数据、消息、merge/fork、标签及文件树。
- [原始对象复核](strict-validation.json)独立重建预期 bytes 并计算 SHA-1，检查保留节点一一对应、所有引用最终树相同及 v0.13 SHA 不变。
- 18 项历史工具回归和 `make check-doc-drift` 通过，见[维护检查](maintenance-validation.json)。

上一轮[粒度报告](../identities/granularity-review.md)的 COW 示例曾误写为网络门禁提交 `cca22eca2bf1`；本轮修正为输入 `0262ddc85cfd`（输出 `3fff3d60ccf7`），实际补丁为 12 文件的 COW 正确性修复。旧报告的统计仍按当时快照保留。

## 离线备份与复现

仓库外 `/home/fqwqf/OS/squash-followup-20261008/before.bundle` 保存本轮输入全部 refs 与完整历史，SHA-256：`0de775bb31cdbd87dc579d192b8feb5368e5df78a53d7bea7a1c5f324c80537d`。最终状态另存同目录 `final.bundle`。此前各阶段的 bundle 继续保留。

```sh
git clone --mirror /path/to/squash-followup-20261008/before.bundle /tmp/a20-before-squash.git
python3 tools/history_rewrite/engine.py preview \
  --repo /tmp/a20-before-squash.git \
  --output /tmp/a20-squash-preview.git \
  --plan docs/history/2026-10-08/squash-followup/plan.json \
  --artifacts /tmp/a20-squash-evidence
python3 tools/history_rewrite/actual-verify.py \
  --repo /tmp/a20-before-squash.git --mirror /tmp/a20-squash-preview.git \
  --results /tmp/a20-squash-evidence \
  --plan docs/history/2026-10-08/squash-followup/plan.json \
  --refs-before docs/history/2026-10-08/squash-followup/refs-before.txt \
  --topology docs/history/2026-10-08/squash-followup/topology.json \
  --expected-commits 2197 --expected-groups 19 --expected-removed 22 \
  --expected-merges-main 57 --expected-merges-all 92 --expected-forks 69 \
  --validation /tmp/a20-squash-independent-validation.json
```

发布仅更新发生变化的 main 与公开 release tags，使用原远程 SHA 的显式 lease 和 atomic push。随后从远程重新拉取 main 与全部 release tags，核对端点、对象和 fsck；不发布私有研究、备份或 stash refs，不修改服务器维护的 PR 引用。完整发布及清理日志留在上述仓库外目录。

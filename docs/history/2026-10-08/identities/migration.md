# Sisyphus 身份更正（2026-10-08）

用户确认 Sisyphus 的提交实际均由 fQwQf 完成。本阶段将精确身份 `sisyphus <sisyphus@local>` 更正为仓库当前使用的 `fQwQf <supertjz123@foxmail.com>`，共涉及 13 个 author 和 13 个 committer 字段。此前删除不实共同作者 trailer 的操作与本次 author/committer 更正是两个独立阶段。

## 保留范围

预演覆盖全部 31 个本地 refs 的 2196 个提交，其中 599 个因身份或祖先变化而重新计算 SHA。92 个 merge、69 个分叉和 16 个 annotated tag 保持原图关系；没有 squash 或删节点。各节点的原作者时间、提交时间及时区分别按字节保留，正文、tree、其他身份与 headers 均保持不变。标签仅映射目标 SHA，tagger、日期和说明不变。

`before.bundle` 包含 31 个 refs 与额外的 HEAD 记录，因此 Git 显示 32 个端点。GitHub 的 `refs/pull/1/head` 是另一个由服务器维护的远程引用，不在本地 refs 中；本次发布仅更新发生变化的 main 和公开 release tags，不修改该 PR 引用，也不上传本地研究、备份或 stash 引用。

本次维护提交新增工具和审阅记录，使用实际提交时间。上面的“不改时间和 tree”保证适用于其之前被映射的 2196 个节点。

## 可复核制品

- [精确身份计划](plan.json)、[完整提交及标签映射](map.tsv)、[原引用](refs-before.json)、[新引用](refs-after.json)。
- [工具验证](validation.json)逐节点检查两种时间、正文、tree、其他元数据、父边顺序及 ref 映射；另以 `cat-file --batch` 和独立 SHA-1 计算核验全部 2215 个映射对象，见[独立验证](independent-validation.json)。
- [近期提交粒度审阅](granularity-review.md)记录两组可考虑合并的文档修订，以及因分叉点而不能合并的两组相关改动。本阶段只审阅粒度，未进一步 squash。

离线旧历史保存在仓库外 `/home/fqwqf/OS/identity-rewrite-20261008/before.bundle`，SHA-256 为 `09e0c6cc7b5c64a4dd27b32ee90d1db721f30d810057e8041232c7f6d870f977`。这份 bundle 属于消息规范化之后、身份更正之前，不能用更早的历史清理 bundle 替代。最终状态另存同目录 `final.bundle`。

## 复现与发布检查

```sh
git clone --mirror /path/to/identity-rewrite-20261008/before.bundle /tmp/a20-before-identities.git
python3 tools/history_rewrite/identities.py \
  --repo /tmp/a20-before-identities.git \
  --output /tmp/a20-identities-preview.git \
  --plan docs/history/2026-10-08/identities/plan.json \
  --artifacts /tmp/a20-identities-evidence
python3 -m unittest discover -s tools/history_rewrite -p 'test_*.py' -v
make check-doc-drift
```

18 项历史工具测试、文档漂移及引用检查均通过，见[维护检查](maintenance-validation.json)。身份映射没有修改运行时源码，因此本阶段不重复运行内核启动测试。

工具仅生成隔离 mirror 与证据，不更新源 refs 或推送。发布使用原远程 SHA 的显式 lease 与 atomic push；发布后单独拉取 main 和 release tags，核对端点、对象、身份及 fsck。完整离线日志保存在上述仓库外目录；仓库内记录的 refs-after 是新增维护提交之前的身份映射结果。

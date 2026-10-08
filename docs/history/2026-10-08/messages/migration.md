# 提交信息规范化与署名纠正（2026-10-08）

本阶段在文件清理与 squash 完成后，统一全部本地引用可达的提交信息。保留每个现有节点，保持原始文件树、父边顺序、分叉与合并关系，不再 squash。消息采用贡献指南中的 `type(scope): description`；标题使用英文祈使句，正文概括实际改动、原因与必要验证事实，删去冗长日志和过程性复述。

## 结果与边界

- 2,195 个现有 commit 一一映射，没有删除或合并节点；改写 2,166 个标题，整理 356 条正文。
- 92 个 merge、69 个分叉节点保留，全部 31 个引用映射，包括本地分支、远程跟踪引用、stash、标签及内部 tree 引用。
- 原始 author、committer 的身份、Unix 时间和时区逐字保留；所有非 parent 的 commit headers 完全相同，所有 commit tree SHA 完全相同。
- 16 个 annotated tag 的目标随历史映射；tagger、时间、说明不变。v0.1–v0.16 均映射至新历史。
- 移除 33 条 Claude Opus/Anthropic 与 2 条 Sisyphus 不实 `Co-authored-by` 行。这些行经用户确认不属实；没有替换原 author 或 committer。
- 文件清理后变空的论文产物提交继续保留，消息说明其历史用途。未完成的 PowerPC64LE bring-up 保留未完成状态，避免把脚手架描述为可用平台。

原 main `8a8abd6aedd489356d1375b87f53b113f39a2607` 映射为 `d9e177bf1e17f2008fd6af2ac254e34cc73be0ff`。本说明、贡献规范和工具随后单独提交，使用新增维护提交的实际时间，不冒用历史时间。运行时代码没有变化。

## 映射与复核

[map.tsv](map.tsv) 记录本阶段所有 commit 和 annotated tag 的旧、新 SHA；[消息计划](../../../../tools/history_rewrite/plans/messages-20261008.json) 保存逐节点审阅后的完整消息计划，与预演输入逐字相同；[refs-before.json](refs-before.json) 和 [refs-after.json](refs-after.json) 保存迁移主体的引用快照，不包含随后新增维护提交。

查找第一阶段之前的旧 SHA，先查[第一阶段映射](../map.tsv)，再查本阶段映射。旧文档中的历史 SHA 与历史判断不据此改写成现在的结论；对应对象可从离线备份恢复。

全图验证见 [validation.json](validation.json)。独立实现另使用批量原始对象读取与 SHA-1 重新计算，逐字核对预期对象、全部父边与所有引用，结果见 [independent-validation.json](independent-validation.json)。消息计划作为迁移工具的输入数据保存于 `tools/history_rewrite/plans/`；其中包含原消息对门禁负向测试的引用，不属于当前文档主张。消息计划保持可直接审阅的 JSON 文本，没有另加压缩包。

两次验证均确认 35 条不实署名已消失、元数据与文件树不变、映射为双射。

工具与复现命令见 [README](../../../../tools/history_rewrite/README.md)。回归测试检查元数据、分叉/merge、空提交、stash、嵌套和 tree 标签、直接 tree/blob 引用、真实共同作者保留、签名拒绝与碰撞拒绝。配套维护改动还运行 `git diff --check` 和 `make check-doc-drift`；完整运行时代码构建不作为本次纯消息迁移的验证依据。

## 备份与同步

迁移前完整历史保存在仓库父目录的 `message-rewrite-20261008/before.bundle`，SHA-256：

```text
f0e6d82505fe9986e2c17447b834552125a44765e3c14ee4d5d65b6683ea8122
```

该 bundle 包含旧署名，只用于离线恢复，不能把其中旧分支重新推送回已迁移远程。上一阶段的原始 bundle 也继续保留。协作者应先保存未推送工作，再重新 clone；需要的本地改动在新历史上重新应用。

远程 main 与既有发布标签使用带每个旧 SHA 租约的原子 push 更新，避免覆盖迁移期间出现的新改动。GitHub 管理的 PR 引用和服务器缓存不属于可由本地 force-push 改写的分支、标签范围；本阶段的对外历史检查针对发布分支与发布标签。

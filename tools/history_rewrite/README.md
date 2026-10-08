# 离线 Git 历史迁移工具

本工具用于 2026-10-08 的历史整理，约束与结果见 [迁移说明](../../docs/history/2026-10-08/migration.md)。它只在独立 mirror 中重建对象，不能直接更新源仓库或推送远程。

```sh
python3 -m unittest discover -s tools/history_rewrite -p 'test_*.py' -v
python3 tools/history_rewrite/engine.py preview --help
python3 tools/history_rewrite/actual-verify.py --help
```

`preview` 读取显式 squash/message/purge 计划；squash 仅适用于 v0.13 之后，路径过滤适用于全部引用。计划中的完整 SHA 对应迁移前的离线 bundle，不能直接用于迁移后的仓库。作者身份需在组内一致，保留最后节点的 author、committer、日期及最终文件树。merge、分叉、标签和引用端点不会被删除。

输出目录和证据目录须与源仓库隔离。源仓库引用在预演前后比对；若计划目录含 `refs-before.txt`，也会检查该快照。带签名的对象需要变化时拒绝处理；存在无路径 blob 引用时，路径过滤也拒绝继续。

`actual-verify.py` 是独立实现的只读复核器，比较原仓库与预演 mirror 的全部引用、元数据、父边和过滤后的子树。其默认计数属于本次迁移，复用时按 `--help` 指定预期计数与拓扑文件。所有旧、新 SHA 与检查证据应在应用或远程推送前保存。

使用迁移前 bundle 恢复的仓库完成预演后，可执行独立复核：

```sh
python3 tools/history_rewrite/actual-verify.py \
  --repo /tmp/a20-original.git --mirror /tmp/a20-preview.git \
  --results /tmp/a20-preview-evidence \
  --plan docs/history/2026-10-08/plan.json \
  --refs-before docs/history/2026-10-08/refs-before.txt \
  --topology docs/history/2026-10-08/topology.json \
  --validation /tmp/a20-independent-validation.json
```

## 提交信息规范化

同日的第二阶段仅改写消息，不 squash、不改树。规则与结果见[消息迁移说明](../../docs/history/2026-10-08/messages/migration.md)。`messages.py` 要求显式完整消息计划；源、预演 mirror 和证据目录相互隔离，不更新源引用，也不推送。

它保留原始 commit headers，仅替换父 SHA 与消息；拒绝签名对象和节点碰撞。验证逐字比较 author、committer（包括时间及时区）、tree 与其他 headers，检查父边顺序、完整图、tagger/标签说明、tree/blob 引用与共同作者署名。消息格式校验之外，标题和正文的事实性需要结合原补丁审阅。

本次明确确认不实的共同作者为 Claude/Anthropic/Opus 与 Sisyphus；工具只针对这些行进行检查，不推断其他作者的贡献。原 author/committer 身份不因此改动。

复核需要本阶段开始前的 `before.bundle`，不能使用上一阶段 squash 前的 `original.bundle`：

```sh
git clone --mirror /path/to/message-rewrite-20261008/before.bundle /tmp/a20-before-messages.git
python3 tools/history_rewrite/messages.py \
  --repo /tmp/a20-before-messages.git \
  --output /tmp/a20-messages-preview.git \
  --plan tools/history_rewrite/plans/messages-20261008.json \
  --artifacts /tmp/a20-messages-evidence
```

完整消息计划作为工具输入保存在 `plans/messages-20261008.json`，其中保留历史负向测试样例的原始引用，供消息逐字核对，不属于当前文档主张。

完整回归命令保持为本文件开头的 unittest discovery；新增用例覆盖不同作者/提交者与不同时间及时区、merge 父序、分叉、空提交、stash、嵌套标签、tree/blob 引用、真实共同作者保留、计划覆盖、签名拒绝和节点碰撞。

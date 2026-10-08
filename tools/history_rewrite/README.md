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

# v0.13 之后的历史整理与误提交产物清理（2026-10-08）

本说明记录文件清理与 squash 阶段的结果。同日后续又进行了[提交信息规范化](messages/migration.md)；查找当前提交时，应串联两个阶段的映射。

本次迁移整理 v0.13 之后的提交，并从全部本地分支、标签、远程跟踪分支及 stash 可达历史中清除误提交的产物。原始历史已另存为仓库外的完整 Git bundle；它仍含被清理的文件，不应重新推送。

## 规则

- 保留分叉点、merge 节点、原有父边顺序及引用端点。只在分支内部连续的单父链中，合并同一问题的实现、修复、验收和文档补完。
- 每个 squash 组沿用最后一个提交的完整 author、committer 与两种时间（含时区）。其余节点保留原始身份和时间。不同作者的工作分别保留。
- 消息描述最终聚合改动，撤销过时的诊断或验证声明；独立功能分别提交。
- 路径清理覆盖所有历史。v0.13 及更早的消息和拓扑不作语义整理，但如果其文件树含清理目标，则相应提交及后代 SHA、标签目标也会变化。标签的说明和 tagger 时间保留。
- 文件过滤不自动裁剪空提交或 merge，避免破坏原有分叉与合并关系。

## 清理范围

完整机器可读规则见 [plan.json](plan.json) 的 `purge` 部分。

| 类型 | 路径 |
| --- | --- |
| 会话计划 | `.zcode/`、`.zcodeignore` |
| 根目录依赖清单 | 根目录的 `*.d`；不按此规则删除子目录同后缀文件 |
| 下载与解包产物 | `qemu-10.0.13+ds/` 及根目录三个对应的 Debian 源码包文件 |
| 可再生成论文产物 | `docs/paper/main.pdf`，保留 TeX 源码 |
| 历史编译产物 | overlay 中的 `poweroff-helper`、`a20mount` ELF |

保留图片、字体等项目资源和有明确用途的二进制测试数据，例如 mlibc 的 `tests/rtld/fail-load/fake/fake-lib`。外部 QEMU 文件引用保留版本号与上游相对路径，QEMU 下载、解压和构建在仓库外进行；这些引用不再表示仓库内有对应文件。

## 外部 QEMU 源码定位

引用对应 Debian 源码包 `qemu` 版本 `1:10.0.13+ds-0+deb13u1`。将 `.dsc` 及其两个 tar 包下载至仓库外同一目录，再执行 `dpkg-source -x qemu_10.0.13+ds-0+deb13u1.dsc`。文中的 `qemu-10.0.13+ds/...` 路径相对于解包目录，不再是 A20OS checkout 内的文件。

以下 SHA-256 来自迁移前跟踪的源码包描述文件，用于定位原始证据版本；本次未重新下载或验证 Debian 签名：

| 文件 | SHA-256 |
| --- | --- |
| `qemu_10.0.13+ds.orig.tar.xz` | `6a0888e806c2ffc0c5f0733e974f9aea55491950d3c47c98669c8b236dde59ae` |
| `qemu_10.0.13+ds-0+deb13u1.debian.tar.xz` | `da73c092f5684c551b0334d675eb16a3dec91e1edea94759cfb1102fa4a0364e` |

## 查找旧提交

[map.tsv](map.tsv) 记录本次迁移之前所有可达 commit 和 annotated tag 对象的旧、新 SHA。多个旧提交指向同一个新提交，表示它们属于同一 squash 组。历史文档里的旧 SHA 可以通过此表定位；表中的新 SHA 指向迁移主体，后续新增维护提交不在表内。

## 验证与恢复

预演在隔离 mirror 中进行。逐节点验证作者和提交者原始头字段、父边映射及顺序、每个保留文件的 blob 和权限、标签说明与 tagger 元数据，并确认所有可达文件树均无清理目标。源码、测试数据及构建配置的内容只允许发生显式清理路径对应的删除；迁移后的维护提交另补忽略规则和文档。

迁移工具和临时 DAG 回归测试见 `tools/history_rewrite/`。测试包含分叉边界拒绝、merge 父序、标签、stash、时区、跨提交者 squash、缺对象 gitlink、嵌套路径、根目录 glob、旧版本污染、空提交保留和源仓库输出隔离。

```sh
python3 -m unittest discover -s tools/history_rewrite -p 'test_*.py' -v
```

若需复核原历史，请在新的目录恢复离线 bundle，不要将旧分支重新推送到已迁移远程：

```sh
git clone --mirror /path/to/original.bundle /tmp/a20-original.git
python3 tools/history_rewrite/engine.py preview \
  --repo /tmp/a20-original.git \
  --output /tmp/a20-preview.git \
  --plan docs/history/2026-10-08/plan.json \
  --artifacts /tmp/a20-preview-evidence
```

已有协作者应先备份未提交或未推送工作，再使用新的 clone 继续开发。不要把旧历史 merge 回 main；需要的本地改动应在新历史上重新应用。

远程更新采用带旧 SHA 租约的原子 push，拒绝覆盖迁移期间发生的远程新改动。分支与标签更新完成后，本地旧 reflog 和不可达对象会清理；离线 bundle 单独保留。托管平台对无引用对象的缓存与回收由平台控制，本次检查范围是可达的分支、标签及本地引用。

离线原始备份：`history-rewrite-20261008-_32ryr29/original.bundle`（位于仓库父目录），SHA-256 为 `5b2bd16ab70e64042edf4e04df146810a3e877f96794a7592d00a7babc55c77d`。`git bundle verify` 已确认它是可独立恢复的完整历史。

## 本次结果

- v0.13 之后：965 → 803 个提交，74 组 squash 合并掉 162 个线性节点；另改写 167 条保留提交的消息。
- 全部本地引用可达历史：2,356 → 2,194 个提交；92 个 merge 和 69 个分叉节点全部保留，其中 v0.13 后的 merge 为 57 个。
- 2,194 个保留节点的原始 author/committer 头字段与预期父边全部通过独立验证；16 个注释标签保留说明和 tagger 字段。
- 递归对照 13,932 组旧、新子树，非清理目标的路径、模式、类型与对象 ID 均相同。两个内部 tree 引用也完成过滤。
- v0.12、v0.13 受路径清理影响；v0.1–v0.11 的标签对象保持原样。v0.14–v0.16 映射到整理后的提交。
- 一个更早的 PDF-only 提交清理后变为空提交，按保持拓扑规则保留。
- 原 main `fe440e3917b84ba9032fe9816ab7843fd4f79a1a` 映射为 `4bcf1f748f3297e63a467e4629172675ff5e0bb3`；本说明及忽略规则作为随后新增的维护提交。

机器可读独立检查结果见 [validation.json](validation.json)，历史路径盘点见 [purge-audit.json](purge-audit.json)。17 MB 的干净完整 bundle 与 116 MB 的原始 bundle 均已通过恢复完整性检查。历史重建不改动运行时代码；本次新增维护改动只涉及文档、源码注释、忽略规则及离线迁移工具。

维护提交验证：8 项迁移工具回归通过；`make check-doc-drift` 通过（1,957 条仓库内行号引用检查）；16 项忽略规则正反例通过；两处 C 注释修改前后去注释 token 相同；`git diff --check` 通过。新忽略规则按类别匹配多个版本和生成文件名，不逐个枚举本次清理的文件。

引用重建时保留各端点的提交节点。之后按已合并分支清理要求，删除远程 `archive/legacy-desktop` 和 `feat/graphics-hardening`，它们的提交仍在 main 的祖先中；本次临时维护分支在快进合并后删除。其余本地分支均同步映射，原始引用快照见 [refs-before.txt](refs-before.txt)，保护点见 [topology.json](topology.json)。

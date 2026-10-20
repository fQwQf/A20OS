# 状态、进度与测量

本目录是 A20OS 的**状态类文档**存放处：当前进度、已知问题、子系统实现状态与测量协议。
`docs/` 的其余部分描述**接口、契约与用法**；本目录的内容不定义任何接口契约。

## 为什么单独成目录

状态类内容与参考类内容的时效性不同：参考文档描述"系统是什么"，状态文档描述"现在到哪一步、
还差什么"。混在一起时读者无法判断一句话是契约还是进度。把它们集中在这里，`docs/` 根目录与
各子系统目录就只剩可长期有效的参考材料。

## 结构

| 目录 | 内容 |
| --- | --- |
| `current/` | 当前状态：模块状态、已知问题、就绪度、各子系统实现状态、1.0 进度与待办 |
| `history/` | 已被取代的旧状态与验证叙述，仅保留结论性文字 |
| `measurements/` | 测量基线与测量协议 |

## 收录规则

- 只写**当前状态**：系统现在是什么行为、还差什么、下一步做什么。
- **不记录 commit SHA、不记录逐次运行结果**（PASS/耗时/原始日志/哈希清单）。这类内容描述的是
  某个过去提交上的某次运行，属于历史记录而非当前状态；它们不进入仓库。
- 唯一例外是 `measurements/`：`tools/lock_bench.py` 的基线与改造后数据要作为**同输入对照的
  参照**保留，否则测量结论无法复算。这里只放对照所需的数据与协议，不放逐次运行日志。
- 验收纪律仍然是流程要求：任何结论在作为当前事实引用前，必须按
  [testing-gates.md](../testing-gates.md) 的入口在**当前提交**上实际重跑。跑完的结论写进状态文档，
  运行产物本身不入库。
- 被新结论取代的旧文档移入 `history/` 并保留结论性文字，不保留运行细节。

## 当前状态索引

- [module-status-2026-10-19.md](current/module-status-2026-10-19.md)：已集成模块的实际行为与未决边界
- [known-issues.md](current/known-issues.md)：已知问题队列
- [server-readiness.md](current/server-readiness.md)：作为服务器部署时的能力边界与阻塞项排序
- [v1.0-progress.md](current/v1.0-progress.md)：12 个 V1 工作包的状态
- [v1.0-next-20.md](current/v1.0-next-20.md)：人工 triage 的 20 项待办需求（N01–N21）
- [v1.0-requirements.json](current/v1.0-requirements.json)：机器生成的源码条目候选清单
- 各子系统实现状态：[集群](current/cluster-implementation-status.md)、
  [驱动](current/drivers-implementation-status.md)、
  [Hypervisor](current/hypervisor-implementation-status.md)、
  [混合内核](current/hybrid-kernel-status.md)、
  [Native ABI 运行时](current/native-abi-runtime-status.md)、
  [图形](current/graphics-status.md)、[网络](current/net-status.md)、
  [文件系统](current/fs-status.md)、[平台](current/platforms-status.md)、
  [发行版与引导](current/distro-status.md)、
  [桌面与 Minecraft](current/desktop-minecraft-status.md)
- [validation-dontunmap.md](current/validation-dontunmap.md)：`MREMAP_DONTUNMAP` 仍待闭合的范围

发布要求与验收规范不在这里，见 [releases/](../releases/)；面向未来的计划见 [roadmap/](../roadmap/)。

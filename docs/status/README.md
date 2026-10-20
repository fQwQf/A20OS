# 状态、进度与证据

本目录是 A20OS 的**状态类文档**唯一存放处：进度台账、已知问题、子系统实现状态、验证记录、
测量数据与逐次运行的原始证据。`docs/` 的其余部分描述**接口、契约与用法**；本目录的内容不定义
任何接口契约。

## 为什么单独成目录

状态类内容与参考类内容的时效性不同：参考文档描述"系统是什么"，状态文档描述"某个提交上
发生了什么"。混在一起时，读者无法判断一句话是契约还是某一轮的运行结果。把它们集中在这里，
`docs/` 根目录与各子系统目录就只剩可长期有效的参考材料。

## 结构

| 目录 | 内容 | 时效 |
| --- | --- | --- |
| `current/` | 当前状态：模块状态、已知问题、就绪度、各子系统实现状态、1.0 进度与待办 | 随 HEAD 更新 |
| `history/` | 已被取代的进度与验证记录、历史实现笔记 | 冻结，不再更新 |
| `measurements/` | 锁竞争等测量基线与测量协议 | 冻结，附产生它的命令与提交 |
| `evidence/` | 逐次运行的原始日志、哈希清单、manifest | 冻结，字节级保留 |

## 收录规则

- 描述**当前**状态的进 `current/`；被新记录取代后移入 `history/`，不删除。
- 运行证据一律进 `evidence/`，保留原始日志与哈希，不因后续修复而改写。
- 记录一旦归档不再更新。若某项结论需要作为当前事实引用，必须按
  [testing-gates.md](../testing-gates.md) 的入口在**当前提交**上重新运行，不能继承历史结论。
- 路径与哈希的例外：当证据文件本身被移动时，只允许修正**哈希清单里的路径字符串**
  （哈希值不变，清单仍可 `sha256sum -c` 校验）。运行状态记录（如 `source-inputs.json` 里
  记录的当次 untracked 路径）保持原样，因为它是当时的事实。

## 当前状态索引

- [module-status-2026-10-19.md](current/module-status-2026-10-19.md)：七个已集成模块的实际行为与未决边界
- [known-issues.md](current/known-issues.md)：已知问题队列与历史根因排查
- [server-readiness.md](current/server-readiness.md)：作为服务器部署时的能力边界与阻塞项排序
- [v1.0-progress.md](current/v1.0-progress.md)：12 个 V1 工作包的首波状态
- [v1.0-next-20.md](current/v1.0-next-20.md)：人工 triage 的 20 项待办需求（N01–N21）
- [v1.0-requirements.json](current/v1.0-requirements.json)：机器生成的源码条目候选清单
- 各子系统实现状态：[集群](current/cluster-implementation-status.md)、
  [驱动](current/drivers-implementation-status.md)、
  [Hypervisor](current/hypervisor-implementation-status.md)、
  [混合内核](current/hybrid-kernel-status.md)、
  [Native ABI 运行时](current/native-abi-runtime-status.md)
- [validation-dontunmap.md](current/validation-dontunmap.md)：`MREMAP_DONTUNMAP` 当前仍是 open 的验证范围

发布要求与验收规范不在这里，见 [releases/](../releases/)；面向未来的计划见 [roadmap/](../roadmap/)。

## 证据校验

证据目录内的哈希清单按清单自身的相对基准校验，例如：

```sh
cd docs/status/evidence/releases/kernel-integration/mm-final-2026-10-19
sha256sum -c logs.sha256
```

部分清单引用构建产物（`.kernel-build/`、`<build-dir>/` 下的镜像）或按清单所在目录
的相对路径书写，因此只在原运行环境中可完整校验；这与文档重组无关，是证据本身的既有边界。

## 溯源

本目录由 2026-10-20 的文档重组建立：原先散落在 `docs/` 根目录、`releases/`、`distro/`、
`measured/` 与各子系统目录下的状态与证据文件集中至此。被取代的一次性日志已删除并随
[历史重写](../history/rewrite-provenance/) 一并从历史中移除；`docs/history/` 下的既有溯源记录
保持字节不变。

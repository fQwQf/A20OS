# 用户态集群服务与接口

A20OS 用户态集群由 Native 守护进程（`clusterd`、`cluster-agent`、`jobd`）组成，与 MCU 叶子节点保持一致的协议规范与资源预算。完整主机以普通 Native 进程运行服务，MCU 在周期泵中调用静态 agent；两者均执行 CW/1 规范。构建命令：`make ARCH=riscv64 native-cluster-agent`（其他主机架构替换 `ARCH` 即可）。固件与运行构建说明参见 [10-operator-guide.md](10-operator-guide.md)。

| 组件 | 实际职责 | 详细契约 |
|---|---|---|
| clusterd | 从种子与内核快照重建成员；直接/间接探测；能力目录、租约、any 选择 | 控制面服务与动态路由传播 |
| cluster-agent | CW 查询、可注册算子、显式清单进程、期限处理 | 执行代理与算子目录 |
| jobd | 本地有界 DOT/词频作业提交、远端分片派发、失败重派、二叉树归约 | 分布式分片归约调度器 |
| cluster-supervisor | 持久配置、三服务独立重启、退避与退出回收 | [10-operator-guide.md](10-operator-guide.md) |
| cluster-wordcount | 从文件提交词频作业并打印排序结果 | [cluster-apps.md](cluster-apps.md) |
| Native SDK | 固定目录解析、CM/1 any 选择、绑定启动会话的连接和作业驱动 | `user/liba20rt/a20_cluster*.h` |
| MCU leaf | 相同 CW 协议、静态算子表与能力查询、单帧可靠应答缓存 | [04-transports.md](04-transports.md) |

## 消息编码

CL 载荷可承载应用自己的协议；现有 CW、CD、CM、CJ 是有固定字段和版本的显式小端协议，不发送 C 结构体内存映像。原有 TLV 金样仍为兼容数据，不能把它描述成目前 clusterd/jobd 实际使用的格式。Native 本地句柄传递只用于本机连接，不会序列化到远端。

## 恢复和限制

clusterd 重启从现存内核状态恢复，已有业务连接不因控制面替换而断开。 agent 重启产生新服务槽位，旧连接失败，启动 epoch 在同内核内保持不变。 jobd 根进程退出会终止作业；没有作业日志或自动恢复。可重试算子只保证至少一次执行与结果去重；进程任务禁止不确定失败后的自动重派。

有界成员传播与租约路由已在 `clusterd` 接入，持久配置与 Native 监管已由 `cluster-supervisor` 统一管理（参见 [10-operator-guide.md](10-operator-guide.md)）。通用字节结果、自定义封装归约、CONCAT 和词频已经使用相同调度器执行（参见 [cluster-apps.md](cluster-apps.md)）。以下仍是设计目标：通用 TLV 作业提交、多作业持久队列、外部数据流式处理、更大规模与物理混合部署。当前实现已将目标与运行证据分开，继续按统一运行时架构规范推进。

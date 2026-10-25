# CLUSTER 缺口

对应 V1-CLUSTER。现状见 [current/cluster-implementation-status.md](../current/cluster-implementation-status.md)。

当前没有开放的 1.0 缺口。CLUSTER-01..04 的关闭依据见状态页的
“CLUSTER 1.0 关闭记录（2026-10-22）”：真实三内核故障矩阵、四节点
速率契约与有界 soak、物理边界的明确披露，以及
[62-release-scope.md](../../cluster/62-release-scope.md) 的范围决定。

`check-mcu-program-rtl` 的构建阻塞属于全局构建规则，记录在
[DELIVERY-06](delivery.md)，不在本领域台账内。

新增的多独立机器单一系统映像目标单独记录在
[cluster-ssi.md](cluster-ssi.md)。它扩展既有 1.0 范围，不把远程页快照服务
当作透明共享内存或普通程序跨机线程执行已经完成。

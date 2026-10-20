# CLUSTER 缺口

对应 V1-CLUSTER。现状见 [current/cluster-implementation-status.md](../current/cluster-implementation-status.md)。

### CLUSTER-01 服务/对象/应用生命周期

- 现状：服务启动/停止/回收、nonce 去重、代次与旧 handle 失效有实现。
- 缺口：服务死亡后的恢复、`UNKNOWN outcome` 查询不得二次执行、网络重连/成员故障没有完整故障矩阵。
- 关闭条件：每种故障路径都有 guest 用例，退出码与最终资源回收一致。
- 来源：[current/cluster-implementation-status.md](../current/cluster-implementation-status.md)。

### CLUSTER-02 四节点负载目标

- 现状：60 秒 SEND 业务核对通过，但吞吐约 165–240 条/秒，远未达 >10000 条/秒目标。
- 缺口：目标未达成；8 小时混合 SEND/CALL soak 与物理板未验证。
- 关闭条件：负载达到既定目标，或把目标改成有依据的契约；soak 与物理板给出结论。
- 来源：[current/cluster-implementation-status.md](../current/cluster-implementation-status.md)。

### CLUSTER-03 物理边界

- 现状：验收在 QEMU 上完成，接口与布局检查过。
- 缺口：真板时序、电气行为、总线一致性与 MCU 资源高点无法由 QEMU 确立。
- 关闭条件：在物理板上完成对应矩阵；或随候选明确披露为未验证边界。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-CLUSTER 行。

### CLUSTER-04 进程自动迁移与跨重启 exactly-once

- 现状：CA1/CA2 范围明确；没有跨重启 exactly-once 或自动迁移。
- 缺口：这些是研究扩展，不属于当前 release scope；需要决定是否纳入 1.0。
- 关闭条件：要么实现并有故障矩阵，要么在范围文档里明确排除。
- 来源：[current/cluster-implementation-status.md](../current/cluster-implementation-status.md)。

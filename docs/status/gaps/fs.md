# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-07 xattr/ACL 持久化与重启

- 现状：后端 xattr/ACL 持久化没有显式重启测试。
- 缺口：属性在 fsync/重启后的持久性未被证明。
- 关闭条件：写入 xattr/ACL → fsync → 重启 → 读回一致；失败路径不产生半写入。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-FS 行。

### FS-08 UXFS/close-token 的剩余边界

- 现状：close-token 让丢失的 CLOSE 可重放；OPEN 不幂等；token 不跨服务/挂载通道重启。
- 缺口：CONFIRM-ACK 丢失与额外 RPC 性能代价未覆盖；UXFS 崩溃一致性与更广后端生命周期仍开放。
- 关闭条件：CONFIRM-ACK 丢失场景通过；重启后 token 语义有明确契约；崩溃一致性用例通过。
- 来源：[fs/ufs-close-token.md](../../fs/ufs-close-token.md)、[current/v1.0-progress.md](../current/v1.0-progress.md)。

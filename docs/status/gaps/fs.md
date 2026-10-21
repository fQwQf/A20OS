# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-03 文件数据块完整性校验

- 现状：元数据与 JBD2 有校验和；文件内容损坏不可检测。
- 缺口：没有兼容磁盘格式的 data-block 校验和。
- 关闭条件：定义兼容的校验和元数据，读取时能检测损坏块，重挂载与日志重放后行为一致。
- 来源：requirements N16。

### FS-05 NTFS 崩溃与重挂载

- 现状：NTFS 的 crash/remount 在 C 与 Rust 两种实现上都失败。
- 缺口：崩溃后的 NTFS 卷无法正确恢复或拒绝。
- 关闭条件：崩溃注入后重挂载要么恢复一致，要么以明确错误拒绝；两种实现行为一致。
- 来源：[current/fs-status.md](../current/fs-status.md)、[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

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

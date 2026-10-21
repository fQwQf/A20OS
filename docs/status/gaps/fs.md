# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-08 UXFS/close-token 的剩余边界

- 进展（2026-10-21）：服务端已可注入「丢失 CONFIRM 应答」——`user/svc/ufsd.rs` 里与
  `UFSD_CLOSE_TOKEN_DROP_ACK_ONCE` 同一个测试 cfg 现在也会丢弃第一条被接受的 CONFIRM 的应答；
  `user/cmds/core/ufs_confirm_ack_test.c` 跑客户端一侧（写文件 → close → 读回 → 再开关一次）。
  实测：把带该故障的 Rust ufsd 换进 `both` 镜像后，客户端用例通过
  （`CONFIRM_ACK: PASS close survived lost CLOSE and CONFIRM replies`），但**服务端的 drop 标记
  一次都没出现**——因为 `protocol::close_confirm_requested()` 要求 `backend == BACKEND_VNFS`，
  而 `/bin/ufsd-rv <mp> <idx> fat` 走的是 FAT 后端，close-token/confirm 能力根本没协商上。
  也就是说这次只证明了「普通 close 能用」，没有真正走到丢失 ACK 的路径。换成 ext4(vnfs) 后端的
  一次尝试没有挂载成功（`ENOENT`），我在这里停止，没有把这条做成门禁。

- 现状：close-token 让丢失的 CLOSE 可重放；OPEN 不幂等；token 不跨服务/挂载通道重启。
- 缺口：CONFIRM-ACK 丢失与额外 RPC 性能代价未覆盖；UXFS 崩溃一致性与更广后端生命周期仍开放。
- 关闭条件：CONFIRM-ACK 丢失场景通过；重启后 token 语义有明确契约；崩溃一致性用例通过。
- 来源：[fs/ufs-close-token.md](../../fs/ufs-close-token.md)、[current/v1.0-progress.md](../current/v1.0-progress.md)。

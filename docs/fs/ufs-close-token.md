# UFSD close-token 协议

close-token 扩展让一个丢失的 CLOSE 响应可以重放，并由服务端保留已接受的结果。它不使 OPEN 幂等，也不跨 UFSD 或挂载通道重启保留 token。

## 线协议行为

协议版本仍为 2。INIT 期间客户端声明 `UFS_CLOSE_CONFIRM_CAPABILITY`；只有服务端保留已接受结果时才会回显该位。仅支持 token 的旧对端保持既有 OPEN/CLOSE 行为，不启用重试。

确认模式下，OPEN 在 `out2` 返回一个唯一的非零 token；若在取得后端 open 引用之前失败，则返回 `-EAGAIN`。CLOSE 在 `arg0` 携带该 token。CLOSE 响应成功后，客户端用同一 token 发送 `UFS_OP_CLOSE_CONFIRM`（19）。若 CLOSE 结果不明确，用该 token 重试 CLOSE；若确认不明确，只重试 CONFIRM。

token 序号在同一挂载代次内单调递增且不复用。已接受的 CLOSE 结果占用一张有界的 64 项墓碑表，直到被确认；该表不淘汰仍可重试的条目。表满时新的 close 会在消耗后端之前失败。重放的 CLOSE 成功返回，且不重复执行后端 close。未知的 CONFIRM 幂等成功；对一个仍然存活但未被接受的 token 做确认返回 `-EAGAIN`；已知 token 对应另一个 inode 返回 `-EINVAL`。

Rust 分派器拥有自己的生命周期表，C 实现使用对应的注册表。客户端在 OPEN 之前分配自己的 file context 与 vfile，在接受 close 之后继续保留，只有确认之后才释放。孤儿清理保持原有顺序；清理失败会让已接受的 token 对既有的 SYNC 与挂载恢复路径保持可重放。

## 使用与验证

针对性的 host 契约测试：

```sh
tools/host-tests/test-ufs-close-token-host.sh
```

可用 `NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE=1`（`test-drop-close-token-ack`）启用「丢失 CLOSE-ACK」故障档的 guest。guest 必须观察到响应被丢弃、用同一 token 重试、收到重放成功、确认、重启，并核对数据可读回。该档位测的是重试契约，不是生产档服务。

客户端在结果不明确时（请求发出但没收到应答）**自动重试一次**：CLOSE 用同一 token 重放，CONFIRM 重发同一条确认（未知确认幂等成功），所以丢失的 ACK 不会暴露给调用者，`close(2)` 仍然成功。`make smoke-ufs-confirm-ack` 用一个会丢弃首条 CLOSE 与 CONFIRM 应答的测试用 ufsd 把这两条都跑一遍（vnfs/ext4 挂载——close-token 能力只在该后端协商）。

当前限制：OPEN 的请求/响应丢失不幂等；token 不能跨服务或挂载通道重启存续（重启后旧 token 一律视为未知，CONFIRM 幂等成功、CLOSE 返回 `ENOENT`）；额外那次 RPC 的性能代价没有测量。默认 `VFS_LIFECYCLE_IMPL=c` 不变。另有独立的 UXFS vnode 身份修复，保证同一 inode 的重复查找共享规范的大小状态；该修复与 close-token 重放无关。

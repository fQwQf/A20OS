# UFSD close-token protocol

The close-token extension makes a lost CLOSE response replayable while the server retains the accepted result. It does not make OPEN idempotent and does not preserve tokens across a UFSD or mount-channel restart.

## Wire behavior

Protocol version remains 2. During INIT, a client advertises `UFS_CLOSE_CONFIRM_CAPABILITY`; a server echoes it only when it retains accepted outcomes. Older token-only peers keep the existing OPEN/CLOSE behavior and do not enable retries.

In confirm mode, OPEN returns a unique nonzero token in `out2`, or fails with `-EAGAIN` before acquiring a backend open reference. CLOSE carries the token in `arg0`. After a successful CLOSE response, the client sends `UFS_OP_CLOSE_CONFIRM` (19) with the same token. If CLOSE is ambiguous, retry CLOSE with that token; if confirmation is ambiguous, retry only CONFIRM.

The token serial is monotonic within a mount generation and is not reused. Accepted CLOSE results occupy a bounded 64-entry tombstone table until confirmation; the table does not evict retryable entries. When full, a fresh close fails before backend consumption. Replayed CLOSE succeeds without repeating the backend close. Unknown CONFIRM is idempotent success; confirming a live unaccepted token returns `-EAGAIN`, and a known token for another inode returns `-EINVAL`.

The Rust dispatcher owns its lifetime table; the C implementation uses its corresponding registry. The client allocates its file context and vfile before OPEN, retains them after an accepted close, and releases them only after confirmation. Orphan cleanup retains its existing order; a cleanup failure leaves the accepted token replayable for the existing SYNC and mount-recovery paths.

## Use and validation

The focused host contract test is:

```sh
tools/test-ufs-close-token-host.sh
```

A lost-CLOSE-ACK fault-profile guest can be enabled with `NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE=1` (`test-drop-close-token-ack`). The guest must observe the dropped response, retry the same token, receive replay success, confirm it, restart, and verify payload readback. This profile tests the retry contract, not the production-profile service.

The historical r9 run reported the complete retry/confirm sequence, a 33-byte cold read after restart, clean `e2fsck`, and natural poweroff. Its recorded test-profile kernel hash was `efdbc8912362c492a259dd3902e2a900d953ed9ce8b42b96f6e71e9289ef7c75`; the test-profile service hash was `a2906a330a8efbe1ff541ed4986949ddbcb5211cde17a7b9a9add79acee5f68c`. The original runner and raw log were outside the repository and are not retained here, so this historical result is not a fresh source-bound acceptance run.

Current limits: OPEN request/response loss is not idempotent; tokens do not survive service or mount-channel restart; CONFIRM-ACK loss and the extra RPC's performance cost are not covered by the recorded guest run. The default `VFS_LIFECYCLE_IMPL=c` remains unchanged. A separate UXFS vnode-identity fix ensures repeated lookups of one inode share canonical size state; that fix is independent of close-token replay.

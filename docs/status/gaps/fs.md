# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-01 ext4 无日志时的孤儿 inode（N21）

- 现状：`ext4_inode_remove()` 在存在缓存 vnode 时只标 unlinked，不减磁盘链接计数、不记 orphan 链；缺陷在 unlink 当下就存在。
- 缺口：POSIX unlink-while-open 语义保留了，但崩溃/重启后无法回收或恢复该 inode。
- 关闭条件：open-unlink 后崩溃/重启，inode 要么被回收、要么被恢复到 `lost+found`，且 `e2fsck -fn` 干净；不许用 `e2fsck -fy` 修镜像算通过。
- 诊断：[notes/fs-ext4-orphan.md](notes/fs-ext4-orphan.md)。
- 来源：[current/ext4-orphan-gap.md](../current/ext4-orphan-gap.md)、requirements N21。

### FS-02 JBD2 特性语义

- 现状：运行期元数据走 JBD2 ordered commit；校验和记在 descriptor tag 内。
- 缺口：barrier/async-commit 广告、ordered 与 journal-data 两种模式、fast commit、revoke 记录的延迟回滚用途均未完整实现。
- 关闭条件：已广告的每种模式都真实实现并有崩溃注入用例；不支持的明确标注为未完成契约，不能只改文档删需求。
- 来源：requirements N14。

### FS-03 文件数据块完整性校验

- 现状：元数据与 JBD2 有校验和；文件内容损坏不可检测。
- 缺口：没有兼容磁盘格式的 data-block 校验和。
- 关闭条件：定义兼容的校验和元数据，读取时能检测损坏块，重挂载与日志重放后行为一致。
- 来源：requirements N16。

### FS-04 JBD2 校验和 v1/v2 降级策略

- 现状：v1/v2 日志失败关闭（拒绝挂载），没有显式降级策略。
- 缺口：旧格式要么支持只读降级，要么明确列为不支持；当前既未支持也未形成契约。
- 关闭条件：干净 v1/v2 镜像可只读挂载；脏或校验和损坏的镜像被拒绝；无遗留重放/写支持的声明准确。
- 来源：requirements N15。

### FS-05 NTFS 崩溃与重挂载

- 现状：NTFS 的 crash/remount 在 C 与 Rust 两种实现上都失败。
- 缺口：崩溃后的 NTFS 卷无法正确恢复或拒绝。
- 关闭条件：崩溃注入后重挂载要么恢复一致，要么以明确错误拒绝；两种实现行为一致。
- 来源：[current/fs-status.md](../current/fs-status.md)、[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### FS-06 ISOFS 压力覆盖

- 现状：ISOFS 压力覆盖不完整；曾有 `errno 19` 被 skip 却记 PASS 的问题。
- 缺口：ISOFS 在大目录/长名/边界输入下的行为没有完整门禁。
- 关闭条件：ISOFS 压力用例实际执行（不 skip）并通过，失败有明确原因。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

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

### FS-09 guest 写盘后 ext4 位图校验和不一致

- 现状：`image-world` 产物 `e2fsck -fn` 干净；guest 写入一次后，从宿主机看（qcow2 overlay 或 kill 后的镜像）出现 `Block bitmap checksum does not match bitmap`，`debugfs` 打不开。
- 缺口：怀疑写入路径没有同步 `metadata_csum` 的位图校验和（无日志时没有重放兜底）。
- 关闭条件：guest 写盘后 `e2fsck -fn` 干净；损坏来源有明确解释。
- 诊断：[notes/fs-ext4-bitmap.md](notes/fs-ext4-bitmap.md)。

### FS-10 新挂载 API 没有 detached mount，util-linux 的 mount 因此不可用

- 现状：`mount(2)` 与 `busybox mount` 可用——在 guest 内直接调 `mount("tmpfs","/tmp","tmpfs",0,NULL)` 返回 0。util-linux 的 `mount` 走 libmount 的新 API（`fsopen`→`fsconfig`→`fsmount`→`move_mount`），而 `sys_fsmount` 以 `target=NULL` 调 `fscontext_fsmount()`，后者在 `!target` 时直接 `-EINVAL`，于是 util-linux 对任何文件系统都失败（"wrong fs type, bad option, bad superblock …"，退出码 32，见 `kernel/fs/fscontext.c`、`kernel/abi/linux/sys_mount_api.c`）。
- 缺口：没有 detached mount。Linux 的 `fsmount(2)` 返回一个未挂接的挂载 fd，由 `move_mount(2)` 带 `MOVE_MOUNT_F_EMPTY_PATH` 挂到目标；A20OS 的各个后端（ram/virtual/cgroup/block）都在挂载时定下 path 并立刻 `vfs_mount_link_tree()`，没有"先建、后接"的路径，`sys_move_mount` 也只能在两个路径之间搬。
- 关闭条件：`fsopen`+`fsmount` 返回 detached mount fd；`move_mount(mfd, "", AT_FDCWD, dir, MOVE_MOUNT_F_EMPTY_PATH)` 成功挂接；guest 内 util-linux 的 `mount -t tmpfs tmpfs <dir>` 与 `umount <dir>` 可用；`smoke-vfs-*` 门禁保持通过。
- 来源：`kernel/fs/fscontext.c`、`kernel/fs/vfs/mount_ops.c`、`kernel/abi/linux/sys_mount_api.c`。

### FS-11 `smoke-vfs-stress` 的 ISOFS 读用例失败

- 现状：`make smoke-vfs-stress` 在当前提交上以 `VFS_STRESS: FAIL isofs-read-hello errno=0` 与 `VFS_STRESS: FAIL isofs-read-nested errno=0` 结束（rc=2）；同一日志里 ISOFS 已 `Mounted: block_size=2048 vol_blocks=22 root_extent=18 root_size=146`。把对 `kernel/fs/vfs/{dcache.c,mount_state.rs}` 的并发修复 stash 掉后结果不变，所以与该修复无关。
- 缺口：ISOFS 挂载成功但按名读取失败，errno 为 0（即失败不来自被检查的 errno 路径）；`smoke-vfs-stress` 因此整体为红。
- 关闭条件：两条 isofs 读用例在候选上通过，或失败有明确原因并给出对应门禁。
- 来源：并发 VFS 负载回归时首次跑到该目标而暴露。

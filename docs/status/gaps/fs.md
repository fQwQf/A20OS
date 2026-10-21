# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-01 ext4 无日志时的孤儿 inode（N21）

- 现状（2026-10-21 实测更正）：unlink 当下**是**正确的——`ext4_inode_remove()` 会先把零链接 inode
  挂上孤儿链（`ext4_orphan_add_last_link()`，`s_last_orphan` + `i_dtime` 作 next），日志里能看到
  `[EXT4] unlink orphan ino=... refs=... vfile_opens=...`；崩溃后 `e2fsck -fn` 也**干净**。
  真正的阻塞在**下一次挂载**：`ext4_mount_flags()` 对无日志镜像执行
  `!ext4_state_admit(sb.s_state)` 就整体拒绝挂载（`refusing mount: no-journal filesystem state
  is not clean (0x0)`），于是 recovery 根本没有机会跑；实测「open+unlink 后 kill QEMU，再启同一张
  镜像」就是这个结果，`/extra` 没挂上，孤儿 inode 既未回收也未进 `lost+found`。
  另一条路径（world 镜像，状态位为 VALID 时）走到 recovery 并报
  `refusing mount: orphan recovery failed (-95)`，说明恢复路径本身也需要能处理该 inode 形状。
- 缺口：POSIX unlink-while-open 语义保留了，但崩溃/重启后无法回收或恢复该 inode。
- 关闭条件：open-unlink 后崩溃/重启，inode 要么被回收、要么被恢复到 `lost+found`，且 `e2fsck -fn` 干净；不许用 `e2fsck -fy` 修镜像算通过。
- 未决点（需要决定，不适合单方面改）：无日志镜像是否允许在「状态位非 VALID」时挂载并跑 recovery。
  现在的拒绝是刻意 fail-closed（`ext4.c` 注释：崩溃或元数据失败后让下一次 RW 挂载失败关闭，
  而不是信任易失的隔离状态）；放行它等于接受「崩溃后可能带着未校验的元数据继续写」，是可用性与
  安全性的取舍。
- 另一处已确认缺陷（同一域）：`ext4_release_vn()` 对「inode 号已被复用」的滞留 vnode 仍按 inode 号
  触发一次 reap，该 inode 早已不在链上，于是打出误导性的
  `deferred orphan cleanup failed ... orphan retained for recovery`（实测 err=-5）；缓存 vnode 存活
  期间 inode 号复用本身也可能让滞留 vnode 误回收别的文件。
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

- 现状（2026-10-21 实测）：降级策略**已经存在**，这条的 现状 描述过时了。只读挂载尝试会走显式降级：
  `ext4_journal_pending()` 在 `jbd2_load_superblock()` 返回 `-EOPNOTSUPP` 且 `read_only` 时调用
  `a20_jbd2_allow_clean_legacy_readonly()`（`user/svc/jbd2_legacy_policy.rs`，由
  `tools/test-jbd2-legacy-policy-host.sh` 的 4 个用例门禁），干净旧日志得到
  `EXT4_JOURNAL_READONLY_LEGACY` 并打印 `clean legacy checksum journal; explicit read-only downgrade`。
  可写挂载不走降级：实测把 `ext4-journal.img` 的日志校验和三字段清掉（即 mke2fs 原始形态）后，
  引导时的可写挂载是 `[EXT4/JBD2] recovery failed: -95` → `Refusing mount after journal recovery
  failure`，即写路径对旧格式保持失败关闭。
- 缺口：旧格式要么支持只读降级，要么明确列为不支持；当前既未支持也未形成契约。
- 关闭条件：干净 v1/v2 镜像可只读挂载；脏或校验和损坏的镜像被拒绝；无遗留重放/写支持的声明准确。
- 未完成的验证：只读降级这一半我只有代码与宿主用例，还没有端到端证据——树里没有任何门禁会生成
  v1/v2 镜像（`img.py ext4-journal` 一律做 csum_v3 手术），guest 的 `/extra` 又总是可写挂载，
  所以要走 RO 路径得先让门禁能产出旧格式镜像并在 guest 内只读挂载它。
- 来源：requirements N15。

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

# 文件系统当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

各文件系统后端的当前支持范围与限制。

<!-- 以下内容从 docs/fs/ntfs.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 路径访问与 Landlock

普通 open、openat2 与 proc fd 重开在创建、截断、后端 open 前检查解析后的目标路径。普通 open 与 openat2 使用同一次遍历返回的 vnode 与 canonical path，避免检查原始符号链接名后访问另一个目标。READ_FILE/WRITE_FILE 子集及继承语义见[安全候选验收](../../security/release-acceptance.md)；该路径策略不提供 inode 锚定的 Linux 完整 Landlock 语义。`smoke-security-release` 覆盖符号链接、相对别名、openat2、 proc fd 重开与创建拒绝，并验证没有截断/创建副作用。

## ISOFS

只读；名字按 ISO 9660 规则转小写并去掉 `;1` 版本后缀。没有 Rock Ridge / Joliet。

测试镜像由 `tools/fs/mkisofs_test.c` 生成，刻意覆盖这几类形状：跨多个 2048 字节逻辑块的根目录（记录不跨块，块内剩余部分补零，扫描需要续到下一块）、200 字节长名（贴近驱动`ISOFS_MAX_NAME` 上限）、零长文件、恰好一个块的文件、跨块文件，以及嵌套目录。`smoke-vfs-stress` 在 guest 内读回全部这些文件（含首块与末块目录项），并检查 EOF 之后读返回 0、缺失名字返回 `ENOENT`、写打开失败。同一张镜像也供用户态 ufsd 的`smoke-native-fs-all` 使用。

文件读取走页缓存；`readpage` 的返回值是页内有效字节数，页缓存会把该值之后的部分清零。

## ext4

有日志与无日志（`^has_journal`）两种镜像都支持完整的 create/write/unlink/rmdir 回收：

- unlink 把零链接 inode 挂到孤儿链（`s_last_orphan`，`i_dtime` 作 next），崩溃后由挂载时的 recovery 处理；延迟回收（最后一个 vnode 引用消失时）会先核对 inode 上那条链是否仍是本 vnode 写下的那一条，所以 inode 号被复用后不会再对别的文件误 reap、也不会因此 poison 日志；
- rmdir 的目录没有孤儿链条目（它自己的两个链接随它一起消失），所以待释放状态记在 vnode 上，最后一个引用消失时截断并释放 inode —— 缓存的 vnode 不再会让目录 inode 永远留在盘上；
- `metadata_csum`（superblock/group descriptor/bitmap 的校验和）在写入路径同步，`e2fsck` 不报校验和错误。
- xattr：`user.*` 与两个 POSIX ACL 属性名持久化在 inode 尾部（合计约 90 字节/inode，超出返回`ENOSPC` 且不改动已有属性；没有外部 EA 块）。`trusted.*`/`security.*` 仍只在易失表里。ACL 只存储与读回，不做权限判定。门禁 `smoke-ext4-xattr`：写入 → fsync → 关机 → 宿主 `debugfs ea_list`看到属性且 `e2fsck -fn` 干净 → 重启后读回一致，并验证替换与 `ENOSPC` 路径。

正常关机后宿主 `e2fsck -fn` 干净，并且这条性质有门禁：`smoke-vfs-stress` 在 guest 跑完（含 create/unlink/rmdir）之后会对同一张镜像跑一次 `e2fsck -fn`。

无日志镜像被**直接 kill**（不是正常关机）后，下一次挂载会认出 `s_state` 的脏标记并跑孤儿 recovery 再挂载，不拒绝：孤儿 inode 在 recovery 里被截断并释放，恢复后 `e2fsck -fn` 干净。`smoke-ext4-recovery` 就是这个场景的门禁（open+unlink → kill → 重启同一张镜像 → 检查挂载成功、 inode 已被回收、两次 `e2fsck -fn` 都干净）。仍然拒绝的是 `ERROR_FS`：那表示驱动自己在上一会话里发现过不可信元数据，挂载期 recovery 修不了它。

需要日志级崩溃原子性（提交点前后哪些写在崩溃后存活）时用带日志的 `ext4-journal.img`，由`smoke-ext4-journal` 覆盖（4 个注入点的崩溃 + 恢复后 `e2fsck` 干净）。

日志模式只有 **ordered 元数据日志**一种（revoke 记录已实现并在重放时生效）。改变日志语义的挂载选项——`data=journal`、`data=writeback`、`journal_async_commit`、`journal_checksum`、`barrier=0`、`nobarrier`、`fast_commit`、`journal_dev`——会被 `ext4_validate_mount_options()` 显式拒绝（`-EOPNOTSUPP`），不会静默降级成 ordered；日志 superblock 侧也只接受 `REVOKE|64BIT|CSUM_V3`。

带日志镜像的日志必须是 v3 校验和格式（mke2fs 默认）。旧式 v1/v2 日志不支持重放或写入：干净的 v1/v2 镜像可以**只读**挂载（显式降级），日志非空的旧式镜像连只读都拒绝，可写挂载一律拒绝。`smoke-ext4-legacy-journal` 覆盖这三种情形，并在宿主机上对干净镜像跑 `e2fsck -fn`。

## NTFS 限制

- 不支持 `$ATTRIBUTE_LIST` 跨记录属性（大文件/碎片文件可能拒绝读取）。
- 创建的文件分配在 MFT 空闲记录（首适应，从记录 12 起扫），不维护 `$MFT` 位图。
- 时间戳未按 FILETIME 格式转换（写 0，读时忽略）。
- 不支持 hard link、symbolic link、`$LogFile` replay 或 `$MFTMirr` 恢复；崩溃一致性不等同 Windows NTFS。
- 服务崩溃后的重挂载是可用的：`UXFS_NTFS` 用 SIGKILL 杀掉 ufsd 实例、卸载、重启同一卷，断言内容（8 KB 非驻留文件、嵌套目录、重启标记）仍在。让这一步成立的是 UXFS 卸载路径：服务被 kill 后它再也不会碰通道，`peer_closed` 因此一直是 0，卸载会等满 5 s 超时并返回 `ETIMEDOUT`；现在卸载在超时后用服务任务是否存活再判一次，确认已退出才撤销注册。
- 这里保证的是**服务级**崩溃（进程死掉）后的重挂载；机器掉电不会重放 `$LogFile`，那种情况下卷可能需要 Windows 的 chkdsk，本驱动不声称能修复。
- VFS xattr 仅使用易失的全局 RAM 表，没有 NTFS attribute 持久化后端。

# 文件系统当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

各文件系统后端的当前支持范围与限制。

<!-- 以下内容从 docs/fs/ntfs.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## ISOFS

只读；名字按 ISO 9660 规则转小写并去掉 `;1` 版本后缀。没有 Rock Ridge / Joliet。

测试镜像由 `tools/mkisofs_test.c` 生成，刻意覆盖这几类形状：跨多个 2048 字节逻辑块的根目录
（记录不跨块，块内剩余部分补零，扫描需要续到下一块）、200 字节长名（贴近驱动
`ISOFS_MAX_NAME` 上限）、零长文件、恰好一个块的文件、跨块文件，以及嵌套目录。
`smoke-vfs-stress` 在 guest 内读回全部这些文件（含首块与末块目录项），并检查 EOF 之后
读返回 0、缺失名字返回 `ENOENT`、写打开失败。同一张镜像也供用户态 ufsd 的
`smoke-native-fs-all` 使用。

文件读取走页缓存；`readpage` 的返回值是页内有效字节数，页缓存会把该值之后的部分清零。

## ext4

有日志与无日志（`^has_journal`）两种镜像都支持完整的 create/write/unlink/rmdir 回收：

- unlink 把零链接 inode 挂到孤儿链（`s_last_orphan`，`i_dtime` 作 next），崩溃后由挂载时的
  recovery 处理；延迟回收（最后一个 vnode 引用消失时）会先核对 inode 上那条链是否仍是本 vnode
  写下的那一条，所以 inode 号被复用后不会再对别的文件误 reap、也不会因此 poison 日志；
- rmdir 的目录没有孤儿链条目（它自己的两个链接随它一起消失），所以待释放状态记在 vnode 上，
  最后一个引用消失时截断并释放 inode —— 缓存的 vnode 不再会让目录 inode 永远留在盘上；
- `metadata_csum`（superblock/group descriptor/bitmap 的校验和）在写入路径同步，`e2fsck` 不报
  校验和错误。

正常关机后宿主 `e2fsck -fn` 干净，并且这条性质有门禁：`smoke-vfs-stress` 在 guest 跑完
（含 create/unlink/rmdir）之后会对同一张镜像跑一次 `e2fsck -fn`。

无日志镜像被**直接 kill**（不是正常关机）后，下一次挂载会认出 `s_state` 的脏标记并跑孤儿
recovery 再挂载，不拒绝：孤儿 inode 在 recovery 里被截断并释放，恢复后 `e2fsck -fn` 干净。
`smoke-ext4-recovery` 就是这个场景的门禁（open+unlink → kill → 重启同一张镜像 → 检查挂载成功、
inode 已被回收、两次 `e2fsck -fn` 都干净）。仍然拒绝的是 `ERROR_FS`：那表示驱动自己在上一会话里
发现过不可信元数据，挂载期 recovery 修不了它。

需要日志级崩溃原子性（提交点前后哪些写在崩溃后存活）时用带日志的 `ext4-journal.img`，由
`smoke-ext4-journal` 覆盖（4 个注入点的崩溃 + 恢复后 `e2fsck` 干净）。

## NTFS 限制

- 不支持 `$ATTRIBUTE_LIST` 跨记录属性（大文件/碎片文件可能拒绝读取）。
- 创建的文件分配在 MFT 空闲记录（首适应，从记录 12 起扫），不维护 `$MFT` 位图。
- 时间戳未按 FILETIME 格式转换（写 0，读时忽略）。
- 不支持 hard link、symbolic link、`$LogFile` replay 或 `$MFTMirr` 恢复；崩溃一致性不等同 Windows NTFS。
- VFS xattr 仅使用易失的全局 RAM 表，没有 NTFS attribute 持久化后端。

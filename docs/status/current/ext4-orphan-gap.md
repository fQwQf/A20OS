# Ext4 已 unlink inode 的完整性缺口

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

四文件系统 guest 的服务级断言曾通过，但运行后的 `e2fsck -fn` 在 C 与 Rust 两个 UFSD 档位上都不通过。
这是真实的 ext4 完整性缺陷，不是服务语言差异。

在同一份干净 ext4 种子上做阶段隔离复现，可以定位第一个失败操作：guest 创建并读回 `EXT4BIN.BIN`
（8 KiB），重命名为 `RENAMED.BIN`，然后 unlink。`created` 与 `renamed` 两个阶段的镜像都能过 `e2fsck -fn`；
`unlinked` 阶段的镜像报 `Unattached inode 13`。C 与 Rust UFSD 运行产生的 unlinked ext4 镜像逐字节相同。
每个 guest 都以 0 退出并打印了三个阶段标记。缺陷在 unlink 当下就存在，早于服务死亡、重挂载或后端拆除。

该孤儿 inode 的 mode 为 `0100644`、链接计数 1、大小 8192、块计数 16 扇区、extent 标志 `0x80000`，
数据块 2201–2208。根目录里只有 `hello.txt` 与 `lost+found`；inode 13 没有任何目录项。它的
generation/version 与时间戳都是零，删除时间也是零。`e2fsck` 判定它未被挂接，并提议把它连接到
`lost+found`。干净种子里没有 inode 13。

源码路径是 `kernel/fs/diskfs/ext4_namei.c`：`ext4_inode_remove()` 移除目录项，并在存在缓存 vnode 时把它
标为 unlinked 后直接返回，既不减磁盘上的 `i_links_count`，也不把该 inode 记入 ext4 的 orphan 链。真正的
截断与释放被推迟到 `ext4_release_vn()`。vnode 引用可以比 unlink 活得更久，因此这种延迟清理不能替代
orphan 跟踪。下一版实现必须保留 POSIX 的 unlink-while-open 语义，并让崩溃/重启后能回收或恢复该 inode；
不要用 `e2fsck -fy` 修好测试镜像然后算作通过。

这是已确认、仍然开放的 v1.0 文件系统完整性要求。本记录证明缺陷及其触发方式，不宣称 orphan 恢复或清理
已经实现。

当前挂载守卫处理的是遗留的 `s_last_orphan`/`i_dtime` 链。现代 ext4 的 `orphan_file` 不是这个格式：
`EXT4_FEATURE_COMPAT_ORPHAN_FILE` 是 compat 位 `0x1000`，`EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT` 是
ro-compat 位 `0x10000`。后者绝不能被当作 incompat 位 `0x10000`（那是加密）。干净文件系统可以在清除
`ORPHAN_PRESENT` 的同时保留兼容的 `ORPHAN_FILE` 特性，该状态仍可挂载。在现代 orphan-file 重放实现之前，
驱动只在其正确的 superblock 字段里拒绝 `ORPHAN_PRESENT`。这是安全边界，不是现代 orphan-file 支持。
目前还没有针对这些修正后常量的外部格式 fixture 运行过。

# A20OS 文件系统一致性模型

本文档记录 A20OS P1 中各后端的能力、一致性和 Linux ABI 行为矩阵。内容已按 `e33c3219` 的 `kernel/fs/` 与 VFS wrapper 做源码核对；“支持”表示当前代码路径存在，不等于每个后端、架构和崩溃场景都已有运行时测试。当前提交没有匹配的完整干净双架构平台复验。

## 1. 范围

覆盖的后端：

- `kernel/fs/diskfs/fat32.c`：FAT32 块设备文件系统
- `kernel/fs/diskfs/ext4.c`：ext4 块设备文件系统
- `kernel/fs/diskfs/ramfs.c`：内存文件系统（rootfs、`/dev/shm`、`tmpfs`）
- `kernel/fs/devfs/devfs.c`：设备特殊文件树（`/dev`）
- `kernel/fs/procfs/procfs.c`：进程 / 合成文件树（`/proc`）
- `kernel/fs/sysfs.c`：系统对象树（`/sys`）
- `kernel/fs/pipe.c`：匿名 pipe 实现
- `kernel/fs/anonfd.c`：匿名 fd 安装 helper

覆盖的 VFS 横切行为：

- permission 检查（`kernel/fs/vfs/stat_perm.c`、`kernel/fs/vfs.c`）
- path resolution（`kernel/fs/vfs/path_resolution.c`）
- mount-point 处理（`kernel/fs/vfs/mount_ops.c`、`kernel/fs/vfs/mount.c`）
- dcache invalidation（`kernel/fs/vfs/dcache.c`）
- xattr 存储（`kernel/fs/xattr.c`）

## 2. 图例

| 列 | 含义 |
|--------|---------|
| **Op** | VFS vnode 操作或高层 syscall 行为 |
| **Support** | 只描述当前代码路径，不编码测试结论；具体取值见下表 |
| **Errno** | 不支持或错误路径返回的 errno（来自代码，不是意图） |
| **Ordering / Atomicity** | 当前代码实际提供的保证 |
| **Linux ABI gap** | 测试必须编码的 Linux 差异 |

| Support 值 | 含义 |
|---|---|
| `Y` | 对应实现路径存在；不表示已在任一架构、后端组合或崩溃场景运行验证 |
| `partial` | 只实现操作的子集，或实现带有说明栏列出的功能/持久化限制 |
| `N` | 当前不支持；`Errno` 栏记录实际拒绝结果 |
| `-` | 对该后端或对象不适用 |

## 3. 各后端矩阵

### 3.1 FAT32（`kernel/fs/diskfs/fat32.c`）

FAT32 从 virtio-blk block cache 挂载。superblock 由 `fat32_sb_t.lock` 保护。per-inode metadata（mode、uid、gid）保存在按 cluster number 索引的全局 RAM 表 `g_fat32_meta` 中（`fat32.c`）。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `fat32_lookup`（`fat32_vn.c`）。支持 `.` 和 `..`。大小写不敏感的 8.3 + LFN。 |
| create | Y | — | `fat32_vn_create`（`fat32_vn.c`）。分配一个 cluster，写入 8.3 short entry。 |
| mkdir | Y | — | `fat32_vn_mkdir`（`fat32_vn.c`）。创建 `.`/`..` entry。 |
| unlink | Y | — | `fat32_vn_unlink`（`fat32_vn.c`）。释放 cluster chain。目录返回 `-EISDIR`。 |
| rmdir | Y | — | `fat32_vn_rmdir`（`fat32_vn.c`）。检查 `fat32_dir_is_empty`（统计 active entry > 2）。 |
| rename | Y | — | `fat32_vn_rename`（`fat32_vn.c`）。创建新目录项并删除旧项；移动目录时改写其 `..` entry。支持 `RENAME_NOREPLACE`；替换目标。 |
| link | N | `-ENOSYS` | `g_fat32_vnode_ops` 中没有 `.link` op。`vfs_link` 返回 `-ENOSYS`（`kernel/fs/vfs.c`）。 |
| symlink | N | `-ENOSYS` | 没有 `.symlink`/`.readlink` ops。`vfs_symlink` 返回 `-ENOSYS`（`kernel/fs/vfs.c`）。 |
| readlink | N | `-EINVAL` | symlink vnode 不可能存在；`vfs_readlinkat` 返回 `-EINVAL`（`kernel/fs/vfs.c`）。 |
| stat | Y | — | `fat32_stat`（`fat32_vn.c`）。`st_nlink` 硬编码为 1。`st_blocks` 按 512 向上取整。 |
| truncate | Y | — | `fat32_vn_truncate`（`fat32_vn.c`）。size 0 会重新分配单个 cluster。 |
| chmod | Y | — | `fat32_vn_chmod`（`fat32_vn.c`）。只存储在 RAM meta table 中。 |
| chown | Y | — | `fat32_vn_chown`（`fat32_vn.c`）。只存储在 RAM meta table 中；清除 suid/sgid bit。 |
| read/write/lseek | Y | — | `g_fat32_fops`（`fat32_file.c`）。每次 open 有一个带 cluster cache 的 `fat32_fctx_t`。 |
| readdir | Y | — | `fat32_freaddir`（`fat32_file.c`）。返回 `DT_DIR`/`DT_REG`；无 `DT_LNK`。 |
| ioctl | N | `-ENOTTY` | `.ioctl` 为 `NULL`；`vfs_ioctl` 落到 `-ENOTTY`。 |
| fsync | partial | — | `vfs_fsync` 同步共享脏映射、vnode page cache 和 mount block cache（`kernel/fs/vfs/file.c`），但 FAT32 没有显式 inode log。 |
| xattr | partial | — | 无 FAT32 后端 hook；reg/dir 的值由全局 `(mnt, ino)` RAM 表提供，unmount/reboot 后丢失。 |

**FAT32 顺序保证**

- 整个文件系统由 `sb->lock` 串行化（`fat32.c`）。
- 目录项更新和 FAT 更新之间不是原子的。unlink 先把目录项标记为删除，再立即释放 cluster chain，或在 vnode 仍被引用时把释放推迟到最后一个引用消失；如果在目录项删除后、chain 释放前崩溃，已不可达的 cluster 可能泄漏。
- 文件大小只在 close 时写回目录项（`fat32_fclose`，`fat32_file.c`）。close 前断电会丢失 size。
- block cache 是 write-back；`vfs_fsync` 和 unmount（`fat32_unmount`，`fat32.c`）会调用 `bcache_sync`。

**FAT32 ABI 缺口**

- 没有 hard link 或 symbolic link（rename 已实现）。
- 文件 ownership/mode 是易失的（只存在 RAM）。
- 没有 atime/mtime/ctime 持久化；没有对应 RAM time-meta 时，`vfs_vnode_stat` 回退为当前时间（`kernel/fs/vfs/stat_perm.c`）。
- `st_nlink` 总是 1。
- 没有 xattr 持久化。

### 3.2 ext4（`kernel/fs/diskfs/ext4.c`）

ext4 从 block cache 挂载，并使用强引用 vnode cache：同一 `(superblock, inode)` 在 cache 中只有一个 live vnode，cache 自身持有引用，lookup 命中时为调用者再取引用。unlink/rmdir/rename-remove 会先把 victim 移出 cache 并标记 unlinked，最后一个外部引用释放后才回收 inode 数据；cache prune 与 unmount 负责释放仍由 cache 独占的引用。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `ext4_lookup`（`ext4_namei.c`）。支持 `.`/`..`。 |
| create | Y | — | `ext4_vn_create`（`ext4_namei.c`）。分配 inode，写入目录项。 |
| mkdir | Y | — | `ext4_vn_mkdir`（`ext4_namei.c`）。为 `.`/`..` 分配一个 block。 |
| unlink | Y | — | `ext4_vn_unlink`（`ext4_namei.c`）。目录以 `-EISDIR` 拒绝。 |
| rmdir | Y | — | `ext4_vn_rmdir`（`ext4_namei.c`）。检查除 `.`/`..` 外为空。 |
| rename | Y | — | `ext4_vn_rename`（`ext4_namei.c`）。支持普通替换、`RENAME_NOREPLACE` 与 `RENAME_EXCHANGE`。 |
| link | Y | — | `ext4_vn_link`（`ext4_namei.c`）。递增 `i_links_count`；拒绝目录；失败时回滚。 |
| symlink | Y | — | `ext4_vn_symlink`（`ext4_namei.c`）。只支持 fast symlink（target <= 60 bytes）。 |
| readlink | Y | — | `ext4_readlink`（`ext4_namei.c`）。从 `i_block` 读取最多 60 字节。 |
| stat | Y | — | `ext4_stat`（`ext4_namei.c`）。`st_nlink` 来自 `i_links_count`。 |
| truncate | Y | — | `ext4_vn_truncate`（`ext4.c`）。零大小截断全部 block；非零大小释放 EOF 之后的 block（collect-rebuild / 间接块裁剪）。支持 64 位文件大小（`i_size_high`）。 |
| chmod | Y | — | `ext4_vn_chmod`（`ext4_namei.c`）。将 `i_mode` 写到磁盘。 |
| chown | Y | — | `ext4_vn_chown`（`ext4_namei.c`）。写入 `i_uid`/`i_gid`；清除 suid/sgid。 |
| read/write/lseek | Y | — | `g_ext4_fops`（`ext4_file.c`）。 |
| readdir | Y | — | `ext4_freaddir`（`ext4_file.c`）。返回 `DT_DIR`/`DT_REG`/`DT_LNK`。 |
| ioctl | N | `-ENOTTY` | `.ioctl` 为 `NULL`。 |
| fsync | partial | — | 同步 block cache；运行时 mutation 不写 JBD2 journal，因此 metadata 和 data 没有 ext4 ordered/journal 保证。 |
| xattr | partial | — | 无 ext4 xattr 后端 hook；reg/dir/lnk 的值只进入全局 RAM 表。 |

**ext4 顺序保证**

- inode/block allocation 由 `alloc_lock` 保护，namespace mutation 由 `metadata_lock` 串行；运行时没有 journal transaction 或 ordered writeback。
- 普通 `ext4_vn_rename` 通过一组目录项更新完成，`RENAME_EXCHANGE` 交换两侧 inode；这些运行时更新不受 journal transaction 保护，断电原子性不等同 Linux ext4。
- `vfs_fsync` 会先同步共享脏映射和 vnode page cache，再对 mount 的 block cache 调用 `bcache_sync`（`kernel/fs/vfs/file.c`）。

**ext4 ABI 缺口**

- 只支持 fast symlink；更长 target 返回 `-ENAMETOOLONG`（`ext4.c`）。
- hard link 已实现；`st_nlink` 来自 `i_links_count`（unlink 递减，link 递增）。
- 挂载时对带 journal 的镜像执行 **JBD2 recovery**（`EXT4_FEATURE_INCOMPAT_RECOVER` 已在 `unsupported_incompat` 中显式排除，`ext4_journal_recover` 在挂载时运行），recovery 失败则 fail closed 拒绝挂载。
- 没有 xattr。
- mount 时做 fail-closed feature 检查：不支持的 incompat 特性（meta_bg、bigalloc、inline_data、casefold、encryption、MMP）会拒绝挂载，而不是静默误读镜像。

### 3.3 NTFS（`kernel/fs/diskfs/ntfs.c`）

NTFS 从 block cache 挂载，直接解析 MFT（master file table）记录，支持 `$I30` 目录索引（index root + index allocation block）。`ntfs_sb_t` 由 `ntfs_lock` 保护。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `ntfs_lookup`（`ntfs_namei.c`）。通过 `ntfs_read_directory` 枚举 `$I30` 索引。 |
| create | Y | — | `ntfs_vn_create`。构建 FILE record（`$STANDARD_INFORMATION`/`$FILE_NAME`/`$DATA`）并插入父目录索引。 |
| mkdir | Y | — | 创建带 `$INDEX_ROOT` 的空目录 record。 |
| unlink | Y | — | `ntfs_vn_unlink`（`ntfs_namei.c`）。从索引移除，释放数据 cluster 与 MFT record。 |
| rmdir | Y | — | `ntfs_vn_rmdir`（`ntfs_namei.c`）。目录必须为空。 |
| rename | Y | — | `ntfs_vn_rename`。移除旧索引项，改写子 record 的 `$FILE_NAME`（新 parent ref + 新 name），插入新索引项；失败回滚。 |
| link | N | `-ENOSYS` | 无 `.link` op。 |
| symlink | N | `-ENOSYS` | 无 `.symlink`/`.readlink` ops。 |
| stat | Y | — | `ntfs_stat`（`ntfs_namei.c`）。`st_nlink` 为 1。 |
| statfs | Y | — | `ntfs_statfs`（`ntfs_namei.c`）。`f_bfree` 是估算值（total/2）。 |
| truncate | Y | — | `ntfs_vn_truncate`（`ntfs.c`）。resident 数据就地收缩，或裁剪非 resident run。 |
| read/write/lseek | Y | — | `g_ntfs_fops`。支持 resident 与 non-resident（runlist 编码）数据。 |
| readdir | Y | — | `ntfs_freaddir`。从 `$I30` 索引枚举。 |
| ioctl | N | `-ENOTTY` | 无 `.ioctl` op。 |

**NTFS 顺序保证**

- 所有 mutation 在 `ntfs_lock` 下串行化。
- rename 在改写 `$FILE_NAME` 后、插入新索引项前，若失败会恢复旧索引项，避免名字丢失。
- 无 `$LogFile`/`$MFTMirr` 处理；崩溃一致性不保证。
- 压缩/加密属性被拒绝（`ntfs.c`）；`$ATTRIBUTE_LIST`（跨 record 属性）不支持。

**NTFS ABI 缺口**

- 没有 hard link、symbolic link 或 rename 之外的名字操作。
- 索引插入只支持 root 或既有 allocation block 内追加，索引满时返回 `-ENOSPC`（无 B-tree split）。
- `$MFT` bitmap 不维护；通过扫描 MFT record 的 in-use flag 找空闲记录。
- 非 ASCII 文件名变成 `?`。

### 3.4 ISO9660（`kernel/fs/diskfs/isofs.c`）

ISO9660 是只读 CD-ROM 文件系统，从 block cache 挂载。在逻辑块 16 处扫描主卷描述符（PVD），按逻辑块大小（512–4096）读取目录记录；文件数据直接按 extent × block_size + 偏移从 block cache 读取。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `isofs_lookup`。名字转小写、去 `;1` 版本。 |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-EROFS` | 只读挂载；VFS 在 `vfs_vnode_permission` 对 `W_OK` 返回 `-EROFS`。 |
| stat | Y | — | `isofs_stat`。`st_blksize=2048`。 |
| statfs | Y | — | `isofs_statfs`。`f_type=0x9660`。 |
| read/lseek | Y | — | `g_isofs_fops`。只读取 walker 最终选中的单个 extent，不聚合 multi-extent 记录。 |
| readdir | Y | — | `isofs_freaddir`。返回 `DT_DIR`/`DT_REG`；multi-extent 名字只由最终未置 continuation flag 的记录暴露。 |
| ioctl | N | `-ENOTTY` | 无 `.ioctl` op。 |

**ISO9660 顺序保证**

- 目录记录可跨块边界（`isofs_read_dirent` 处理跨块组装）。
- multi-extent walker 跳过所有设置 `0x80` continuation flag 的记录，并把最后一个未设置该 flag 的记录当作普通文件条目。因此当前实际暴露的是最终 extent，而不是首 extent；前面的 extent 不可见，也没有聚合读取，不能描述为“读首 extent 后返回垃圾”。
- 名字总是转成小写（ISO 原为大写 8.3 风格）；`;1` 版本号被剥离。

**ISO9660 ABI 缺口**

- 没有 Rock Ridge（长名字、symlink、POSIX 权限）。
- 没有 Joliet（补充卷描述符未解析）。
- 只读；所有 mutation 返回 `-EROFS`。

### 3.5 ramfs（`kernel/fs/diskfs/ramfs.c`）

ramfs 是 root filesystem，也是 `/dev/shm` 和显式 `tmpfs`/`ramfs` mount 的后端。它使用单个全局 inode table，每个目录有固定的 `RAMFS_MAX_INODES`（4096）和 `RAMFS_MAX_DIR_ENTRIES`（256）上限。
| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `ramfs_vnode_lookup`（`ramfs.c`）。 |
| create | Y | — | `ramfs_vnode_create`（`ramfs.c`）。 |
| mkdir | Y | — | `ramfs_vnode_mkdir`（`ramfs.c`）。 |
| unlink | Y | — | `ramfs_vnode_unlink`（`ramfs.c`）。递减 `nlink`，可能释放。 |
| rmdir | Y | — | `ramfs_vnode_rmdir`（`ramfs.c`）。empty 检查统计 active entry > 2。 |
| rename | Y | — | `ramfs_vnode_rename`（`ramfs.c`）。仅同一 mount（由 VFS 强制）。 |
| link | Y | — | `ramfs_vnode_link`（`ramfs.c`）。拒绝目录。 |
| symlink | Y | — | `ramfs_vnode_symlink`（`ramfs.c`）。 |
| readlink | Y | — | `ramfs_vnode_readlink`（`ramfs.c`）。 |
| stat | Y | — | `ramfs_vnode_stat`（`ramfs.c`）。`st_nlink` 来自 inode。 |
| truncate | Y | — | `ramfs_vnode_truncate`（`ramfs.c`）。 |
| chmod | Y | — | `ramfs_vnode_chmod`（`ramfs.c`）。 |
| chown | Y | — | `ramfs_vnode_chown`（`ramfs.c`）。 |
| read/write/lseek | Y | — | `g_ramfs_fops`（`ramfs.c`）。 |
| readdir | Y | — | `ramfs_freaddir`（`ramfs.c`）。 |
| ioctl | N | `-ENOTTY` | 无 `.ioctl` op。 |
| fsync | Y（no-op） | — | `vfs_fsync` 同步 block cache；ramfs 没有 block cache，因此实际为 no-op。 |
| xattr | partial | — | 存储在全局 RAM 表（`kernel/fs/xattr.c`），重启后丢失。 |

**ramfs 顺序保证**

- namespace/inode metadata 由全局 `g_ramfs_meta_lock` 串行，regular-file 内容由每 inode 的 `data_lock` 保护；这不是“隐式单线程、无 per-inode lock”模型。
- `ramfs_vnode_link` 正确递增 `nlink`（`ramfs.c`）。
- `ramfs_vnode_unlink` 递减 `nlink`，并在 `nlink == 0 && ref_count <= 1` 时释放 inode（`ramfs.c`）。

**ramfs ABI 缺口**

- 每目录 entry 上限为 256（`ramfs.c`）。
- 总 inode 上限为 4096（`ramfs.c`）。
- 没有持久化；xattr 也只是全局 RAM 状态。

### 3.6 devfs（`kernel/fs/devfs/devfs.c`）

devfs 是合成设备树。`g_nodes` 提供编译期静态节点；驱动核心虽注册 char/block/net/input/display/audio 六类 class 设备，但通用动态 devfs 节点只为 char/block/audio 生成 `/dev/charN`、`diskN`、`audioN`。input/display 继续通过静态 `/dev/event0` 与 `/dev/fb0` 聚合节点消费，net 没有通用动态 devfs 节点。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `devfs_lookup`。支持 root、`misc/`、`pts/` 目录与 char/block/audio 动态 class 节点。 |
| create | N | `-ENOSYS` | 无 `.create` op；带 `O_CREAT` 的 `vfs_open` 返回 `-ENOSYS`。 |
| mkdir | N | `-ENOTDIR` | 无 `.mkdir` op；`vfs_mkdir` 返回 `-ENOTDIR`。 |
| unlink | N | `-ENOTDIR` | 无 `.unlink` op；`vfs_unlink` 返回 `-ENOTDIR`。 |
| rmdir | N | `-ENOSYS` | 无 `.rmdir` op；`vfs_rmdir` 返回 `-ENOSYS`。 |
| rename | N | `-ENOSYS` | 无 `.rename` op。 |
| link | N | `-ENOSYS` | 无 `.link` op。 |
| symlink | N | `-ENOSYS` | 无 `.symlink` op。 |
| stat | Y | — | `devfs_stat`。报告 `S_IFCHR`/`S_IFBLK`/`S_IFDIR`。 |
| chmod/chown | N | `-EPERM` | 无 `.chmod`/`.chown` ops；`vfs_chmod_vnode` 返回 `-EPERM`。 |
| open | Y | — | `devfs_open_vnode`。分发到 per-device `vfile_ops_t`。 |
| read/write/ioctl | Y | — | per-kind `vfile_ops_t` 表。 |
| readdir | Y | — | `devfs_dir_readdir`。 |

**devfs 顺序保证**

- 内建 `g_nodes` 是静态表；char/block/audio class device 会通过 `class_device_get_nth/get_by_name` 动态枚举和引用，节点释放时归还 class-device 引用。不能把整个 devfs 描述为 init 后静态不变，也不能把该动态节点机制外推到 net/input/display。

**devfs ABI 缺口**

- 不能创建、删除或 rename 设备节点。
- 不支持 `chmod`/`chown`。
- char/block/audio class 设备的节点名由驱动核心按 `class_device_get_nth` 生成（`/dev/charN` 等），节点内容由驱动 probe 决定；net/input/display 不走该通用 devnode 路径，uevent 驱动的动态命名尚未实现。

### 3.7 procfs（`kernel/fs/procfs/procfs.c`）

procfs 是完全合成的文件系统。entry 在 `lookup` 和 `open` 时生成。没有后端 mutation 操作。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `procfs_lookup`（`kernel/fs/procfs/procfs.c`）。数字 PID 和静态 entry。 |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-ENOSYS`/`-ENOTDIR` | `g_procfs_vnode_ops` 只定义 `lookup`、`readlink`、`stat`、`open`、`release`（`kernel/fs/procfs/procfs.c`）。 |
| stat | Y | — | `procfs_stat`（`kernel/fs/procfs/procfs.c`）。 |
| open | Y | — | `procfs_open_vnode`（`kernel/fs/procfs/procfs.c`）。分配 `procfs_priv_t` snapshot。 |
| read | Y | — | `procfs_fread`（`kernel/fs/procfs/procfs.c`）。 |
| write | partial | `-EINVAL` | 只有特定 tunable 接受写入（`procfs_fwrite`，`kernel/fs/procfs/procfs.c`）。 |
| lseek | Y | — | `procfs_flseek`（`kernel/fs/procfs/procfs.c`）。 |
| readdir | Y | — | `procfs_freaddir`（`kernel/fs/procfs/procfs.c`）。 |
| chmod/chown | N | `-EPERM` | 无后端 hook。 |

**procfs 顺序保证**

- 内容在 `open` 时生成并缓存在 `procfs_priv_t` 中；并发 process 状态变化不会在 open 后反映出来。
- 可写 tunable 的同步策略不统一：`pid_max` 经 `proc_set_pid_max` 更新，`oom_score_adj` 和 `pipe-max-size` 则直接更新对应字段或全局值。

**procfs ABI 缺口**

- 许多 `/proc/<pid>` 文件只是占位符，返回空内容或静态内容。
- `/proc/self/exe` 和 `/proc/<pid>/exe`/`cwd` 在 `vfs_readlinkat` 中作为特殊情况处理（`kernel/fs/vfs.c`），不是真正的 symlink。
- 不允许文件 mutation。

### 3.8 sysfs（`kernel/fs/sysfs.c`）

sysfs 是合成树。除 `/sys/block/loopN/{dev,size,uevent}` 与 `/sys/class/drm/card0/...` 外，当前还按驱动核心的 class registry 动态暴露 `/sys/class/{char,block,net,input,display,audio}/<device>/dev`。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `sysfs_lookup`（`kernel/fs/sysfs.c`）。 |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-ENOSYS`/`-ENOTDIR` | `g_sysfs_vnode_ops` 只定义 `lookup`、`stat`、`open`、`release`（`kernel/fs/sysfs.c`）。 |
| stat | Y | — | `sysfs_stat`（`kernel/fs/sysfs.c`）。 |
| open/read/lseek/readdir | Y | — | `g_sysfs_fops`（`kernel/fs/sysfs.c`）。 |
| write | N | `-EINVAL` | 未注册 `.write`。 |
| chmod/chown | N | `-EPERM` | 无后端 hook。 |

**sysfs 顺序保证**

- 内容在 lookup/open/readdir 时由静态视图和动态 class-device registry 合成；动态节点持有 class-device 引用。
- sysfs 本身不提供用户 mutation op，但底层 class registry 可随驱动 bind/remove 改变。

**sysfs ABI 缺口**

- 视图仍远小于 Linux sysfs；动态 class 只提供设备名与 `dev` 等最小属性，不是完整 kobject/uevent 层级。
- 没有 writable attribute，也没有 uevent write。

### 3.9 pipe（`kernel/fs/pipe.c`）

pipe 不是挂载文件系统。它创建一对共享 `pipe_buf_t` 环形缓冲区的 `vfile_t` 对象。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| create (pipe2) | Y | — | `pipe_create`（`kernel/fs/pipe.c`）。分配环形缓冲区和两个全局 fd。 |
| read | Y | — | `pipe_read`（`kernel/fs/pipe.c`）。除非 `O_NONBLOCK`，否则阻塞。 |
| write | Y | — | `pipe_write`（`kernel/fs/pipe.c`）。不超过 `PIPE_BUF_SIZE`（4096）字节的写入具备原子性；更大写入可能交错。 |
| poll | Y | — | `pipe_poll_events`（`kernel/fs/pipe.c`）。 |
| set_size | Y | — | `pipe_set_size`（`kernel/fs/pipe.c`），在 `vfs_fcntl` 中受 `CAP_SYS_RESOURCE` 限制。 |
| lseek | N | `-ESPIPE` | 无 `.lseek` op；`vfs_lseek` 返回 `-ESPIPE`。 |

**pipe 顺序保证**

- `PIPE_BUF_SIZE`（4096）字节或更少的写入相互之间是原子的（`kernel/fs/pipe.c`）；复制整个 chunk 时持有 spinlock。
- 大于 `PIPE_BUF_SIZE` 的写入会被拆分，可能与其他 writer 交错。
- `read`/`write` 在 wait queue 上阻塞，并通过 `pipe_wake_readers`/`pipe_wake_writers` 唤醒所有对应 waiter（`kernel/fs/pipe.c`）。
- 关闭最后一个 reader 会唤醒 writer；后续或已阻塞的 write 检测到该状态后向当前 task 发送 `SIGPIPE` 并返回 `-EPIPE`（`kernel/fs/pipe.c`）。

**pipe ABI 缺口**

- 内核 `PIPE_BUF_SIZE` 定义于 `kernel/include/core/consts.h`，值为 4096，与 Linux ABI 的 `PIPE_BUF` 预期一致。
- `F_SETPIPE_SZ` 对非特权 task 的容量限制硬编码为 1 MiB（`kernel/fs/vfs.c`）。

### 3.10 anonfd（`kernel/fs/anonfd.c`）

anonfd 是把匿名 vfile 安装到当前 fd table 的 helper。它不是文件系统，没有 path 语义。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| install | Y | — | `anonfd_install_vfile`（`kernel/fs/anonfd.c`）。 |
| close | Y | — | 通过 `anonfd_free_priv_close` 释放 `vf->priv`（`kernel/fs/anonfd.c`）。 |

**anonfd 顺序保证**

- install 和 close 相对于调用 task 的 `fdtable` 是单线程的。

**anonfd ABI 缺口**

- 按设计没有缺口；它是内部 helper，不是 Linux ABI 表面。

## 4. VFS 横切行为

### 4.1 Permission 模型

- `vfs_vnode_permission` 使用当前 task 的 `fsuid`/`fsgid` 调用 `vfs_mode_has_perm_ids`（`kernel/fs/vfs/stat_perm.c`）。
- `CAP_DAC_OVERRIDE` 绕过 read/write，但仍拒绝对没有 execute bit 的 regular file 执行（`kernel/fs/vfs/stat_perm.c`）。
- 不带 `AT_EACCESS` 的 `access()` / `faccessat()` 使用 real uid/gid，且不使用 capability（`vfs_mode_has_perm_ids_nocap`，`kernel/fs/vfs/stat_perm.c`）。
- `vfs_chmod_vnode` 要求所有权或 `CAP_FOWNER`（`kernel/fs/vfs_stat.c`）。
- `vfs_chown_vnode` 要求 `CAP_CHOWN` 或满足受限 ownership 规则（`kernel/fs/vfs_stat.c`）。
- sticky-bit 删除检查在 `vfs_sticky_may_remove` 中应用（`kernel/fs/vfs/stat_perm.c`）。

### 4.2 Path resolution

- `vnode_lookup_path` 按当前 task 的 `root_path`、起始 dirfd 与 resolve context 解析路径。
- walk 期间按 flag 跟随 symlink，深度限制为 `MAX_SYMLINKS`（40）。
- mount root 的 `..` 通过 mount 表回到覆盖点的父目录；`RESOLVE_NO_XDEV` 会拒绝该跨越，chroot/`IN_ROOT` 边界仍优先约束逃逸。
- 名称长度 >= `MAX_NAME_LEN` 返回 `-ENAMETOOLONG`（`kernel/fs/vfs/path_resolution.c`）。
- `vfs_resolve_no_follow_final` 解析父目录，并在不跟随最终 component 的情况下 lookup 它（`kernel/fs/vfs.c`）。

### 4.3 Mount 操作

- `vfs_mount` 支持 `tmpfs`/`ramfs`、`cgroup`/`cgroup2`、`proc`、`sysfs`、`devtmpfs`/`devfs`，以及 `fat32`/`vfat`/`ext4`/`ntfs`/`isofs`/`iso9660` 的块设备 mount（`kernel/fs/vfs/mount_ops.c`）。
- 当块设备 mount 的 fstype 为空时，`mount_block_dev_idx` 先尝试 `ext4`，再尝试 `vfat`（`kernel/fs/vfs/mount_ops.c`）。
- `vfs_umount` 按 normalized path 匹配；FAT32、ext4、NTFS 和 ISO9660 会调用各自的 unmount helper，然后移除 mount（`kernel/fs/vfs/mount_ops.c`）。
- `vfs_rename_flags` 用 `-EXDEV` 拒绝 cross-mount rename（`kernel/fs/vfs.c`）。

### 4.4 Dcache

- dcache 只对 ramfs、FAT32、ext4 启用；它是 512 项 lookup 优化层，不是权威 namespace。`RESOLVE_CACHED` 通过 dcache 命中检查实现：任意组件未缓存即返回 `-EAGAIN`，由调用方去 flag 重试。
- create、unlink、rmdir、rename、link、symlink 等路径多数按 `(parent, name)` 定点失效；mount/umount、部分全局状态变更和无法安全定点的情况仍调用 `vfs_dcache_invalidate_all`。

### 4.5 xattr

- xattr 存储在按 `(mnt, ino)` 索引的 1024 项全局 RAM 表 `g_xattrs` 中（`kernel/fs/xattr.c`）。
- 遵守 `XATTR_CREATE` 和 `XATTR_REPLACE` flag（`kernel/fs/xattr.c`）。
- name 限制为 `XATTR_NAME_MAX_LOCAL`；value 限制为 `XATTR_VALUE_MAX_LOCAL`。
- 没有后端特定 xattr hook；FAT32/ext4 的值不会写入磁盘，并会在 unmount/reboot 后丢失。

## 5. 已知 Linux ABI 缺口（汇总）

| 领域 | 当前行为 | 期望的 Linux 行为 |
|------|------------------|-------------------------|
| openat2 resolve flag | `NO_XDEV`/`NO_SYMLINKS`/`BENEATH`/`IN_ROOT`/`CACHED` 有实现（`CACHED` 未命中返回 `-EAGAIN`，编号 `0x20` 与 Linux 一致）；`NO_MAGICLINKS` 被接受但未执行检查 | Linux 编号和可扩展 `open_how` size 语义 |
| renameat2 flag | VFS 接受 `NOREPLACE/EXCHANGE`；ramfs/ext4 支持两者，FAT32/NTFS 只支持 `NOREPLACE`，whiteout 不支持 | 后端一致支持与 overlay whiteout |
| statx | 不是 requested-only：实现总把 `STATX_BASIC_STATS` 加入 `stx_mask` 并返回基础元数据，再按请求追加 `STATX_BTIME` 等可选字段；支持 sync type 子集 | 完整遵守 `AT_STATX_*`/`STATX_*` 及字段可用性语义 |
| fchmodat2 | 带 flags 分发到 `sys_fchmodat`（`kernel/abi/linux/syscall_table.def`） | 独立 syscall，完整处理 `AT_*` flag |
| xattr | 只有全局 RAM 表 | 具备 namespace 检查的后端持久化 xattr |
| Symlink loop limit | 40（`MAX_SYMLINKS`，`kernel/include/fs/vfs.h`） | Linux 使用 40（`MAX_SYMLINKS`） |
| Mount-point `..` | 已跨到 parent mount，并受 `NO_XDEV`/root 边界约束 | 仍需更多嵌套 mount/chroot 组合覆盖 |
| chroot | 已实现：`root_path` 在路径解析中生效（`vfs_path_normalize_absolute_with_root`，`path_resolution.c`） | resolution 必须受 `root_path` 约束 |
| faccessat2 flag | 支持 `AT_EACCESS`/`AT_SYMLINK_NOFOLLOW`/`AT_EMPTY_PATH`，未支持 bit 返回 `-EINVAL`（`kernel/fs/vfs_stat.c`） | 同时严格校验不支持的 bit |

## 6. 测试影响

- 上方矩阵中的每个 unsupported operation 都必须有测试断言精确 errno。
- Cross-mount `rename` 和 `link` 必须返回 `-EXDEV`。
- Permission 测试必须覆盖 real uid/gid（`access`）、effective uid/gid（`faccessat2(AT_EACCESS)`）、capability bypass 和 sticky-bit 目录。
- FAT32/ext4 必须有 fsync/truncate/page-cache coherence 测试。
- Pipe 测试必须验证 `PIPE_BUF` 大小写入的原子性和 `SIGPIPE` 投递。

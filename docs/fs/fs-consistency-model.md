# A20OS 文件系统一致性模型

本文档记录 A20OS P1 中各后端的能力、一致性和 Linux ABI 行为矩阵。每条声明都基于 `kernel/fs/` 下的当前实现和 `kernel/fs/vfs.c` 中的 VFS wrapper。它是 Wave 1 实现以及后续测试门禁的设计契约。

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
| **Support** | `Y` = 已实现并被测试覆盖，`N` = 返回明确 errno，`-` = 不适用 |
| **Errno** | 不支持或错误路径返回的 errno（来自代码，不是意图） |
| **Ordering / Atomicity** | 当前代码实际提供的保证 |
| **Linux ABI gap** | 测试必须编码的 Linux 差异 |

## 3. 各后端矩阵

### 3.1 FAT32（`kernel/fs/diskfs/fat32.c`）

FAT32 从 virtio-blk block cache 挂载。superblock 由 `fat32_sb_t.lock` 保护。per-inode metadata（mode、uid、gid）保存在按 cluster number 索引的全局 RAM 表 `g_fat32_meta` 中（`fat32.c:295`）。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `fat32_lookup`（`fat32.c:321`）。支持 `.` 和 `..`。大小写不敏感的 8.3 + LFN。 |
| create | Y | — | `fat32_vn_create`（`fat32.c:467`）。分配一个 cluster，写入 8.3 short entry。 |
| mkdir | Y | — | `fat32_vn_mkdir`（`fat32.c:381`）。创建 `.`/`..` entry。 |
| unlink | Y | — | `fat32_vn_unlink`（`fat32.c:535`）。释放 cluster chain。目录返回 `-EISDIR`。 |
| rmdir | Y | — | `fat32_vn_rmdir`（`fat32.c:708`）。检查 `fat32_dir_is_empty`（统计 active entry > 2）。 |
| rename | Y | — | `fat32_vn_rename`（`fat32.c:959`）。创建新目录项并删除旧项；移动目录时改写其 `..` entry。支持 `RENAME_NOREPLACE`；替换目标。 |
| link | N | `-ENOSYS` | `g_fat32_vnode_ops` 中没有 `.link` op。`vfs_link` 返回 `-ENOSYS`（`vfs.c:822`）。 |
| symlink | N | `-ENOSYS` | 没有 `.symlink`/`.readlink` ops。`vfs_symlink` 返回 `-ENOSYS`（`vfs.c:861`）。 |
| readlink | N | `-EINVAL` | symlink vnode 不可能存在；`vfs_readlinkat` 返回 `-EINVAL`（`vfs.c:771`）。 |
| stat | Y | — | `fat32_stat`（`fat32.c:357`）。`st_nlink` 硬编码为 1。`st_blocks` 按 512 向上取整。 |
| truncate | Y | — | `fat32_vn_truncate`（`fat32.c:574`）。size 0 会重新分配单个 cluster。 |
| chmod | Y | — | `fat32_vn_chmod`（`fat32.c:655`）。只存储在 RAM meta table 中。 |
| chown | Y | — | `fat32_vn_chown`（`fat32.c:669`）。只存储在 RAM meta table 中；清除 suid/sgid bit。 |
| read/write/lseek | Y | — | `g_fat32_fops`（`fat32.c:1149`）。每次 open 有一个带 cluster cache 的 `fat32_fctx_t`。 |
| readdir | Y | — | `fat32_freaddir`（`fat32.c:1046`）。返回 `DT_DIR`/`DT_REG`；无 `DT_LNK`。 |
| ioctl | N | `-ENOTTY` | `.ioctl` 为 `NULL`；`vfs_ioctl` 落到 `-ENOTTY`。 |
| fsync | partial | — | `vfs_fsync` 同步 block cache（`vfs/file.c:232`），但 FAT32 没有显式 inode log。 |
| xattr | N | `-EOPNOTSUPP` | 无后端 hook；`sys_xattr_*` 拒绝非 reg/dir/lnk，随后落到 RAM 表；该表只在已挂载 vnode 生命周期内存在。 |

**FAT32 顺序保证**

- 整个文件系统由 `sb->lock` 串行化（`fat32.c:271`）。
- 目录项更新和 FAT 更新之间不是原子的；如果在释放 cluster 后、标记目录项删除前崩溃，可能泄漏 cluster。
- 文件大小只在 close 时写回目录项（`fat32_fclose`，`fat32.c:1105`）。close 前断电会丢失 size。
- block cache 是 write-back；`vfs_fsync` 和 unmount（`fat32_unmount`，`fat32.c:1218`）会调用 `bcache_sync`。

**FAT32 ABI 缺口**

- 没有 hard link 或 symbolic link（rename 已实现）。
- 文件 ownership/mode 是易失的（只存在 RAM）。
- 没有 atime/mtime/ctime 持久化；timestamp 在 `vfs_vnode_stat` 中回退为当前时间（`vfs/stat_perm.c:188`）。
- `st_nlink` 总是 1。
- 没有 xattr 持久化。

### 3.2 ext4（`kernel/fs/diskfs/ext4.c`）

ext4 从 block cache 挂载，并使用短生命周期 vnode 模型：每次 lookup 都创建新的 vnode，refcount 到零时释放（`ext4.c:67`）。inode cache hook 是 stub（`ext4.c:75`）。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `ext4_lookup`（`ext4.c:731`）。支持 `.`/`..`。 |
| create | Y | — | `ext4_vn_create`（`ext4.c:802`）。分配 inode，写入目录项。 |
| mkdir | Y | — | `ext4_vn_mkdir`（`ext4.c:848`）。为 `.`/`..` 分配一个 block。 |
| unlink | Y | — | `ext4_vn_unlink`（`ext4.c:934`）。目录以 `-EISDIR` 拒绝。 |
| rmdir | Y | — | `ext4_vn_rmdir`（`ext4.c:971`）。检查除 `.`/`..` 外为空。 |
| rename | Y | — | `ext4_vn_rename`（`ext4.c:1150`）。替换现有目标，不支持 exchange。 |
| link | Y | — | `ext4_vn_link`（`ext4.c:1377`）。递增 `i_links_count`；拒绝目录；失败时回滚。 |
| symlink | Y | — | `ext4_vn_symlink`（`ext4.c:1206`）。只支持 fast symlink（target <= 60 bytes）。 |
| readlink | Y | — | `ext4_readlink`（`ext4.c:1190`）。从 `i_block` 读取最多 60 字节。 |
| stat | Y | — | `ext4_stat`（`ext4.c:846`）。`st_nlink` 来自 `i_links_count`。 |
| truncate | Y | — | `ext4_vn_truncate`（`ext4.c:1249`）。零大小截断全部 block；非零大小释放 EOF 之后的 block（collect-rebuild / 间接块裁剪）。支持 64 位文件大小（`i_size_high`）。 |
| chmod | Y | — | `ext4_vn_chmod`（`ext4.c:1111`）。将 `i_mode` 写到磁盘。 |
| chown | Y | — | `ext4_vn_chown`（`ext4.c:1121`）。写入 `i_uid`/`i_gid`；清除 suid/sgid。 |
| read/write/lseek | Y | — | `g_ext4_fops`（`ext4.c:1394`）。 |
| readdir | Y | — | `ext4_freaddir`（`ext4.c:1332`）。返回 `DT_DIR`/`DT_REG`/`DT_LNK`。 |
| ioctl | N | `-ENOTTY` | `.ioctl` 为 `NULL`。 |
| fsync | partial | — | 同步 block cache；未使用 journal，因此 metadata 和 data 没有顺序保证。 |
| xattr | N | `-EOPNOTSUPP` | 无后端 hook。 |

**ext4 顺序保证**

- inode allocation、block allocation 和目录项写入在 `sb->alloc_lock` 下执行（`ext4.c:166`、`ext4.c:188`、`ext4.c:205`），但没有 journal 或 ordered writeback。
- `ext4_vn_rename`（`ext4.c:991`）会移除目标、加入新 entry，再移除旧 entry。崩溃后可能两个 entry 都存在，也可能都不存在。
- `vfs_fsync` 会对 mount 的 block cache 调用 `bcache_sync`（`vfs/file.c:232`）。

**ext4 ABI 缺口**

- 只支持 fast symlink；更长 target 返回 `-ENAMETOOLONG`（`ext4.c:1219`）。
- hard link 已实现；`st_nlink` 来自 `i_links_count`（unlink 递减，link 递增）。
- 没有 journaling；metadata 更新跨多个 block 时不是原子的。
- 没有 xattr。
- mount 时做 fail-closed feature 检查：不支持的 incompat 特性（journal/recover、meta_bg、bigalloc、inline_data、casefold、encryption、MMP）会拒绝挂载，而不是静默误读镜像。

### 3.3 NTFS（`kernel/fs/diskfs/ntfs.c`）

NTFS 从 block cache 挂载，直接解析 MFT（master file table）记录，支持 `$I30` 目录索引（index root + index allocation block）。`ntfs_sb_t` 由 `ntfs_lock` 保护。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `ntfs_lookup`（`ntfs.c:1294`）。通过 `ntfs_read_directory` 枚举 `$I30` 索引。 |
| create | Y | — | `ntfs_vn_create`。构建 FILE record（`$STANDARD_INFORMATION`/`$FILE_NAME`/`$DATA`）并插入父目录索引。 |
| mkdir | Y | — | 创建带 `$INDEX_ROOT` 的空目录 record。 |
| unlink | Y | — | `ntfs_vn_unlink`（`ntfs.c:1534`）。从索引移除，释放数据 cluster 与 MFT record。 |
| rmdir | Y | — | `ntfs_vn_rmdir`（`ntfs.c:1568`）。目录必须为空。 |
| rename | Y | — | `ntfs_vn_rename`。移除旧索引项，改写子 record 的 `$FILE_NAME`（新 parent ref + 新 name），插入新索引项；失败回滚。 |
| link | N | `-ENOSYS` | 无 `.link` op。 |
| symlink | N | `-ENOSYS` | 无 `.symlink`/`.readlink` ops。 |
| stat | Y | — | `ntfs_stat`（`ntfs.c:1325`）。`st_nlink` 为 1。 |
| statfs | Y | — | `ntfs_statfs`（`ntfs.c:1341`）。`f_bfree` 是估算值（total/2）。 |
| truncate | Y | — | `ntfs_vn_truncate`（`ntfs.c:1676`）。resident 数据就地收缩，或裁剪非 resident run。 |
| read/write/lseek | Y | — | `g_ntfs_fops`。支持 resident 与 non-resident（runlist 编码）数据。 |
| readdir | Y | — | `ntfs_freaddir`。从 `$I30` 索引枚举。 |
| ioctl | N | `-ENOTTY` | 无 `.ioctl` op。 |

**NTFS 顺序保证**

- 所有 mutation 在 `ntfs_lock` 下串行化。
- rename 在改写 `$FILE_NAME` 后、插入新索引项前，若失败会恢复旧索引项，避免名字丢失。
- 无 `$LogFile`/`$MFTMirr` 处理；崩溃一致性不保证。
- 压缩/加密属性被拒绝（`ntfs.c:441`）；`$ATTRIBUTE_LIST`（跨 record 属性）不支持。

**NTFS ABI 缺口**

- 没有 hard link、symbolic link 或 rename 之外的名字操作。
- 索引插入只支持 root 或既有 allocation block 内追加，索引满时返回 `-ENOSPC`（无 B-tree split）。
- `$MFT` bitmap 不维护；通过扫描 MFT record 的 in-use flag 找空闲记录。
- 非 ASCII 文件名变成 `?`。

### 3.4 ISO9660（`kernel/fs/diskfs/isofs.c`）

ISO9660 是只读 CD-ROM 文件系统，从 block cache 挂载。在逻辑块 16 处扫描主卷描述符（PVD），按逻辑块大小（512–4096）读取目录记录；文件数据直接按 extent × block_size + 偏移从 block cache 读取。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `isofs_lookup`（`isofs.c:196`）。名字转小写、去 `;1` 版本。 |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-EROFS` | 只读挂载；VFS 在 `vfs_vnode_permission` 对 `W_OK` 返回 `-EROFS`（`stat_perm.c:207`）。 |
| stat | Y | — | `isofs_stat`（`isofs.c:222`）。`st_blksize=2048`。 |
| statfs | Y | — | `isofs_statfs`（`isofs.c:232`）。`f_type=0x9660`。 |
| read/lseek | Y | — | `g_isofs_fops`（`isofs.c:312`）。单 extent 读取。 |
| readdir | Y | — | `isofs_freaddir`（`isofs.c:297`）。返回 `DT_DIR`/`DT_REG`。 |
| ioctl | N | `-ENOTTY` | 无 `.ioctl` op。 |

**ISO9660 顺序保证**

- 目录记录可跨块边界（`isofs_read_dirent` 处理跨块组装）。
- multi-extent（`0x80`）延续记录被跳过；超过首 extent 的读取会返回垃圾数据（与 Uinxed 相同限制）。
- 名字总是转成小写（ISO 原为大写 8.3 风格）；`;1` 版本号被剥离。

**ISO9660 ABI 缺口**

- 没有 Rock Ridge（长名字、symlink、POSIX 权限）。
- 没有 Joliet（补充卷描述符未解析）。
- 只读；所有 mutation 返回 `-EROFS`。

### 3.5 ramfs（`kernel/fs/diskfs/ramfs.c`）

ramfs 是 root filesystem，也是 `/dev/shm` 和显式 `tmpfs`/`ramfs` mount 的后端。它使用单个全局 inode table，每个目录有固定的 `RAMFS_MAX_INODES`（4096）和 `RAMFS_MAX_DIR_ENTRIES`（256）上限。
| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `ramfs_vnode_lookup`（`ramfs.c:220`）。 |
| create | Y | — | `ramfs_vnode_create`（`ramfs.c:276`）。 |
| mkdir | Y | — | `ramfs_vnode_mkdir`（`ramfs.c:246`）。 |
| unlink | Y | — | `ramfs_vnode_unlink`（`ramfs.c:323`）。递减 `nlink`，可能释放。 |
| rmdir | Y | — | `ramfs_vnode_rmdir`（`ramfs.c:436`）。empty 检查统计 active entry > 2。 |
| rename | Y | — | `ramfs_vnode_rename`（`ramfs.c:392`）。仅同一 mount（由 VFS 强制）。 |
| link | Y | — | `ramfs_vnode_link`（`ramfs.c:380`）。拒绝目录。 |
| symlink | Y | — | `ramfs_vnode_symlink`（`ramfs.c:353`）。 |
| readlink | Y | — | `ramfs_vnode_readlink`（`ramfs.c:342`）。 |
| stat | Y | — | `ramfs_vnode_stat`（`ramfs.c:234`）。`st_nlink` 来自 inode。 |
| truncate | Y | — | `ramfs_vnode_truncate`（`ramfs.c:489`）。 |
| chmod | Y | — | `ramfs_vnode_chmod`（`ramfs.c:465`）。 |
| chown | Y | — | `ramfs_vnode_chown`（`ramfs.c:472`）。 |
| read/write/lseek | Y | — | `g_ramfs_fops`（`ramfs.c:692`）。 |
| readdir | Y | — | `ramfs_freaddir`（`ramfs.c:650`）。 |
| ioctl | N | `-ENOTTY` | 无 `.ioctl` op。 |
| fsync | Y（no-op） | — | `vfs_fsync` 同步 block cache；ramfs 没有 block cache，因此实际为 no-op。 |
| xattr | partial | — | 存储在全局 RAM 表（`kernel/fs/xattr.c`），重启后丢失。 |

**ramfs 顺序保证**

- 所有 ramfs 操作都是内存内操作，并由大内核隐式单线程路径串行化（无 per-inode lock）。
- `ramfs_vnode_link` 正确递增 `nlink`（`ramfs.c:388`）。
- `ramfs_vnode_unlink` 递减 `nlink`，并在 `nlink == 0 && ref_count <= 1` 时释放 inode（`ramfs.c:76`）。

**ramfs ABI 缺口**

- 每目录 entry 上限为 256（`ramfs.c:11`）。
- 总 inode 上限为 4096（`ramfs.c:10`）。
- 没有持久化；xattr 也只是全局 RAM 状态。

### 3.4 devfs（`kernel/fs/devfs/devfs.c`）

devfs 是合成设备树。它没有可变目录内容；entry 在编译期固定于 `g_nodes`（`devfs.c:71`）。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `devfs_lookup`（`devfs.c:511`）。只支持 root、`misc/` 和 `pts/` 目录。 |
| create | N | `-ENOSYS` | 无 `.create` op；带 `O_CREAT` 的 `vfs_open` 返回 `-ENOSYS`（`vfs.c:95`）。 |
| mkdir | N | `-ENOTDIR` | 无 `.mkdir` op；`vfs_mkdir` 返回 `-ENOTDIR`（`vfs.c:223`）。 |
| unlink | N | `-ENOTDIR` | 无 `.unlink` op；`vfs_unlink` 返回 `-ENOTDIR`（`vfs.c:277`）。 |
| rmdir | N | `-ENOSYS` | 无 `.rmdir` op；`vfs_rmdir` 返回 `-ENOSYS`（`vfs.c:415`）。 |
| rename | N | `-ENOSYS` | 无 `.rename` op。 |
| link | N | `-ENOSYS` | 无 `.link` op。 |
| symlink | N | `-ENOSYS` | 无 `.symlink` op。 |
| stat | Y | — | `devfs_stat`（`devfs.c:551`）。报告 `S_IFCHR`/`S_IFBLK`/`S_IFDIR`。 |
| chmod/chown | N | `-EPERM` | 无 `.chmod`/`.chown` ops；`vfs_chmod_vnode` 返回 `-EPERM`（`vfs.c:586`）。 |
| open | Y | — | `devfs_open_vnode`（`devfs.c:585`）。分发到 per-device `vfile_ops_t`。 |
| read/write/ioctl | Y | — | per-kind `vfile_ops_t` 表（`devfs.c:492`）。 |
| readdir | Y | — | `devfs_dir_readdir`（`devfs.c:176`）。 |

**devfs 顺序保证**

- `vfs_init` 后 devfs node 是静态的；除 per-device 状态（TTY、loop、PTY）外没有并发可变状态。

**devfs ABI 缺口**

- 不能创建、删除或 rename 设备节点。
- 不支持 `chmod`/`chown`。
- `/dev` 内容硬编码；未实现 uevent 驱动的节点创建。

### 3.5 procfs（`kernel/fs/procfs/procfs.c`）

procfs 是完全合成的文件系统。entry 在 `lookup` 和 `open` 时生成。没有后端 mutation 操作。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `procfs_lookup`（`procfs.c:896`）。数字 PID 和静态 entry。 |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-ENOSYS`/`-ENOTDIR` | `g_procfs_vnode_ops` 只定义 `lookup`、`stat`、`open`、`release`（`procfs.c:1038`）。 |
| stat | Y | — | `procfs_stat`（`procfs.c:1001`）。 |
| open | Y | — | `procfs_open_vnode`（`procfs.c:1322`）。分配 `procfs_priv_t` snapshot。 |
| read | Y | — | `procfs_fread`（`procfs.c:1046`）。 |
| write | partial | `-EINVAL` | 只有特定 tunable 接受写入（`procfs_fwrite`，`procfs.c:1086`）。 |
| lseek | Y | — | `procfs_flseek`（`procfs.c:1146`）。 |
| readdir | Y | — | `procfs_freaddir`（`procfs.c:1162`）。 |
| chmod/chown | N | `-EPERM` | 无后端 hook。 |

**procfs 顺序保证**

- 内容在 `open` 时生成并缓存在 `procfs_priv_t` 中；并发 process 状态变化不会在 open 后反映出来。
- 部分可写 tunable（`oom_score_adj`、`pid_max`、`pipe-max-size`）除整数写入周围的全局 spinlock 外没有其他同步。

**procfs ABI 缺口**

- 许多 `/proc/<pid>` 文件只是占位符，返回空内容或静态内容。
- `/proc/self/exe` 和 `/proc/<pid>/exe`/`cwd` 在 `vfs_readlinkat` 中作为特殊情况处理（`vfs.c:694`），不是真正的 symlink。
- 不允许文件 mutation。

### 3.6 sysfs（`kernel/fs/sysfs.c`）

sysfs 是最小合成树。当前只暴露 `/sys/block/loopN/{dev,size,uevent}`。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| lookup | Y | — | `sysfs_lookup`（`sysfs.c:91`）。 |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-ENOSYS`/`-ENOTDIR` | `g_sysfs_vnode_ops` 只定义 `lookup`、`stat`、`open`、`release`（`sysfs.c:191`）。 |
| stat | Y | — | `sysfs_stat`（`sysfs.c:170`）。 |
| open/read/lseek/readdir | Y | — | `g_sysfs_fops`（`sysfs.c:298`）。 |
| write | N | `-EINVAL` | 未注册 `.write`。 |
| chmod/chown | N | `-EPERM` | 无后端 hook。 |

**sysfs 顺序保证**

- 内容在 `open` 时从静态常量生成。
- 不支持并发 mutation。

**sysfs ABI 缺口**

- 只暴露 loop block device。
- 没有 writable attribute，也没有 uevent write。

### 3.7 pipe（`kernel/fs/pipe.c`）

pipe 不是挂载文件系统。它创建一对共享 `pipe_buf_t` 环形缓冲区的 `vfile_t` 对象。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| create (pipe2) | Y | — | `pipe_create`（`pipe.c:354`）。分配环形缓冲区和两个全局 fd。 |
| read | Y | — | `pipe_read`（`pipe.c:70`）。除非 `O_NONBLOCK`，否则阻塞。 |
| write | Y | — | `pipe_write`（`pipe.c:108`）。<= `PIPE_BUF` 的写入具备原子性；更大写入可能交错。 |
| poll | Y | — | `pipe_poll_events`（`pipe.c:313`）。 |
| set_size | Y | — | `pipe_set_size`（`pipe.c:346`），在 `vfs_fcntl` 中受 `CAP_SYS_RESOURCE` 限制。 |
| lseek | N | `-ESPIPE` | 无 `.lseek` op；`vfs_lseek` 返回 `-ESPIPE`。 |

**pipe 顺序保证**

- `PIPE_BUF` 字节或更少的写入相互之间是原子的（`pipe.c:126`）；复制整个 chunk 时持有 spinlock。
- 大于 `PIPE_BUF` 的写入会被拆分，可能与其他 writer 交错。
- `read`/`write` 在 wait queue 上阻塞，并唤醒所有 reader/writer（`pipe.c:27`）。
- 关闭最后一个 reader 会向 writer 发送 `SIGPIPE`/`EPIPE`（`pipe.c:115`、`pipe.c:129`）。

**pipe ABI 缺口**

- `PIPE_BUF` 值定义于 `core/consts.h`；需要验证它在所有架构上都匹配 Linux ABI 对 4096 的预期。
- `F_SETPIPE_SZ` 对非特权 task 的容量限制硬编码为 1 MiB（`vfs.c:1069`）。

### 3.8 anonfd（`kernel/fs/anonfd.c`）

anonfd 是把匿名 vfile 安装到当前 fd table 的 helper。它不是文件系统，没有 path 语义。

| Op | Support | Errno | 说明 / 代码引用 |
|----|---------|-------|------------------------|
| install | Y | — | `anonfd_install_vfile`（`anonfd.c:17`）。 |
| close | Y | — | 通过 `anonfd_free_priv_close` 释放 `vf->priv`（`anonfd.c:8`）。 |

**anonfd 顺序保证**

- install 和 close 相对于调用 task 的 `fdtable` 是单线程的。

**anonfd ABI 缺口**

- 按设计没有缺口；它是内部 helper，不是 Linux ABI 表面。

## 4. VFS 横切行为

### 4.1 Permission 模型

- `vfs_vnode_permission` 使用当前 task 的 `fsuid`/`fsgid` 调用 `vfs_mode_has_perm_ids`（`vfs/stat_perm.c:72`、`vfs/stat_perm.c:201`）。
- `CAP_DAC_OVERRIDE` 绕过 read/write，但仍拒绝对没有 execute bit 的 regular file 执行（`vfs/stat_perm.c:80`）。
- 不带 `AT_EACCESS` 的 `access()` / `faccessat()` 使用 real uid/gid，且不使用 capability（`vfs_mode_has_perm_ids_nocap`，`vfs/stat_perm.c:114`）。
- `vfs_chmod_vnode` 要求所有权或 `CAP_FOWNER`（`vfs.c:575`）。
- `vfs_chown_vnode` 要求 `CAP_CHOWN` 或满足受限 ownership 规则（`vfs.c:621`）。
- sticky-bit 删除检查在 `vfs_sticky_may_remove` 中应用（`vfs/stat_perm.c:221`）。

### 4.2 Path resolution

- `vnode_lookup_path` 在 mount root 内解析绝对路径（`path_resolution.c:27`）。
- walk 期间会跟随 symlink，硬编码深度限制为 8（`path_resolution.c:89`）。
- `..` 通过跟随 `vnode->parent` 解析（`path_resolution.c:52`）。它**不会**跨越 mount point；从 mount root 向外 walk `..` 会留在 mount root 内。
- 名称长度 >= `MAX_NAME_LEN` 返回 `-ENAMETOOLONG`（`path_resolution.c:60`）。
- `vfs_resolve_no_follow_final` 解析父目录，并在不跟随最终 component 的情况下 lookup 它（`vfs.c:437`）。

### 4.3 Mount 操作

- `vfs_mount` 支持 `tmpfs`/`ramfs`、`cgroup`/`cgroup2`，以及 `fat32`/`vfat`/`ext4` 的块设备 mount（`vfs/mount_ops.c:47`）。
- 当 fstype 为空时，`vfs_mount_bc` 先尝试 `ext4`，再尝试 `vfat`（`vfs/mount_ops.c:134`）。
- `vfs_umount` 按 normalized path 匹配，并调用 `fat32_unmount` 或 `ext4_unmount`（`vfs/mount_ops.c:210`）。
- `vfs_rename` 用 `-EXDEV` 拒绝 cross-mount rename（`vfs.c:328`）。

### 4.4 Dcache

- 正向 lookup 会缓存在 `vfs_dcache_lookup`/`vfs_dcache_insert` 中（`kernel/fs/vfs/dcache.c`）。
- create、unlink、rmdir、rename、link、symlink、mount 和 umount 后会调用 `vfs_dcache_invalidate_all`。除了 rename 中的 `vfs_dcache_invalidate(old_dir, old_name)` 和 `vfs_dcache_invalidate(new_dir, new_name)`（`vfs.c:378`），没有细粒度 per-directory invalidation。

### 4.5 xattr

- xattr 存储在按 `(mnt, ino)` 索引的 1024 项全局 RAM 表 `g_xattrs` 中（`kernel/fs/xattr.c:17`）。
- 遵守 `XATTR_CREATE` 和 `XATTR_REPLACE` flag（`xattr.c:55`）。
- name 限制为 `XATTR_NAME_MAX_LOCAL`；value 限制为 `XATTR_VALUE_MAX_LOCAL`。
- 没有后端特定 xattr hook；FAT32/ext4 的值不会写入磁盘，并会在 unmount/reboot 后丢失。

## 5. 已知 Linux ABI 缺口（汇总）

| 领域 | 当前行为 | 期望的 Linux 行为 |
|------|------------------|-------------------------|
| openat2 resolve flag | 被忽略；按 `openat` 处理（`sys_proc.c:483`） | 必须遵守 `RESOLVE_*` |
| renameat2 flag | 被拒绝（`sys_path.c:33`） | 支持 `RENAME_NOREPLACE`、`RENAME_EXCHANGE`、`RENAME_WHITEOUT` |
| statx | 只有 basic stats（`sys_path.c:287`） | 遵守 `mask`、`AT_STATX_*`、`STATX_*` |
| fchmodat2 | 带 flags 分发到 `sys_fchmodat`（`syscall_table.def:74`） | 独立 syscall，完整处理 `AT_*` flag |
| xattr | 只有全局 RAM 表 | 具备 namespace 检查的后端持久化 xattr |
| Symlink loop limit | 8（`path_resolution.c:89`） | Linux 使用 40（`MAX_SYMLINKS`） |
| Mount-point `..` | 留在 mount root 内 | 跨到 parent mount |
| chroot | 设置 `root_path`，但 resolution 忽略它（`sys_namespace.c:26`） | resolution 必须受 `root_path` 约束 |
| faccessat2 flag | 支持 `AT_EACCESS`/`AT_SYMLINK_NOFOLLOW`/`AT_EMPTY_PATH` | 同时严格校验不支持的 bit |

## 6. 测试影响

- 上方矩阵中的每个 unsupported operation 都必须有测试断言精确 errno。
- Cross-mount `rename` 和 `link` 必须返回 `-EXDEV`。
- Permission 测试必须覆盖 real uid/gid（`access`）、effective uid/gid（`faccessat2(AT_EACCESS)`）、capability bypass 和 sticky-bit 目录。
- FAT32/ext4 必须有 fsync/truncate/page-cache coherence 测试。
- Pipe 测试必须验证 `PIPE_BUF` 大小写入的原子性和 `SIGPIPE` 投递。

# A20OS VFS 边界语义设计

本文档记录 A20OS 在 P1 Wave 1 收紧 VFS Linux 边界语义时的设计决策。每项决策都映射到实现文件和验证它的测试门禁。本文基于 `kernel/fs/`、`kernel/abi/linux/` 下的当前代码，以及面向用户的压力测试套件 `user/cmds/vfs_stress.c`。

## 1. 范围

覆盖的 syscall 和 VFS 边界领域：

- `openat2`：完整 Linux `resolve` flag 集合
- `renameat2` flag：`RENAME_NOREPLACE`、`RENAME_EXCHANGE`、`RENAME_WHITEOUT`
- `statx`：mask 处理和 sync type
- `faccessat2` / `fchmodat2`：`AT_*` flag 语义
- `*xattr`：namespace 校验和后端持久化
- Symlink loop limit：提高到 Linux `MAX_SYMLINKS`
- Mount-point `..` crossing：正确逃出 mount root
- `chroot`：让 path resolution 遵守 `task->fs.root_path`

本轮不包含：实际代码变更的实现（本文档是 Wave 1 契约）；运行时文件系统初始化移除（Wave 2）；page-cache 与 mmap coherence（由 MM 改进循环覆盖）。

## 2. openat2：完整 Linux resolve flag 集合

### 2.1 当前状态

`kernel/abi/linux/sys_proc.c:483` 中的 `sys_openat2` 会从用户 `struct open_how` 复制 `how->flags` 和 `how->mode`，但忽略 `how->resolve` 和 `how->size`。随后它调用 `sys_openat`，因此 `RESOLVE_*` flag 没有任何效果。

### 2.2 决策

实现完整 Linux `openat2` resolve flag 集合：

| Flag | 语义 | 实现文件 |
|------|-----------|---------------------|
| `RESOLVE_NO_SYMLINKS` | 整个 walk 期间绝不跟随 symlink | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_BENEATH` | 拒绝 walk 到起始目录之上 | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_IN_ROOT` | 相对于起始目录解析绝对路径和 `..` | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_NO_MAGICLINKS` | 拒绝 procfs/sysfs 风格 magic link | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_NO_XDEV` | 拒绝跨越 mount point | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_CACHED` | 只使用缓存 lookup；dentry 未缓存则失败 | **范围外**，以 `-EINVAL` 拒绝 |
| `RESOLVE_NO_TRAILING_SYMLINKS` | 不跟随最终 component 处的 symlink | 在 `vfs_open` / `vfs_fstatat` 中处理 |

### 2.3 实现映射

- `kernel/abi/linux/sys_proc.c`：对 `sizeof(struct open_how)` 校验 `how->size`；用 `-EINVAL` 拒绝未知 `resolve` bit；将 resolve flag 传给 `vfs_openat2`。
- `kernel/fs/vfs.c` / 新 `kernel/fs/vfs/open.c`：增加 `vfs_openat2(dirfd, path, flags, mode, resolve)`，构造 resolution context 并调用 resolver。
- `kernel/fs/vfs/path_resolution.c`：扩展 `vnode_lookup_path`，使其接受 `resolve_flags` bitmask 和起始目录。增加 helper `vfs_path_walk_beneath`。
- `kernel/fs/vfs/dcache.c`：~~增加 `vfs_dcache_lookup_cached`~~；由于当前 dentry cache 是优化层而不是权威 lookup 来源，`RESOLVE_CACHED` 以 `-EINVAL` 拒绝。
- `kernel/include/fs/vfs.h`：增加 `RESOLVE_*` 常量和 `open_how` struct layout。
- `kernel/include/abi/linux/fcntl.h`：在 ABI 边界镜像 Linux `RESOLVE_*` 值。
- `user/cmds/vfs_stress.c`：增加 `openat2` resolve flag 测试。

### 2.4 ABI 说明

- Linux `struct open_how` 是 `{ u64 flags; u64 mode; u64 resolve; u64 __padding[8]; }`。
- A20OS 必须接受相同布局；`how->size` 必须至少 24 字节，且不超过 `sizeof(struct open_how)`。
- 未知 `resolve` bit 返回 `-EINVAL`。

## 3. renameat2 flag

### 3.1 当前状态

`kernel/abi/linux/sys_path.c:31` 中的 `sys_renameat2` 会用 `-EINVAL` 拒绝任何非零 `flags`，并调用 `vfs_rename` 执行不带 flag 的普通 rename。

### 3.2 决策

实现 `RENAME_NOREPLACE`、`RENAME_EXCHANGE`，并记录 `RENAME_WHITEOUT` 本轮范围外，因为 A20OS 没有 overlay/whiteout 文件系统支持。

| Flag | 支持 | 语义 | 实现文件 |
|------|---------|-----------|---------------------|
| `RENAME_NOREPLACE` | 实现 | 如果目标已存在，以 `-EEXIST` 失败 | `kernel/fs/vfs.c`、后端 rename ops |
| `RENAME_EXCHANGE` | 实现 | 原子交换源和目标 | `kernel/fs/vfs.c`、后端 rename ops |
| `RENAME_WHITEOUT` | 范围外 | 需要 overlayfs 语义 | 记录为不支持；返回 `-EINVAL` |

### 3.3 实现映射

- `kernel/abi/linux/sys_path.c`：校验 flag；允许 `RENAME_NOREPLACE | RENAME_EXCHANGE`；拒绝 `RENAME_WHITEOUT` 和不兼容组合。
- `kernel/fs/vfs.c`：为 `vfs_rename` 增加 `flags` 参数。在委托给 `old_dir->ops->rename` 前检查 cross-mount（`-EXDEV`）、same target（`-EINVAL`）和 permission/sticky-bit 规则。
- `kernel/fs/ramfs.c`：在 `ramfs_vnode_rename` 中实现 `RENAME_NOREPLACE`（现有 lookup）和 `RENAME_EXCHANGE`。
- `kernel/fs/ext4.c`：扩展 `ext4_vn_rename`，处理 `RENAME_NOREPLACE` 和 `RENAME_EXCHANGE`。
- `kernel/fs/fat32.c`：保持 `.rename = NULL`；VFS 会继续对 FAT32 返回 `-ENOSYS`。
- 后端 vnode ops 签名变更：`int (*rename)(vnode_t *old_dir, const char *old_name, vnode_t *new_dir, const char *new_name, unsigned int flags)`。
- `kernel/include/fs/vfs.h`：更新 `rename` 函数指针类型并增加 `RENAME_*` 常量。
- `user/cmds/vfs_stress.c`：增加 renameat2 flag 测试。

## 4. statx

### 4.1 当前状态

`kernel/abi/linux/sys_path.c:287` 中的 `sys_statx` 校验部分 flag，默认 `mask` 为 `STATX_BASIC_STATS`，并填充固定字段集合。它不遵守 `AT_STATX_FORCE_SYNC` / `AT_STATX_DONT_SYNC`，不报告 `STATX_BTIME`，也忽略大部分请求的 `mask`。

### 4.2 决策

- 遵守 `mask`：只填充调用者请求的字段；将 `stx_mask` 设为实际提供的字段。
- 遵守 `AT_STATX_SYNC_TYPE`：`FORCE_SYNC` 刷新 page-cache writeback 并重新读取 inode metadata；`DONT_SYNC` 使用缓存值。
- 将 `STATX_BTIME` 报告为与 `ctime` 相同（A20OS 不跟踪 birth time）。
- 用 `-EINVAL` 拒绝未知 flag。

### 4.3 实现映射

- `kernel/abi/linux/sys_path.c`：将 `mask` 和 sync type 传给 `vfs_statx`。
- `kernel/fs/vfs.c`：增加 `vfs_statx(dirfd, path, flags, mask, buf)`。
- `kernel/fs/vfs/stat_perm.c`：扩展 `vfs_vnode_stat` 以接受 sync hint，或增加 `vfs_vnode_statx`。
- `kernel/fs/fat32.c` / `kernel/fs/ext4.c` / `kernel/fs/ramfs.c`：后端 `stat` ops 已提供所有基础字段；增加 btime 报告。
- `kernel/fs/page_cache.c`：为 `FORCE_SYNC` 提供 `page_cache_writeback_vnode` 调用。
- `kernel/include/abi/linux/stat.h`：已经定义 `STATX_*` 和 `AT_STATX_*`。
- `user/cmds/vfs_stress.c`：增加 statx mask 和 sync-type 测试。

## 5. faccessat2 / fchmodat2

### 5.1 当前状态

- `kernel/fs/vfs.c:527` 中的 `vfs_faccessat2` 处理 `AT_EACCESS`、`AT_SYMLINK_NOFOLLOW` 和 `AT_EMPTY_PATH`。
- `kernel/abi/linux/sys_path.c:232` 中的 `sys_fchmodat` 处理 `AT_SYMLINK_NOFOLLOW` 和 `AT_EMPTY_PATH`，但在 legacy `fchmodat` 路径中忽略第四个参数。
- `fchmodat2` 在 `kernel/abi/linux/syscall_table.def:74` 中连到带 flags 参数的 `sys_fchmodat`。

### 5.2 决策

- `faccessat2`：保留当前行为；增加严格校验，确保没有设置 `AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH` 之外的 bit。
- `fchmodat2`：作为一等 syscall 处理，支持完整 flag（`AT_SYMLINK_NOFOLLOW`、`AT_EMPTY_PATH`）。用 `-EINVAL` 拒绝不支持的 bit。

### 5.3 实现映射

- `kernel/fs/vfs.c`：收紧 `vfs_faccessat2` flag 校验。
- `kernel/fs/vfs.c`：`vfs_chmodat` 已检查 flag；确保 `vfs_fchmodat2`（或带 flags 的 `fchmodat` 路径）从 syscall table 暴露。
- `kernel/abi/linux/sys_path.c`：让 `sys_fchmodat` 成为规范实现，并确保 `fchmodat2` 共享同一路径。
- `user/cmds/vfs_stress.c`：增加 faccessat2 flag 和 fchmodat2 测试。

## 6. xattr

### 6.1 当前状态

- xattr syscall 位于 `kernel/abi/linux/sys_xattr.c`。
- 存储是 `kernel/fs/xattr.c` 中按 `(mnt, ino)` 索引的全局 RAM 表。
- 没有后端特定 xattr hook；FAT32/ext4 不持久化值。
- 未执行 namespace 校验（例如 `security.*`、`trusted.*`、`user.*`）。

### 6.2 决策

P1 保留全局 RAM xattr 表，但收紧 ABI 表面：

- 用 `-EINVAL` 拒绝没有 namespace 前缀（`user.`、`trusted.`、`security.`、`system.`）的名称。
- 如果调用者缺少 `CAP_SYS_ADMIN`，拒绝 `security.*` 和 `trusted.*`。
- 用 `-EOPNOTSUPP` 拒绝非 reg/dir/lnk vnode 上的 xattr（`sys_xattr.c:35` 已这样做）。
- 保持值只存在于 RAM；记录块文件系统的持久化缺口。

### 6.3 实现映射

- `kernel/fs/xattr.c`：增加 `xattr_check_namespace` helper。
- `kernel/abi/linux/sys_xattr.c`：存储前调用 namespace 检查。
- `kernel/include/fs/xattr.h`：如未存在，定义 namespace 常量。
- `kernel/fs/fat32.c` / `kernel/fs/ext4.c`：记录本轮不增加后端 xattr hook。
- `user/cmds/vfs_stress.c`：增加 xattr namespace 和 permission 测试。

## 7. Symlink loop limit

### 7.1 当前状态

`vnode_lookup_path` 递增 `symlink_depth`，当深度超过 8 时以 `-ELOOP` 失败（`kernel/fs/vfs/path_resolution.c:89`）。

### 7.2 决策

将限制提高到 Linux `MAX_SYMLINKS`（40）。计数器仍保留在 resolver context 中。

### 7.3 实现映射

- `kernel/fs/vfs/path_resolution.c`：用 `MAX_SYMLINKS` 替换硬编码 `8`。
- `kernel/include/fs/vfs.h`：增加 `#define MAX_SYMLINKS 40`。
- `user/cmds/vfs_stress.c`：增加 40-link chain 测试和 41-link `-ELOOP` 测试。

## 8. Mount-point `..` crossing

### 8.1 当前状态

`vnode_lookup_path` 通过跟随 `vnode->parent` 处理 `..`（`kernel/fs/vfs/path_resolution.c:52`）。如果 walk 位于 mount root，`vnode->parent` 要么指回自身（`fat32.c:1213`、`ext4.c:1536`），要么指向被挂载文件系统内部的 parent（`ramfs.c:165`、`ramfs.c:266`），因此 `..` 无法逃出 mount。

### 8.2 决策

实现 Linux mount-point `..` 语义：

- 在 mount root 处解析 `..` 时，切换到底层文件系统中 mount point 的父目录。
- 与 `openat2` 一起使用时，`RESOLVE_NO_XDEV` 必须阻止这种跨越。
- `chroot` 与 mount crossing 组合时必须同时遵守两种边界。

### 8.3 实现映射

- `kernel/fs/vfs/mount.c`：增加 `vfs_mount_parent(mount_t *mnt)`，返回覆盖 `mnt->path` 父路径的 mount。
- `kernel/fs/vfs/path_resolution.c`：在 `vnode_lookup_path` 中跟随 `vnode->parent` 前，先检查 `cur` 是否是 mount root；如果是，切换到 parent mount 的 vnode 并继续。
- `kernel/fs/vfs.h`：增加 mount-root 检测 helper。
- `user/cmds/vfs_stress.c`：增加从 `/bin`（FAT32 mount）通过 `..` 回到 `/` 的测试。

## 9. chroot

### 9.1 当前状态

`kernel/abi/linux/sys_namespace.c:26` 中的 `sys_chroot` 会校验目标为目录、检查 `CAP_SYS_CHROOT`、把路径存入 `cur->fs.root_path`，并把 `cwd` 重置为 `/`。但是 `vfs_resolve_at` 和 `vnode_lookup_path` 不读取 `root_path`，所以 `chroot` 对后续 path resolution 没有效果。

### 9.2 决策

让 path resolution 遵守 `task->fs.root_path`：

- 绝对路径相对于 `root_path` 解释。
- `..` 解析不得逃出 `root_path`。
- chrooted 进程中的 `openat(fd, "../../..", ...)` 必须留在 chroot 内部。
- 绝对路径 `execve` 也必须使用 `root_path`。

### 9.3 实现映射

- `kernel/fs/vfs/path_resolution.c`：增加 `vfs_resolve_rooted`，在 normalization 前将 `root_path` 与请求路径拼接。
- `kernel/fs/vfs.c`：让 `vfs_resolve`、`vfs_resolve_at` 和 `vfs_resolve_no_follow_final` 使用当前 task 的 `root_path`。
- `kernel/proc/exec.c`：确保 `proc_exec` 通过同一个 rooted resolver 解析可执行文件路径。
- `kernel/abi/linux/sys_namespace.c`：保留现有 `sys_chroot`；该处无需改动。
- `user/cmds/vfs_stress.c`：增加 chroot escape 测试。

## 10. 变更 / 扩展文件汇总

| 文件 | 变更 |
|------|--------|
| `kernel/abi/linux/sys_proc.c` | `openat2` 校验与分发 |
| `kernel/abi/linux/sys_path.c` | `renameat2` flag、`statx` sync/mask、`fchmodat2` flag |
| `kernel/abi/linux/sys_xattr.c` | namespace 和 capability 检查 |
| `kernel/fs/vfs.c` | `vfs_rename` flag、`vfs_faccessat2` 校验、rooted resolution hook |
| `kernel/fs/vfs/path_resolution.c` | `RESOLVE_*`、`MAX_SYMLINKS`、mount `..` crossing、chroot root |
| `kernel/fs/vfs/mount.c` | mount-parent lookup helper |
| `kernel/fs/vfs/dcache.c` | `RESOLVE_CACHED` lookup helper |
| `kernel/fs/vfs/stat_perm.c` | `vfs_vnode_statx` / sync hint |
| `kernel/fs/xattr.c` | namespace validation helper |
| `kernel/fs/ramfs.c` | `RENAME_NOREPLACE` / `RENAME_EXCHANGE` |
| `kernel/fs/ext4.c` | `RENAME_NOREPLACE` / `RENAME_EXCHANGE` |
| `kernel/fs/fat32.c` | 无 rename 变更（仍不支持） |
| `kernel/include/fs/vfs.h` | 新常量和 vnode ops 签名 |
| `kernel/include/abi/linux/fcntl.h` | `RESOLVE_*` 和 `RENAME_*` ABI 值 |
| `kernel/include/abi/linux/stat.h` | 已定义 `STATX_*` |
| `kernel/proc/exec.c` | executable 的 rooted path resolution |
| `user/cmds/vfs_stress.c` | 新边界场景测试 |

## 11. 测试门禁

必须增加或扩展以下 smoke / stress 门禁：

- `smoke-vfs-edge`：openat2 resolve flag、renameat2 flag、statx mask/sync、faccessat2/fchmodat2 flag、xattr namespace、symlink loop limit、mount `..` crossing、chroot。
- 现有门禁（`smoke-abi-linux`、`check-vfs-abstraction`）必须保持通过。

`smoke-vfs-fs-specific`（每后端 unsupported-op errno 矩阵）不属于 P1 范围；后端能力差异继续记录在 `docs/fs/fs-consistency-model.md`。

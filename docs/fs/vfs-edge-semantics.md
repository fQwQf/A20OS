# A20OS VFS 边界语义设计

本文档记录 A20OS 在 P1 Wave 1 收紧 VFS Linux 边界语义时的设计决策，并区分当前实现与未完成项。本文已按 2026-08 的 `kernel/fs/`、`kernel/abi/linux/` 和 `user/cmds/stress/vfs_edge.c` 做源码核对；运行结论需在当前提交上复验，历史平台记录不能外推。

> **实现状态**：核心路径已有实现，但不是完整 Linux 兼容。`RESOLVE_NO_MAGICLINKS` 已在 resolver 中执行检查（`/proc/<pid>/fd/N` 类 jump link 拒绝穿越，返回 `-ELOOP`），`RESOLVE_CACHED` 通过 dentry cache 命中检查实现（未命中返回 `-EAGAIN`），A20OS 自定义 `RESOLVE_NO_TRAILING_SYMLINKS` 已移除、不再与 Linux 的 bit 分配冲突，`renameat2` 能力取决于具体后端。`smoke-vfs-edge` 当前只在 RISC-V64、1 vCPU 上运行，覆盖选定场景，不是全 flag、全后端或双架构证明。

## 1. 范围

覆盖的 syscall 和 VFS 边界领域：

- `openat2`：已实现的 `resolve` 子集及 A20OS 扩展
- `renameat2` flag：`RENAME_NOREPLACE`、`RENAME_EXCHANGE`、`RENAME_WHITEOUT`
- `statx`：mask 处理和 sync type
- `faccessat2` / `fchmodat2`：`AT_*` flag 语义
- `*xattr`：namespace 校验和后端持久化
- Symlink loop limit：提高到 Linux `MAX_SYMLINKS`
- Mount-point `..` crossing：正确逃出 mount root
- `chroot`：让 path resolution 遵守 `task->fs.root_path`

本文覆盖当前边界实现及其遗留差距；不覆盖运行时文件系统初始化移除或 page-cache/mmap coherence。

## 2. openat2：当前 resolve 子集

### 2.1 当前状态（设计时）

`kernel/abi/linux/sys_proc.c` 的 `sys_openat2` 曾从用户 `struct open_how` 复制 `flags` 和 `mode`，但忽略 `resolve` 和 syscall 的 `size` 参数，随后调用 `sys_openat`，因此 `RESOLVE_*` flag 没有任何效果。**现已实现**：`size` 被校验、未知 `resolve` bit 以 `-EINVAL` 拒绝，全部六个 `RESOLVE_*` flag（含 `NO_MAGICLINKS`）经 `vfs_openat2` 在 resolver 中生效（`kernel/abi/linux/sys_proc.c`、`kernel/fs/vfs.c`、`kernel/fs/vfs/path_resolution.c`）。

### 2.2 决策

当前实现的 flag 表如下；其中最后两项不是 Linux-compatible 编号：

| Flag | 语义 | 实现文件 |
|------|-----------|---------------------|
| `RESOLVE_NO_SYMLINKS` | 整个 walk 期间绝不跟随 symlink | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_BENEATH` | 拒绝 walk 到起始目录之上 | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_IN_ROOT` | 相对于起始目录解析绝对路径和 `..` | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_NO_MAGICLINKS` | 拒绝穿越 magic link（`/proc/<pid>/fd/N` 类 jump link），普通 symlink 仍可跟随；与 `NO_SYMLINKS` 同返回 `-ELOOP` | `kernel/fs/vfs/path_resolution.c`；magic link 由 vnode `VNODE_MAGICLINK` 标记（procfs fd-symlink 创建时设置） |
| `RESOLVE_NO_XDEV` | 拒绝跨越 mount point | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_CACHED` | 只使用缓存 lookup；任意组件未缓存则返回 `-EAGAIN` | `kernel/fs/vfs/path_resolution.c`；dentry cache 是优化层，未缓存路径由调用方去 flag 重试 |

### 2.3 实现映射

- `kernel/abi/linux/sys_proc.c`：接受 24 到 A20OS `sizeof(struct open_how)`（88）字节；用 `-EINVAL` 拒绝未知 resolve bit；将 resolve flag 传给 `vfs_openat2`。函数中检查超长结构尾部是否为零的分支当前因前置 `size > sizeof(khow)` 拒绝而不可达。
- `kernel/fs/vfs.c`：`vfs_openat2(dirfd, path, flags, mode, resolve)` 构造 resolution context 并调用 resolver。
- `kernel/fs/vfs/path_resolution.c`：让 openat2 resolver 接受起始路径、root path 和 `resolve_flags`，并在 walk 中执行 beneath/root/mount 边界检查；`RESOLVE_CACHED` 在 dentry cache 未命中时返回 `-EAGAIN`（不回落真实 lookup）。
- `kernel/fs/vfs/dcache.c`：`RESOLVE_CACHED` 通过 `vfs_dcache_lookup` 命中检查实现；dentry cache 仍是优化层而非权威 lookup 来源，未缓存路径返回 `-EAGAIN` 由调用方重试。
- `kernel/include/fs/vfs.h`：增加 `RESOLVE_*` 常量和 `open_how` struct layout。
- `kernel/include/core/fcntl.h`：当前常量表与 Linux 的 bit 分配一致（`RESOLVE_CACHED == 0x20`）。
- `user/cmds/stress/vfs_edge.c`：覆盖 `BENEATH`、`NO_SYMLINKS` 和 `CACHED` 的选定场景。

### 2.4 ABI 说明

- Linux 当前已知 `struct open_how` 前缀是 24 字节的 `{ u64 flags; u64 mode; u64 resolve; }`，并通过 syscall 的 `size` 参数做尾部扩展；A20OS 内部结构额外声明了 64 字节 padding。
- A20OS 当前要求 `24 <= size <= 88`，并不接受 Linux 规则允许的更长全零扩展结构。
- `RESOLVE_CACHED` 与 Linux 一致为 `0x20`；A20OS 自定义的 `RESOLVE_NO_TRAILING_SYMLINKS`（曾占用 `0x20`）已移除，不再与 Linux 冲突。
- 未知 resolve bit 返回 `-EINVAL`；六个已知 flag 全部在 resolver 中有对应检查。

## 3. renameat2 flag

### 3.1 当前状态（设计时）

`kernel/abi/linux/sys_path.c` 的 `sys_renameat2` 曾用 `-EINVAL` 拒绝任何非零 `flags`。**现已实现**：`RENAME_NOREPLACE` 与 `RENAME_EXCHANGE` 被接受并经 `vfs_rename_flags` 分发到后端，`RENAME_WHITEOUT` 仍以 `-EINVAL` 拒绝。普通 same-path rename 由后端作为 no-op 成功；VFS 没有 same-path `-EINVAL` guard。

### 3.2 决策

实现 `RENAME_NOREPLACE`、`RENAME_EXCHANGE`，并记录 `RENAME_WHITEOUT` 本轮范围外，因为 A20OS 没有 overlay/whiteout 文件系统支持。

不带冲突 flag 的 source 与 target 为同一路径时返回成功。`RENAME_NOREPLACE` 仍按“目标已存在”规则处理，不能把普通 same-path no-op 扩展成所有 flag 组合都成功。

| Flag | 支持 | 语义 | 实现文件 |
|------|---------|-----------|---------------------|
| `RENAME_NOREPLACE` | 实现 | 如果目标已存在，以 `-EEXIST` 失败 | `kernel/fs/vfs.c`、后端 rename ops |
| `RENAME_EXCHANGE` | 实现 | 原子交换源和目标 | `kernel/fs/vfs.c`、后端 rename ops |
| `RENAME_WHITEOUT` | 范围外 | 需要 overlayfs 语义 | 记录为不支持；返回 `-EINVAL` |

### 3.3 实现映射

- `kernel/abi/linux/sys_path.c`：校验 flag；允许 `RENAME_NOREPLACE | RENAME_EXCHANGE`；拒绝 `RENAME_WHITEOUT` 和不兼容组合。
- `kernel/fs/vfs.c`：由 `vfs_rename_flags` 校验并传递 flags；在委托给 `old_dir->ops->rename` 前检查 cross-mount（`-EXDEV`）和 permission/sticky-bit 规则。`vfs_rename` 保留为 flags 为 0 的 wrapper；没有 same-target `-EINVAL` 检查，普通 same-path no-op 由后端返回成功。
- `kernel/fs/diskfs/ramfs.c`：在 `ramfs_vnode_rename` 中实现 `RENAME_NOREPLACE`（现有 lookup）和 `RENAME_EXCHANGE`。
- `kernel/fs/diskfs/ext4.c`：扩展 `ext4_vn_rename`，处理 `RENAME_NOREPLACE` 和 `RENAME_EXCHANGE`。
- `kernel/fs/diskfs/fat32.c` / `fat32_vn.c`：已接入 rename，支持普通替换、跨目录移动与 `RENAME_NOREPLACE`；不支持 `RENAME_EXCHANGE`。
- 后端 vnode ops 签名变更：`int (*rename)(vnode_t *old_dir, const char *old_name, vnode_t *new_dir, const char *new_name, unsigned int flags)`。
- `kernel/include/fs/vfs.h`：更新 `rename` 函数指针类型并增加 `RENAME_*` 常量。
- `user/cmds/stress/vfs_edge.c`：当前在 ramfs 上覆盖 `NOREPLACE` 与 `EXCHANGE`。

## 4. statx

### 4.1 当前状态（设计时）

`kernel/abi/linux/sys_path.c` 的 `sys_statx` 曾默认 `mask` 为 `STATX_BASIC_STATS` 并填充固定字段集合，不遵守 sync type、不报告 `STATX_BTIME`、忽略大部分请求的 `mask`。当前实现不是 requested-only：它总把 `STATX_BASIC_STATS` 加入 `stx_mask` 并返回基础元数据，再按请求追加 `STATX_BTIME` 等可选字段；`STATX_BTIME` 的值与 `ctime` 相同。

### 4.2 决策

- `mask` 采用“基础字段始终提供、可选字段按请求追加”的策略：`stx_mask` 总含 `STATX_BASIC_STATS`，而不是只回报调用者请求的 bit。
- 遵守 `AT_STATX_SYNC_TYPE`：`FORCE_SYNC` 先执行 vnode page-cache writeback 再读取 metadata；`DONT_SYNC` 不触发该 writeback。
- 将 `STATX_BTIME` 报告为与 `ctime` 相同（A20OS 不跟踪 birth time）。
- 用 `-EINVAL` 拒绝未知 flag。

### 4.3 实现映射

- `kernel/abi/linux/sys_path.c`：将 `mask` 和 sync type 传给 `vfs_statx`。
- `kernel/fs/vfs_stat.c`：`vfs_statx`/`vfs_fstatx` 处理 sync hint，并复用 `vfs_vnode_stat` 读取 metadata。
- `kernel/fs/vfs/stat_perm.c`：`vfs_vnode_stat` 提供基础 stat 与 RAM time-meta 回退。
- `kernel/abi/linux/sys_path.c`：组装 `struct statx`，总是报告基础字段，并在请求 `STATX_BTIME` 时复制 `ctime`。
- `kernel/fs/page_cache.c`：为 `FORCE_SYNC` 提供 `page_cache_writeback_vnode` 调用。
- `kernel/include/abi/linux/stat.h`：已经定义 `STATX_*` 和 `AT_STATX_*`。
- `user/cmds/stress/vfs_edge.c`：覆盖 `FORCE_SYNC`/`DONT_SYNC` 与 size mask。

## 5. faccessat2 / fchmodat2

### 5.1 当前状态（设计时）

- `kernel/fs/vfs_stat.c` 中的 `vfs_faccessat2` 处理 `AT_EACCESS`、`AT_SYMLINK_NOFOLLOW` 和 `AT_EMPTY_PATH`，并对未支持 bit 返回 `-EINVAL`。
- `kernel/abi/linux/sys_path.c` 中的 `sys_fchmodat` 处理 `AT_SYMLINK_NOFOLLOW` 和 `AT_EMPTY_PATH`，但在 legacy `fchmodat` 路径中忽略第四个参数。
- `fchmodat2` 在 `kernel/abi/linux/syscall_table.def` 中连到带 flags 参数的 `sys_fchmodat`。

### 5.2 决策

- `faccessat2`：保留当前行为；增加严格校验，确保没有设置 `AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH` 之外的 bit。
- `fchmodat2`：作为一等 syscall 处理，支持完整 flag（`AT_SYMLINK_NOFOLLOW`、`AT_EMPTY_PATH`）。用 `-EINVAL` 拒绝不支持的 bit。

### 5.3 实现映射

- `kernel/fs/vfs_stat.c`：收紧 `vfs_faccessat2` flag 校验。
- `kernel/fs/vfs_stat.c`：`vfs_chmodat` 已检查 flag；确保带 flags 的 `fchmodat` 路径从 syscall table 暴露。
- `kernel/abi/linux/sys_path.c`：让 `sys_fchmodat` 成为规范实现，并确保 `fchmodat2` 共享同一路径。
- 当前 `vfs_edge.c` 没有专门的 faccessat2/fchmodat2 flag 场景；这是测试缺口。

## 6. xattr

### 6.1 当前状态

- xattr syscall 位于 `kernel/abi/linux/sys_xattr.c`。
- 存储是 `kernel/fs/xattr.c` 中按 `(mnt, ino)` 索引的全局 RAM 表。
- 没有后端特定 xattr hook；FAT32/ext4 不持久化值。
- `xattr_check_namespace()` 只接受 `user.`、`trusted.`、`security.`、`system.`；`trusted.`/`security.` 的 set/remove 要求 `CAP_SYS_ADMIN`。
- syscall 表面只接受 reg/dir/lnk vnode，其他类型返回 `-EOPNOTSUPP`。

### 6.2 已落地决策

P1 保留全局 RAM xattr 表，并收紧 ABI 表面：

- 用 `-EINVAL` 拒绝没有 namespace 前缀（`user.`、`trusted.`、`security.`、`system.`）的名称。
- 如果调用者缺少 `CAP_SYS_ADMIN`，拒绝 `security.*` 和 `trusted.*`。
- 用 `-EOPNOTSUPP` 拒绝非 reg/dir/lnk vnode 上的 xattr（`kernel/abi/linux/sys_xattr.c` 已这样做）。
- 保持值只存在于 RAM；记录块文件系统的持久化缺口。

### 6.3 实现映射

- `kernel/fs/xattr.c`：`xattr_check_namespace` helper 与 capability 检查。
- `kernel/abi/linux/sys_xattr.c`：通过 vnode xattr helper 执行 namespace 检查。
- `kernel/include/fs/xattr.h`：namespace 常量。
- `kernel/fs/diskfs/fat32.c` / `kernel/fs/diskfs/ext4.c`：记录本轮不增加后端 xattr hook。
- `user/cmds/stress/vfs_edge.c`：覆盖 user/invalid/trusted namespace 的选定行为。

## 7. Symlink loop limit

### 7.1 当前状态（设计时）

`vnode_lookup_path` 曾把 `symlink_depth` 上限硬编码为 8（`kernel/fs/vfs/path_resolution.c`）。**现已实现**：使用 `MAX_SYMLINKS`（40，`kernel/include/fs/vfs.h`），深度超过时以 `-ELOOP` 失败。

### 7.2 决策

将限制提高到 Linux `MAX_SYMLINKS`（40）。计数器仍保留在 resolver context 中。

### 7.3 实现映射

- `kernel/fs/vfs/path_resolution.c`：用 `MAX_SYMLINKS` 替换硬编码 `8`。
- `kernel/include/fs/vfs.h`：增加 `#define MAX_SYMLINKS 40`。
- `user/cmds/stress/vfs_edge.c`：构造超过 40 层的 symlink chain 并检查 `ELOOP`。

## 8. Mount-point `..` crossing

### 8.1 当前状态（设计时）

`vnode_lookup_path` 通过跟随 `vnode->parent` 处理 `..`。此前 mount root 的 `vnode->parent` 要么指回自身，要么指向被挂载文件系统内部的 parent，因此 `..` 无法逃出 mount。**现已实现**：在 mount root 处解析 `..` 时切换到 mount point 的父目录（mount crossing），`RESOLVE_NO_XDEV` 阻止这种跨越。

### 8.2 决策

实现 Linux mount-point `..` 语义：

- 在 mount root 处解析 `..` 时，切换到底层文件系统中 mount point 的父目录。
- 与 `openat2` 一起使用时，`RESOLVE_NO_XDEV` 必须阻止这种跨越。
- `chroot` 与 mount crossing 组合时必须同时遵守两种边界。

### 8.3 实现映射

- `kernel/fs/vfs/mount.c`：增加 `vfs_mount_parent(mount_t *mnt)`，返回覆盖 `mnt->path` 父路径的 mount。
- `kernel/fs/vfs/path_resolution.c`：在 `vnode_lookup_path` 中跟随 `vnode->parent` 前，先检查 `cur` 是否是 mount root；如果是，切换到 parent mount 的 vnode 并继续。
- `kernel/include/fs/vfs.h`：增加 mount-root 检测 helper。
- `user/cmds/stress/vfs_edge.c`：在 `/tmp` 挂载 ramfs，检查从 mount root 的 `..` 回到父 mount；当前没有 FAT32 嵌套 mount 专项。

## 9. chroot

### 9.1 当前状态（设计时）

`kernel/abi/linux/sys_namespace.c` 的 `sys_chroot` 曾校验目标为目录、检查 `CAP_SYS_CHROOT`、把路径存入 `cur->fs.root_path` 并重置 cwd，但 resolver 不读取 `root_path`。**现已实现**：`vfs_path_normalize_absolute_with_root` 在路径解析中约束 `root_path`（`kernel/fs/vfs/path_resolution.c`），chrooted 进程的绝对路径与 `..` 都无法逃出 chroot。

### 9.2 决策

让 path resolution 遵守 `task->fs.root_path`：

- 绝对路径相对于 `root_path` 解释。
- `..` 解析不得逃出 `root_path`。
- chrooted 进程中的 `openat(fd, "../../..", ...)` 必须留在 chroot 内部。
- 绝对路径 `execve` 也必须使用 `root_path`。

### 9.3 实现映射

- `kernel/fs/vfs/path_resolution.c`：通过 `vfs_path_normalize_absolute_with_root` 和 resolver 的 root context 约束绝对路径与 `..`。
- `kernel/fs/vfs/path_resolution.c`：让 `vfs_resolve` 和 `vfs_resolve_at` 使用当前 task 的 `root_path`；`kernel/fs/vfs.c` 中的 `vfs_resolve_no_follow_final` 使用相同 root 约束。
- `kernel/proc/exec.c`：`proc_exec` 在构造绝对 executable path 时应用 `root_path`，再调用 `vfs_path_normalize_absolute_with_root`。
- `kernel/abi/linux/sys_namespace.c`：保留现有 `sys_chroot`；该处无需改动。
- `user/cmds/stress/vfs_edge.c`：覆盖 chroot 后通过 `..` 访问外部 sibling 的拒绝行为。

## 10. 变更 / 扩展文件汇总

| 文件 | 变更 |
|------|--------|
| `kernel/abi/linux/sys_proc.c` | `openat2` 校验与分发 |
| `kernel/abi/linux/sys_path.c` | `renameat2` flag、`statx` sync/mask、`fchmodat2` flag |
| `kernel/abi/linux/sys_xattr.c` | namespace 和 capability 检查 |
| `kernel/fs/vfs.c` | `vfs_rename_flags`、rooted resolution hook |
| `kernel/fs/vfs_stat.c` | `vfs_faccessat2` 与 `vfs_chmodat` flag 校验 |
| `kernel/fs/vfs/path_resolution.c` | `RESOLVE_*`、`MAX_SYMLINKS`、mount `..` crossing、chroot root |
| `kernel/fs/vfs/mount.c` | mount-parent lookup helper |
| `kernel/fs/vfs/dcache.c` | 普通 lookup 优化缓存；`RESOLVE_CACHED` 通过 dcache 命中检查实现（未命中 `-EAGAIN`） |
| `kernel/fs/vfs/stat_perm.c` | `vfs_vnode_stat`、permission 与 RAM time-meta |
| `kernel/fs/xattr.c` | namespace validation helper |
| `kernel/fs/diskfs/ramfs.c` | `RENAME_NOREPLACE` / `RENAME_EXCHANGE` |
| `kernel/fs/diskfs/ext4.c` | `RENAME_NOREPLACE` / `RENAME_EXCHANGE` |
| `kernel/fs/diskfs/fat32.c`、`fat32_vn.c` | 普通 rename 与 `RENAME_NOREPLACE`；不支持 exchange |
| `kernel/include/fs/vfs.h` | 新常量和 vnode ops 签名 |
| `kernel/include/abi/linux/fcntl.h` | `RESOLVE_*` 和 `RENAME_*` ABI 值 |
| `kernel/include/abi/linux/stat.h` | 已定义 `STATX_*` |
| `kernel/proc/exec.c` | executable 的 rooted path resolution |
| `user/cmds/stress/vfs_edge.c` | 当前 RISC-V64 edge 场景 |
| `user/cmds/stress/vfs_stress.c` | 较广 VFS 压力与基础行为场景 |

## 11. 测试门禁

必须增加或扩展以下 smoke / stress 门禁：

- `smoke-vfs-edge`：当前 RISC-V64 单核目标，覆盖 openat2 的 `BENEATH`/`NO_SYMLINKS`/`NO_MAGICLINKS`/`CACHED`、ramfs renameat2、statx、xattr、symlink loop、mount `..` crossing、chroot 等选定场景；不覆盖全部 resolve flag 组合、全部文件系统后端或其他架构。
- 现有门禁（`smoke-abi-linux`、`check-vfs-abstraction`）必须保持通过。

`smoke-vfs-fs-specific`（每后端 unsupported-op errno 矩阵）不属于 P1 范围；后端能力差异继续记录在 `docs/fs/fs-consistency-model.md`。

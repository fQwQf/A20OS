# A20OS VFS Edge Semantics Design

This document records the design decisions for tightening A20OS VFS Linux edge
semantics during P1 Wave 1. Each decision is mapped to the files that will
implement it and the test gates that will verify it. It is grounded in the
current code under `kernel/fs/`, `kernel/abi/linux/`, and the user-facing stress
suite `user/cmds/vfs_stress.c`.

## 1. Scope

Covered syscall and VFS edge areas:

- `openat2` — full Linux `resolve` flag set
- `renameat2` flags — `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, `RENAME_WHITEOUT`
- `statx` — mask handling and sync type
- `faccessat2` / `fchmodat2` — `AT_*` flag semantics
- `*xattr` — namespace validation and backend persistence
- Symlink loop limit — raise to Linux `MAX_SYMLINKS`
- Mount-point `..` crossing — escape mount root correctly
- `chroot` — make path resolution respect `task->fs.root_path`

Excluded from this wave: implementation of actual code changes (this document
is the Wave 1 contract); runtime filesystem initialization removal (Wave 2);
page-cache and mmap coherence (covered by the MM improvement loop).

## 2. openat2 — Full Linux resolve-flag set

### 2.1 Current state

`sys_openat2` in `kernel/abi/linux/sys_proc.c:483` copies `how->flags` and
`how->mode` from the user `struct open_how` but ignores `how->resolve` and
`how->size`. It then calls `sys_openat`, so `RESOLVE_*` flags have no effect.

### 2.2 Decision

Implement the full Linux `openat2` resolve-flag set:

| Flag | Semantics | Implementation file |
|------|-----------|---------------------|
| `RESOLVE_NO_SYMLINKS` | Never follow symlinks during the entire walk | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_BENEATH` | Refuse to walk above the starting directory | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_IN_ROOT` | Resolve absolute paths and `..` relative to the starting directory | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_NO_MAGICLINKS` | Refuse procfs/sysfs-style magic links | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_NO_XDEV` | Refuse to cross mount points | `kernel/fs/vfs/path_resolution.c` |
| `RESOLVE_CACHED` | Only use cached lookups; fail if a dentry is not cached | **Out of scope** — rejected with `-EINVAL` |
| `RESOLVE_NO_TRAILING_SYMLINKS` | Do not follow a symlink at the final component | handled in `vfs_open` / `vfs_fstatat` |

### 2.3 Implementation mapping

- `kernel/abi/linux/sys_proc.c`: validate `how->size` against `sizeof(struct open_how)`; reject unknown `resolve` bits with `-EINVAL`; pass resolve flags through to `vfs_openat2`.
- `kernel/fs/vfs.c` / new `kernel/fs/vfs/open.c`: add `vfs_openat2(dirfd, path, flags, mode, resolve)` that builds a resolution context and calls the resolver.
- `kernel/fs/vfs/path_resolution.c`: extend `vnode_lookup_path` to accept a `resolve_flags` bitmask and starting directory. Add helper `vfs_path_walk_beneath`.
- `kernel/fs/vfs/dcache.c`: ~~add `vfs_dcache_lookup_cached`~~ — `RESOLVE_CACHED` is rejected with `-EINVAL` because the current dentry cache is an optimization layer, not an authoritative lookup source.
- `kernel/include/fs/vfs.h`: add `RESOLVE_*` constants and an `open_how` struct layout.
- `kernel/include/abi/linux/fcntl.h`: mirror Linux `RESOLVE_*` values for the ABI boundary.
- `user/cmds/vfs_stress.c`: add `openat2` resolve-flag tests.

### 2.4 ABI notes

- Linux `struct open_how` is `{ u64 flags; u64 mode; u64 resolve; u64 __padding[8]; }`.
- A20OS must accept the same layout; `how->size` must be at least 24 bytes and
  at most `sizeof(struct open_how)`.
- Unknown `resolve` bits return `-EINVAL`.

## 3. renameat2 flags

### 3.1 Current state

`sys_renameat2` in `kernel/abi/linux/sys_path.c:31` rejects any non-zero `flags`
with `-EINVAL` and calls `vfs_rename`, which does a plain rename without flags.

### 3.2 Decision

Implement `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, and document
`RENAME_WHITEOUT` as out of scope for this wave because A20OS has no
overlay/whiteout filesystem support.

| Flag | Support | Semantics | Implementation file |
|------|---------|-----------|---------------------|
| `RENAME_NOREPLACE` | implement | Fail with `-EEXIST` if target exists | `kernel/fs/vfs.c`, backend rename ops |
| `RENAME_EXCHANGE` | implement | Atomically exchange source and target | `kernel/fs/vfs.c`, backend rename ops |
| `RENAME_WHITEOUT` | out of scope | Requires overlayfs semantics | documented as unsupported; returns `-EINVAL` |

### 3.3 Implementation mapping

- `kernel/abi/linux/sys_path.c`: validate flags; allow `RENAME_NOREPLACE | RENAME_EXCHANGE`; reject `RENAME_WHITEOUT` and incompatible combinations.
- `kernel/fs/vfs.c`: extend `vfs_rename` with a `flags` argument. Check cross-mount (`-EXDEV`), same target (`-EINVAL`), and permission/sticky-bit rules before delegating to `old_dir->ops->rename`.
- `kernel/fs/ramfs.c`: implement `RENAME_NOREPLACE` (existing lookup) and `RENAME_EXCHANGE` in `ramfs_vnode_rename`.
- `kernel/fs/ext4.c`: extend `ext4_vn_rename` to handle `RENAME_NOREPLACE` and `RENAME_EXCHANGE`.
- `kernel/fs/fat32.c`: keep `.rename = NULL`; VFS will continue to return `-ENOSYS` for FAT32.
- Backend vnode ops signature change: `int (*rename)(vnode_t *old_dir, const char *old_name, vnode_t *new_dir, const char *new_name, unsigned int flags)`.
- `kernel/include/fs/vfs.h`: update the `rename` function pointer type and add `RENAME_*` constants.
- `user/cmds/vfs_stress.c`: add renameat2 flag tests.

## 4. statx

### 4.1 Current state

`sys_statx` in `kernel/abi/linux/sys_path.c:287` validates a subset of flags,
defaults `mask` to `STATX_BASIC_STATS`, and fills a fixed set of fields. It does
not honor `AT_STATX_FORCE_SYNC` / `AT_STATX_DONT_SYNC`, does not report
`STATX_BTIME`, and ignores most of the requested `mask`.

### 4.2 Decision

- Honor `mask`: only fill fields requested by the caller; set `stx_mask` to the
  actually provided fields.
- Honor `AT_STATX_SYNC_TYPE`: `FORCE_SYNC` flushes page-cache writeback and
  re-reads inode metadata; `DONT_SYNC` uses cached values.
- Report `STATX_BTIME` as the same as `ctime` (A20OS does not track birth time).
- Reject unknown flags with `-EINVAL`.

### 4.3 Implementation mapping

- `kernel/abi/linux/sys_path.c`: pass `mask` and sync type through to
  `vfs_statx`.
- `kernel/fs/vfs.c`: add `vfs_statx(dirfd, path, flags, mask, buf)`.
- `kernel/fs/vfs/stat_perm.c`: extend `vfs_vnode_stat` to accept a sync hint, or
  add `vfs_vnode_statx`.
- `kernel/fs/fat32.c` / `kernel/fs/ext4.c` / `kernel/fs/ramfs.c`: backend
  `stat` ops already provide all basic fields; add btime reporting.
- `kernel/fs/page_cache.c`: provide `page_cache_writeback_vnode` call for
  `FORCE_SYNC`.
- `kernel/include/abi/linux/stat.h`: already defines `STATX_*` and
  `AT_STATX_*`.
- `user/cmds/vfs_stress.c`: add statx mask and sync-type tests.

## 5. faccessat2 / fchmodat2

### 5.1 Current state

- `vfs_faccessat2` in `kernel/fs/vfs.c:527` handles `AT_EACCESS`,
  `AT_SYMLINK_NOFOLLOW`, and `AT_EMPTY_PATH`.
- `sys_fchmodat` in `kernel/abi/linux/sys_path.c:232` handles
  `AT_SYMLINK_NOFOLLOW` and `AT_EMPTY_PATH` but ignores the fourth argument in
  the legacy `fchmodat` path.
- `fchmodat2` is wired in `kernel/abi/linux/syscall_table.def:74` to
  `sys_fchmodat` with the flags argument.

### 5.2 Decision

- `faccessat2`: preserve current behavior; add strict validation that no bits
  outside `AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH` are set.
- `fchmodat2`: treat as a first-class syscall with full flag support
  (`AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`). Reject unsupported bits with
  `-EINVAL`.

### 5.3 Implementation mapping

- `kernel/fs/vfs.c`: tighten `vfs_faccessat2` flag validation.
- `kernel/fs/vfs.c`: `vfs_chmodat` already checks flags; ensure
  `vfs_fchmodat2` (or the `fchmodat` with flags path) is exposed from the
  syscall table.
- `kernel/abi/linux/sys_path.c`: make `sys_fchmodat` the canonical
  implementation and ensure `fchmodat2` shares the same code path.
- `user/cmds/vfs_stress.c`: add faccessat2 flag and fchmodat2 tests.

## 6. xattr

### 6.1 Current state

- xattr syscalls live in `kernel/abi/linux/sys_xattr.c`.
- Storage is a global RAM table in `kernel/fs/xattr.c` keyed by `(mnt, ino)`.
- There are no backend-specific xattr hooks; values are not persisted for
  FAT32/ext4.
- Namespace validation (e.g., `security.*`, `trusted.*`, `user.*`) is not
  performed.

### 6.2 Decision

For P1, keep the global RAM xattr table but tighten the ABI surface:

- Reject names without a namespace prefix (`user.`, `trusted.`, `security.`,
  `system.`) with `-EINVAL`.
- Reject `security.*` and `trusted.*` if the caller lacks `CAP_SYS_ADMIN`.
- Reject xattrs on non-reg/dir/lnk vnodes with `-EOPNOTSUPP` (already done in
  `sys_xattr.c:35`).
- Keep values RAM-only; document persistence gap for block filesystems.

### 6.3 Implementation mapping

- `kernel/fs/xattr.c`: add `xattr_check_namespace` helper.
- `kernel/abi/linux/sys_xattr.c`: call namespace check before storage.
- `kernel/include/fs/xattr.h`: define namespace constants if not present.
- `kernel/fs/fat32.c` / `kernel/fs/ext4.c`: document that backend xattr hooks
  are not added in this wave.
- `user/cmds/vfs_stress.c`: add xattr namespace and permission tests.

## 7. Symlink loop limit

### 7.1 Current state

`vnode_lookup_path` increments `symlink_depth` and fails with `-ELOOP` when the
depth exceeds 8 (`kernel/fs/vfs/path_resolution.c:89`).

### 7.2 Decision

Raise the limit to Linux `MAX_SYMLINKS` (40). Keep the counter in the resolver
context.

### 7.3 Implementation mapping

- `kernel/fs/vfs/path_resolution.c`: replace the hard-coded `8` with
  `MAX_SYMLINKS`.
- `kernel/include/fs/vfs.h`: add `#define MAX_SYMLINKS 40`.
- `user/cmds/vfs_stress.c`: add a 40-link chain test and a 41-link
  `-ELOOP` test.

## 8. Mount-point `..` crossing

### 8.1 Current state

`vnode_lookup_path` handles `..` by following `vnode->parent`
(`kernel/fs/vfs/path_resolution.c:52`). If the walk is at a mount root,
`vnode->parent` points back to itself (`fat32.c:1213`, `ext4.c:1536`) or to the
parent inside the mounted filesystem (`ramfs.c:165`, `ramfs.c:266`), so `..`
cannot escape the mount.

### 8.2 Decision

Implement Linux mount-point `..` semantics:

- When resolving `..` at a mount root, switch to the parent directory of the
  mount point in the underlying filesystem.
- `RESOLVE_NO_XDEV` must prevent this crossing when used with `openat2`.
- `chroot` combined with mount crossing must respect both boundaries.

### 8.3 Implementation mapping

- `kernel/fs/vfs/mount.c`: add `vfs_mount_parent(mount_t *mnt)` that returns the
  mount covering the parent path of `mnt->path`.
- `kernel/fs/vfs/path_resolution.c`: in `vnode_lookup_path`, before following
  `vnode->parent`, check if `cur` is a mount root; if so, switch to the parent
  mount's vnode and continue.
- `kernel/fs/vfs.h`: add mount-root detection helper.
- `user/cmds/vfs_stress.c`: add tests for `..` escaping `/bin` (FAT32 mount)
  back to `/`.

## 9. chroot

### 9.1 Current state

`sys_chroot` in `kernel/abi/linux/sys_namespace.c:26` validates the target as a
directory, checks `CAP_SYS_CHROOT`, stores the path in `cur->fs.root_path`, and
resets `cwd` to `/`. However, `vfs_resolve_at` and `vnode_lookup_path` do not
consult `root_path`, so `chroot` has no effect on subsequent path resolution.

### 9.2 Decision

Make path resolution honor `task->fs.root_path`:

- Absolute paths are interpreted relative to `root_path`.
- `..` resolution must not escape `root_path`.
- `openat(fd, "../../..", ...)` from a chrooted process must stay inside the
  chroot.
- `execve` of an absolute path must also use `root_path`.

### 9.3 Implementation mapping

- `kernel/fs/vfs/path_resolution.c`: add `vfs_resolve_rooted` that joins
  `root_path` with the requested path before normalization.
- `kernel/fs/vfs.c`: make `vfs_resolve`, `vfs_resolve_at`, and
  `vfs_resolve_no_follow_final` use the current task's `root_path`.
- `kernel/proc/exec.c`: ensure `proc_exec` resolves the executable path through
  the same rooted resolver.
- `kernel/abi/linux/sys_namespace.c`: keep the existing `sys_chroot`; no change
  needed there.
- `user/cmds/vfs_stress.c`: add chroot escape tests.

## 10. Summary of changed / extended files

| File | Change |
|------|--------|
| `kernel/abi/linux/sys_proc.c` | `openat2` validation and dispatch |
| `kernel/abi/linux/sys_path.c` | `renameat2` flags, `statx` sync/mask, `fchmodat2` flags |
| `kernel/abi/linux/sys_xattr.c` | namespace and capability checks |
| `kernel/fs/vfs.c` | `vfs_rename` flags, `vfs_faccessat2` validation, rooted resolution hooks |
| `kernel/fs/vfs/path_resolution.c` | `RESOLVE_*`, `MAX_SYMLINKS`, mount `..` crossing, chroot root |
| `kernel/fs/vfs/mount.c` | mount-parent lookup helper |
| `kernel/fs/vfs/dcache.c` | `RESOLVE_CACHED` lookup helper |
| `kernel/fs/vfs/stat_perm.c` | `vfs_vnode_statx` / sync hints |
| `kernel/fs/xattr.c` | namespace validation helper |
| `kernel/fs/ramfs.c` | `RENAME_NOREPLACE` / `RENAME_EXCHANGE` |
| `kernel/fs/ext4.c` | `RENAME_NOREPLACE` / `RENAME_EXCHANGE` |
| `kernel/fs/fat32.c` | no rename changes (remains unsupported) |
| `kernel/include/fs/vfs.h` | new constants and vnode ops signature |
| `kernel/include/abi/linux/fcntl.h` | `RESOLVE_*` and `RENAME_*` ABI values |
| `kernel/include/abi/linux/stat.h` | already defines `STATX_*` |
| `kernel/proc/exec.c` | rooted path resolution for executable |
| `user/cmds/vfs_stress.c` | new edge-case tests |

## 11. Test gates

The following smoke / stress gates must be added or extended:

- `smoke-vfs-edge`: openat2 resolve flags, renameat2 flags, statx mask/sync,
  faccessat2/fchmodat2 flags, xattr namespace, symlink loop limit, mount `..`
  crossing, chroot.
- Existing gates (`smoke-abi-linux`, `check-vfs-abstraction`) must remain green.

`smoke-vfs-fs-specific` (per-backend unsupported-op errno matrix) is out of scope
for P1; backend capability differences remain documented in
`docs/fs-consistency-model.md`.

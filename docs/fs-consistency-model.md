# A20OS Filesystem Consistency Model

This document records the per-backend capability, consistency, and Linux ABI
behavior matrix for A20OS P1. Every claim is grounded in the current
implementation under `kernel/fs/` and the VFS wrappers in `kernel/fs/vfs.c`.
It is a design contract for Wave 1 implementation and the test gates that
follow.

## 1. Scope

Covered backends:

- `kernel/fs/fat32.c` — FAT32 block-device filesystem
- `kernel/fs/ext4.c` — ext4 block-device filesystem
- `kernel/fs/ramfs.c` — in-memory filesystem (rootfs, `/dev/shm`, `tmpfs`)
- `kernel/fs/devfs.c` — device special-file tree (`/dev`)
- `kernel/fs/procfs.c` — process/synthetic tree (`/proc`)
- `kernel/fs/sysfs.c` — system object tree (`/sys`)
- `kernel/fs/pipe.c` — anonymous pipe implementation
- `kernel/fs/anonfd.c` — anonymous fd installation helper

Covered VFS cross-cutting behavior:

- permission checks (`kernel/fs/vfs/stat_perm.c`, `kernel/fs/vfs.c`)
- path resolution (`kernel/fs/vfs/path_resolution.c`)
- mount-point handling (`kernel/fs/vfs/mount_ops.c`, `kernel/fs/vfs/mount.c`)
- dcache invalidation (`kernel/fs/vfs/dcache.c`)
- xattr storage (`kernel/fs/xattr.c`)

## 2. Legend

| Column | Meaning |
|--------|---------|
| **Op** | VFS vnode operation or high-level syscall behavior |
| **Support** | `Y` = implemented and exercised, `N` = returns an explicit errno, `-` = not applicable |
| **Errno** | errno returned on the unsupported or error path (from code, not intent) |
| **Ordering / Atomicity** | guarantees the current code actually provides |
| **Linux ABI gap** | divergence from Linux that tests must encode |

## 3. Per-Backend Matrix

### 3.1 FAT32 (`kernel/fs/fat32.c`)

FAT32 is mounted from a virtio-blk block cache. The superblock is protected by
`fat32_sb_t.lock`. Per-inode metadata (mode, uid, gid) is kept in a global RAM
table `g_fat32_meta` keyed by cluster number (`fat32.c:295`).

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| lookup | Y | — | `fat32_lookup` (`fat32.c:321`). Supports `.` and `..`. Case-insensitive 8.3 + LFN. |
| create | Y | — | `fat32_vn_create` (`fat32.c:467`). Allocates one cluster, writes 8.3 short entry. |
| mkdir | Y | — | `fat32_vn_mkdir` (`fat32.c:381`). Creates `.`/`..` entries. |
| unlink | Y | — | `fat32_vn_unlink` (`fat32.c:535`). Frees cluster chain. Returns `-EISDIR` for directories. |
| rmdir | Y | — | `fat32_vn_rmdir` (`fat32.c:708`). Checks `fat32_dir_is_empty` (counts active entries > 2). |
| rename | N | `-ENOSYS` | `g_fat32_vnode_ops.rename` is `NULL` (`fat32.c:758`). `vfs_rename` returns `-ENOSYS` after same-mount check (`vfs.c:348`). |
| link | N | `-ENOSYS` | No `.link` op in `g_fat32_vnode_ops`. `vfs_link` returns `-ENOSYS` (`vfs.c:822`). |
| symlink | N | `-ENOSYS` | No `.symlink`/`.readlink` ops. `vfs_symlink` returns `-ENOSYS` (`vfs.c:861`). |
| readlink | N | `-EINVAL` | Symlink vnodes cannot exist; `vfs_readlinkat` returns `-EINVAL` (`vfs.c:771`). |
| stat | Y | — | `fat32_stat` (`fat32.c:357`). `st_nlink` hard-coded to 1. `st_blocks` rounded up by 512. |
| truncate | Y | — | `fat32_vn_truncate` (`fat32.c:574`). Size 0 re-allocates a single cluster. |
| chmod | Y | — | `fat32_vn_chmod` (`fat32.c:655`). Stores in RAM meta table only. |
| chown | Y | — | `fat32_vn_chown` (`fat32.c:669`). Stores in RAM meta table only; clears suid/sgid bits. |
| read/write/lseek | Y | — | `g_fat32_fops` (`fat32.c:1149`). Per-open `fat32_fctx_t` with cluster cache. |
| readdir | Y | — | `fat32_freaddir` (`fat32.c:1046`). Returns `DT_DIR`/`DT_REG`; no `DT_LNK`. |
| ioctl | N | `-ENOTTY` | `.ioctl` is `NULL`; `vfs_ioctl` falls through to `-ENOTTY`. |
| fsync | partial | — | `vfs_fsync` syncs the block cache (`vfs/file.c:232`) but FAT32 has no explicit inode log. |
| xattr | N | `-EOPNOTSUPP` | No backend hooks; `sys_xattr_*` rejects non reg/dir/lnk then falls to RAM table, which survives only for mounted vnodes. |

**FAT32 ordering guarantees**

- The whole filesystem is serialized by `sb->lock` (`fat32.c:271`).
- Directory entry updates and FAT updates are not atomic with respect to each
  other; a crash after freeing clusters but before marking the directory entry
  deleted could leak clusters.
- File size is written back to the directory entry only on close
  (`fat32_fclose`, `fat32.c:1105`). A power loss before close loses size.
- The block cache is write-back; `bcache_sync` is called on `vfs_fsync` and
  unmount (`fat32_unmount`, `fat32.c:1218`).

**FAT32 ABI gaps**

- No rename, hard links, or symbolic links.
- File ownership/mode is volatile (RAM-only).
- No atime/mtime/ctime persistence; timestamps fall back to current time in
  `vfs_vnode_stat` (`vfs/stat_perm.c:188`).
- `st_nlink` is always 1.
- No xattr persistence.

### 3.2 ext4 (`kernel/fs/ext4.c`)

ext4 is mounted from a block cache and uses a short-lived vnode model: every
lookup creates a fresh vnode and it is freed when its refcount reaches zero
(`ext4.c:67`). The inode cache hooks are stubbed out (`ext4.c:75`).

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| lookup | Y | — | `ext4_lookup` (`ext4.c:731`). Supports `.`/`..`. |
| create | Y | — | `ext4_vn_create` (`ext4.c:802`). Allocates inode, writes directory entry. |
| mkdir | Y | — | `ext4_vn_mkdir` (`ext4.c:848`). Allocates one block for `.`/`..`. |
| unlink | Y | — | `ext4_vn_unlink` (`ext4.c:934`). Rejects directories with `-EISDIR`. |
| rmdir | Y | — | `ext4_vn_rmdir` (`ext4.c:971`). Checks empty except `.`/`..`. |
| rename | Y | — | `ext4_vn_rename` (`ext4.c:991`). Replaces existing target, no exchange. |
| link | N | `-ENOSYS` | No `.link` op in `g_ext4_vnode_ops`. |
| symlink | Y | — | `ext4_vn_symlink` (`ext4.c:1050`). Fast symlink only (target <= 60 bytes). |
| readlink | Y | — | `ext4_readlink` (`ext4.c:1034`). Reads up to 60 bytes from `i_block`. |
| stat | Y | — | `ext4_stat` (`ext4.c:765`). `st_nlink` hard-coded to 1. |
| truncate | Y | — | `ext4_vn_truncate` (`ext4.c:1092`). Zero size truncates blocks. |
| chmod | Y | — | `ext4_vn_chmod` (`ext4.c:1111`). Writes `i_mode` to disk. |
| chown | Y | — | `ext4_vn_chown` (`ext4.c:1121`). Writes `i_uid`/`i_gid`; clears suid/sgid. |
| read/write/lseek | Y | — | `g_ext4_fops` (`ext4.c:1394`). |
| readdir | Y | — | `ext4_freaddir` (`ext4.c:1332`). Returns `DT_DIR`/`DT_REG`/`DT_LNK`. |
| ioctl | N | `-ENOTTY` | `.ioctl` is `NULL`. |
| fsync | partial | — | Syncs block cache; no journal is used, so metadata and data are not ordered. |
| xattr | N | `-EOPNOTSUPP` | No backend hooks. |

**ext4 ordering guarantees**

- Inode allocation, block allocation, and directory entry writes are performed
  under `sb->alloc_lock` (`ext4.c:166`, `ext4.c:188`, `ext4.c:205`) but there is
  no journal or ordered writeback.
- `ext4_vn_rename` (`ext4.c:991`) removes the target, adds the new entry, then
  removes the old entry. A crash can leave both or neither entry present.
- `vfs_fsync` calls `bcache_sync` on the mount's block cache (`vfs/file.c:232`).

**ext4 ABI gaps**

- No hard links.
- Fast symlinks only; longer targets return `-ENAMETOOLONG`
  (`ext4.c:1062`).
- `st_nlink` is always 1; link count is not maintained.
- No journaling; metadata updates are not atomic across multiple blocks.
- No xattr.

### 3.3 ramfs (`kernel/fs/ramfs.c`)

ramfs is the root filesystem and the backend for `/dev/shm` and explicit
`tmpfs`/`ramfs` mounts. It uses a single global inode table with a fixed
maximum of `RAMFS_MAX_INODES` (4096) and `RAMFS_MAX_DIR_ENTRIES` (256) per
directory.

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| lookup | Y | — | `ramfs_vnode_lookup` (`ramfs.c:220`). |
| create | Y | — | `ramfs_vnode_create` (`ramfs.c:276`). |
| mkdir | Y | — | `ramfs_vnode_mkdir` (`ramfs.c:246`). |
| unlink | Y | — | `ramfs_vnode_unlink` (`ramfs.c:323`). Decrements `nlink`, may free. |
| rmdir | Y | — | `ramfs_vnode_rmdir` (`ramfs.c:436`). Empty check counts active entries > 2. |
| rename | Y | — | `ramfs_vnode_rename` (`ramfs.c:392`). Same mount only (enforced by VFS). |
| link | Y | — | `ramfs_vnode_link` (`ramfs.c:380`). Rejects directories. |
| symlink | Y | — | `ramfs_vnode_symlink` (`ramfs.c:353`). |
| readlink | Y | — | `ramfs_vnode_readlink` (`ramfs.c:342`). |
| stat | Y | — | `ramfs_vnode_stat` (`ramfs.c:234`). `st_nlink` from inode. |
| truncate | Y | — | `ramfs_vnode_truncate` (`ramfs.c:489`). |
| chmod | Y | — | `ramfs_vnode_chmod` (`ramfs.c:465`). |
| chown | Y | — | `ramfs_vnode_chown` (`ramfs.c:472`). |
| read/write/lseek | Y | — | `g_ramfs_fops` (`ramfs.c:692`). |
| readdir | Y | — | `ramfs_freaddir` (`ramfs.c:650`). |
| ioctl | N | `-ENOTTY` | No `.ioctl` op. |
| fsync | Y (no-op) | — | `vfs_fsync` syncs block cache; ramfs has none, so effectively no-op. |
| xattr | partial | — | Stored in global RAM table (`kernel/fs/xattr.c`), lost on reboot. |

**ramfs ordering guarantees**

- All ramfs operations are in-memory and serialized by the big-kernel implicit
  single-threaded paths (no per-inode lock).
- `ramfs_vnode_link` correctly increments `nlink` (`ramfs.c:388`).
- `ramfs_vnode_unlink` decrements `nlink` and frees the inode when
  `nlink == 0 && ref_count <= 1` (`ramfs.c:76`).

**ramfs ABI gaps**

- Directory entry limit is 256 per directory (`ramfs.c:11`).
- Total inode limit is 4096 (`ramfs.c:10`).
- No persistence; xattrs are global RAM only.

### 3.4 devfs (`kernel/fs/devfs.c`)

devfs is a synthetic device tree. It has no mutable directory content; entries
are fixed at compile time in `g_nodes` (`devfs.c:71`).

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| lookup | Y | — | `devfs_lookup` (`devfs.c:511`). Root, `misc/`, and `pts/` directories only. |
| create | N | `-ENOSYS` | No `.create` op; `vfs_open` with `O_CREAT` returns `-ENOSYS` (`vfs.c:95`). |
| mkdir | N | `-ENOTDIR` | No `.mkdir` op; `vfs_mkdir` returns `-ENOTDIR` (`vfs.c:223`). |
| unlink | N | `-ENOTDIR` | No `.unlink` op; `vfs_unlink` returns `-ENOTDIR` (`vfs.c:277`). |
| rmdir | N | `-ENOSYS` | No `.rmdir` op; `vfs_rmdir` returns `-ENOSYS` (`vfs.c:415`). |
| rename | N | `-ENOSYS` | No `.rename` op. |
| link | N | `-ENOSYS` | No `.link` op. |
| symlink | N | `-ENOSYS` | No `.symlink` op. |
| stat | Y | — | `devfs_stat` (`devfs.c:551`). Reports `S_IFCHR`/`S_IFBLK`/`S_IFDIR`. |
| chmod/chown | N | `-EPERM` | No `.chmod`/`.chown` ops; `vfs_chmod_vnode` returns `-EPERM` (`vfs.c:586`). |
| open | Y | — | `devfs_open_vnode` (`devfs.c:585`). Dispatches to per-device `vfile_ops_t`. |
| read/write/ioctl | Y | — | Per-kind `vfile_ops_t` tables (`devfs.c:492`). |
| readdir | Y | — | `devfs_dir_readdir` (`devfs.c:176`). |

**devfs ordering guarantees**

- devfs nodes are static after `vfs_init`; there is no concurrency beyond the
  per-device state (TTY, loop, PTY).

**devfs ABI gaps**

- Cannot create, remove, or rename device nodes.
- `chmod`/`chown` are not supported.
- `/dev` content is hard-coded; uevent-driven node creation is not implemented.

### 3.5 procfs (`kernel/fs/procfs.c`)

procfs is a fully synthetic filesystem. Entries are generated on `lookup` and
`open`. There are no backend mutation operations.

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| lookup | Y | — | `procfs_lookup` (`procfs.c:896`). Numeric PIDs and static entries. |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-ENOSYS`/`-ENOTDIR` | `g_procfs_vnode_ops` only defines `lookup`, `stat`, `open`, `release` (`procfs.c:1038`). |
| stat | Y | — | `procfs_stat` (`procfs.c:1001`). |
| open | Y | — | `procfs_open_vnode` (`procfs.c:1322`). Allocates `procfs_priv_t` snapshot. |
| read | Y | — | `procfs_fread` (`procfs.c:1046`). |
| write | partial | `-EINVAL` | Only specific tunables accept writes (`procfs_fwrite`, `procfs.c:1086`). |
| lseek | Y | — | `procfs_flseek` (`procfs.c:1146`). |
| readdir | Y | — | `procfs_freaddir` (`procfs.c:1162`). |
| chmod/chown | N | `-EPERM` | No backend hooks. |

**procfs ordering guarantees**

- Content is generated at `open` time and cached in `procfs_priv_t`; concurrent
  process state changes are not reflected after open.
- Some writable tunables (`oom_score_adj`, `pid_max`, `pipe-max-size`) are
  updated without synchronization other than the global spinlock around the
  integer writes.

**procfs ABI gaps**

- Many `/proc/<pid>` files exist only as placeholders and return empty or
  static content.
- `/proc/self/exe` and `/proc/<pid>/exe`/`cwd` are handled as special cases in
  `vfs_readlinkat` (`vfs.c:694`) and not as real symlinks.
- File mutations are not allowed.

### 3.6 sysfs (`kernel/fs/sysfs.c`)

sysfs is a minimal synthetic tree. Currently it exposes only
`/sys/block/loopN/{dev,size,uevent}`.

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| lookup | Y | — | `sysfs_lookup` (`sysfs.c:91`). |
| create/mkdir/unlink/rmdir/rename/link/symlink | N | `-ENOSYS`/`-ENOTDIR` | `g_sysfs_vnode_ops` only defines `lookup`, `stat`, `open`, `release` (`sysfs.c:191`). |
| stat | Y | — | `sysfs_stat` (`sysfs.c:170`). |
| open/read/lseek/readdir | Y | — | `g_sysfs_fops` (`sysfs.c:298`). |
| write | N | `-EINVAL` | `.write` is not registered. |
| chmod/chown | N | `-EPERM` | No backend hooks. |

**sysfs ordering guarantees**

- Content is generated at `open` time from static constants.
- No concurrent mutation is supported.

**sysfs ABI gaps**

- Only loop block devices are exposed.
- No writeable attributes, no uevent writes.

### 3.7 pipe (`kernel/fs/pipe.c`)

pipe is not a mounted filesystem. It creates a pair of `vfile_t` objects that
share a `pipe_buf_t` ring buffer.

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| create (pipe2) | Y | — | `pipe_create` (`pipe.c:354`). Allocates ring buffer, two global fds. |
| read | Y | — | `pipe_read` (`pipe.c:70`). Blocks unless `O_NONBLOCK`. |
| write | Y | — | `pipe_write` (`pipe.c:108`). Writes <= `PIPE_BUF` atomically; larger writes may be interleaved. |
| poll | Y | — | `pipe_poll_events` (`pipe.c:313`). |
| set_size | Y | — | `pipe_set_size` (`pipe.c:346`), limited by `CAP_SYS_RESOURCE` in `vfs_fcntl`. |
| lseek | N | `-ESPIPE` | No `.lseek` op; `vfs_lseek` returns `-ESPIPE`. |

**pipe ordering guarantees**

- Writes of `PIPE_BUF` bytes or less are atomic with respect to each other
  (`pipe.c:126`); the spinlock is held while copying the whole chunk.
- Writes larger than `PIPE_BUF` are split and may be interleaved with other
  writers.
- `read`/`write` block on wait queues and wake all readers/writers
  (`pipe.c:27`).
- Closing the last reader sends `SIGPIPE`/`EPIPE` to writers
  (`pipe.c:115`, `pipe.c:129`).

**pipe ABI gaps**

- `PIPE_BUF` value is defined in `core/consts.h`; verify it matches the Linux
  ABI expectation of 4096 on all architectures.
- `F_SETPIPE_SZ` capacity limit is hard-coded to 1 MiB for unprivileged tasks
  (`vfs.c:1069`).

### 3.8 anonfd (`kernel/fs/anonfd.c`)

anonfd is a helper for installing anonymous vfiles into the current fd table.
It is not a filesystem and has no path semantics.

| Op | Support | Errno | Notes / Code reference |
|----|---------|-------|------------------------|
| install | Y | — | `anonfd_install_vfile` (`anonfd.c:17`). |
| close | Y | — | Frees `vf->priv` via `anonfd_free_priv_close` (`anonfd.c:8`). |

**anonfd ordering guarantees**

- Installation and close are single-threaded with respect to the calling task's
  `fdtable`.

**anonfd ABI gaps**

- None by design; it is an internal helper, not a Linux ABI surface.

## 4. VFS Cross-Cutting Behavior

### 4.1 Permission model

- `vfs_vnode_permission` calls `vfs_mode_has_perm_ids` with the current task's
  `fsuid`/`fsgid` (`vfs/stat_perm.c:72`, `vfs/stat_perm.c:201`).
- `CAP_DAC_OVERRIDE` bypasses read/write but still refuses execute on regular
  files with no execute bit (`vfs/stat_perm.c:80`).
- `access()` / `faccessat()` without `AT_EACCESS` uses the real uid/gid and no
  capabilities (`vfs_mode_has_perm_ids_nocap`, `vfs/stat_perm.c:114`).
- `vfs_chmod_vnode` requires ownership or `CAP_FOWNER` (`vfs.c:575`).
- `vfs_chown_vnode` requires `CAP_CHOWN` or restricted ownership rules
  (`vfs.c:621`).
- Sticky-bit removal checks are applied in `vfs_sticky_may_remove`
  (`vfs/stat_perm.c:221`).

### 4.2 Path resolution

- `vnode_lookup_path` resolves absolute paths within a mount root
  (`path_resolution.c:27`).
- Symlinks are followed during the walk with a hard-coded depth limit of 8
  (`path_resolution.c:89`).
- `..` is resolved by following `vnode->parent` (`path_resolution.c:52`). It
  does **not** cross mount points; walking `..` out of a mount root stays inside
  the mount root.
- Name length >= `MAX_NAME_LEN` returns `-ENAMETOOLONG`
  (`path_resolution.c:60`).
- `vfs_resolve_no_follow_final` resolves the parent directory and looks up the
  final component without following it (`vfs.c:437`).

### 4.3 Mount operations

- `vfs_mount` supports `tmpfs`/`ramfs`, `cgroup`/`cgroup2`, and block-device
  mounts for `fat32`/`vfat`/`ext4` (`vfs/mount_ops.c:47`).
- `vfs_mount_bc` tries `ext4` first, then `vfat`, when fstype is empty
  (`vfs/mount_ops.c:134`).
- `vfs_umount` matches by normalized path and calls `fat32_unmount` or
  `ext4_unmount` (`vfs/mount_ops.c:210`).
- `vfs_rename` rejects cross-mount renames with `-EXDEV` (`vfs.c:328`).

### 4.4 Dcache

- Positive lookups are cached in `vfs_dcache_lookup`/`vfs_dcache_insert`
  (`kernel/fs/vfs/dcache.c`).
- `vfs_dcache_invalidate_all` is called after create, unlink, rmdir, rename,
  link, symlink, mount, and umount. There is no fine-grained per-directory
  invalidation except `vfs_dcache_invalidate(old_dir, old_name)` and
  `vfs_dcache_invalidate(new_dir, new_name)` in rename (`vfs.c:378`).

### 4.5 xattr

- xattr is stored in a global RAM table `g_xattrs` of 1024 entries keyed by
  `(mnt, ino)` (`kernel/fs/xattr.c:17`).
- `XATTR_CREATE` and `XATTR_REPLACE` flags are honored (`xattr.c:55`).
- Names are limited to `XATTR_NAME_MAX_LOCAL`; values to
  `XATTR_VALUE_MAX_LOCAL`.
- There are no backend-specific xattr hooks; values are not written to disk for
  FAT32/ext4 and are lost on unmount/reboot.

## 5. Known Linux ABI Gaps (Summary)

| Area | Current behavior | Expected Linux behavior |
|------|------------------|-------------------------|
| openat2 resolve flags | Ignored; treated as `openat` (`sys_proc.c:483`) | Must honor `RESOLVE_*` |
| renameat2 flags | Rejected (`sys_path.c:33`) | Support `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, `RENAME_WHITEOUT` |
| statx | Basic stats only (`sys_path.c:287`) | Honor `mask`, `AT_STATX_*`, `STATX_*` |
| fchmodat2 | Dispatched to `sys_fchmodat` with flags (`syscall_table.def:74`) | Independent syscall with full `AT_*` flag handling |
| xattr | Global RAM table only | Backend-persistent xattrs with namespace checks |
| Symlink loop limit | 8 (`path_resolution.c:89`) | Linux uses 40 (`MAX_SYMLINKS`) |
| Mount-point `..` | Stays inside mount root | Crosses to parent mount |
| chroot | Sets `root_path` but resolution ignores it (`sys_namespace.c:26`) | Resolution must be bounded by `root_path` |
| faccessat2 flags | `AT_EACCESS`/`AT_SYMLINK_NOFOLLOW`/`AT_EMPTY_PATH` supported | Also validate unsupported bits strictly |

## 6. Test Implications

- Each unsupported operation in the matrix above must have a test that asserts
  the exact errno.
- Cross-mount `rename` and `link` must return `-EXDEV`.
- Permission tests must cover real uid/gid (`access`), effective uid/gid
  (`faccessat2(AT_EACCESS)`), capability bypass, and sticky-bit directories.
- FAT32/ext4 must have fsync/truncate/page-cache coherence tests.
- Pipe tests must verify atomicity of `PIPE_BUF`-sized writes and `SIGPIPE`
  delivery.

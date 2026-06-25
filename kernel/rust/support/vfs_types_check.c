/* Layout consistency checks for Rust-exposed VFS types.
 *
 * These assertions guard the C side against drift; the matching Rust
 * definitions live in kernel/rust/support/vfs.rs.  If either side changes,
 * both must be updated and the build here will fail until they agree.
 */

#include "core/consts.h"
#include "core/sync.h"
#include "fs/vfs.h"

_Static_assert(sizeof(refcount_t) == 4, "Rust refcount_t size mismatch");

_Static_assert(sizeof(spinlock_t) == 4, "Rust spinlock_t size mismatch");

_Static_assert(sizeof(wait_queue_t) == 16, "Rust wait_queue_t size mismatch");

_Static_assert(sizeof(mutex_t) == 32, "Rust mutex_t size mismatch");

_Static_assert(offsetof(vfile_t, vnode) == 0, "Rust vfile_t.vnode offset mismatch");
_Static_assert(offsetof(vfile_t, flags) == 8, "Rust vfile_t.flags offset mismatch");
_Static_assert(offsetof(vfile_t, offset) == 16, "Rust vfile_t.offset offset mismatch");
_Static_assert(offsetof(vfile_t, offset_lock) == 24, "Rust vfile_t.offset_lock offset mismatch");
_Static_assert(offsetof(vfile_t, ref_count) == 56, "Rust vfile_t.ref_count offset mismatch");
_Static_assert(offsetof(vfile_t, owner_type) == 60, "Rust vfile_t.owner_type offset mismatch");
_Static_assert(offsetof(vfile_t, owner_pid) == 64, "Rust vfile_t.owner_pid offset mismatch");
_Static_assert(offsetof(vfile_t, owner_signal) == 68, "Rust vfile_t.owner_signal offset mismatch");
_Static_assert(offsetof(vfile_t, seals) == 72, "Rust vfile_t.seals offset mismatch");
_Static_assert(offsetof(vfile_t, lease) == 76, "Rust vfile_t.lease offset mismatch");
_Static_assert(offsetof(vfile_t, rw_hint) == 80, "Rust vfile_t.rw_hint offset mismatch");
_Static_assert(offsetof(vfile_t, path) == 88, "Rust vfile_t.path offset mismatch");
_Static_assert(offsetof(vfile_t, ops) == 600, "Rust vfile_t.ops offset mismatch");
_Static_assert(offsetof(vfile_t, priv) == 608, "Rust vfile_t.priv offset mismatch");
_Static_assert(sizeof(vfile_t) == 616, "Rust vfile_t size mismatch");

_Static_assert(offsetof(vfile_ops_t, read) == 0, "Rust vfile_ops_t.read offset mismatch");
_Static_assert(offsetof(vfile_ops_t, write) == 8, "Rust vfile_ops_t.write offset mismatch");
_Static_assert(offsetof(vfile_ops_t, lseek) == 16, "Rust vfile_ops_t.lseek offset mismatch");
_Static_assert(offsetof(vfile_ops_t, readdir) == 24, "Rust vfile_ops_t.readdir offset mismatch");
_Static_assert(offsetof(vfile_ops_t, ioctl) == 32, "Rust vfile_ops_t.ioctl offset mismatch");
_Static_assert(offsetof(vfile_ops_t, close) == 40, "Rust vfile_ops_t.close offset mismatch");
_Static_assert(sizeof(vfile_ops_t) == 48, "Rust vfile_ops_t size mismatch");

/* Layout consistency checks for Rust-exposed VFS types.
 *
 * These assertions guard the C side against drift; the matching Rust
 * definitions live in kernel/rust/support/vfs.rs.  If either side changes,
 * both must be updated and the build here will fail until they agree.
 */

#include "core/consts.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/types.h"
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

_Static_assert(offsetof(a20_watch_entry_t, target_handle) == 0, "Rust a20_watch_entry_t.target_handle offset mismatch");
_Static_assert(offsetof(a20_watch_entry_t, target_object) == 8, "Rust a20_watch_entry_t.target_object offset mismatch");
_Static_assert(offsetof(a20_watch_entry_t, target_type) == 16, "Rust a20_watch_entry_t.target_type offset mismatch");
_Static_assert(offsetof(a20_watch_entry_t, event_mask) == 24, "Rust a20_watch_entry_t.event_mask offset mismatch");
_Static_assert(offsetof(a20_watch_entry_t, user_data) == 32, "Rust a20_watch_entry_t.user_data offset mismatch");
_Static_assert(offsetof(a20_watch_entry_t, owner_queue) == 40, "Rust a20_watch_entry_t.owner_queue offset mismatch");
_Static_assert(offsetof(a20_watch_entry_t, next) == 48, "Rust a20_watch_entry_t.next offset mismatch");
_Static_assert(sizeof(a20_watch_entry_t) == 56, "Rust a20_watch_entry_t size mismatch");

_Static_assert(offsetof(a20_eventq_t, refcount) == 0, "Rust a20_eventq_t.refcount offset mismatch");
_Static_assert(offsetof(a20_eventq_t, lock) == 4, "Rust a20_eventq_t.lock offset mismatch");
_Static_assert(offsetof(a20_eventq_t, waiters) == 8, "Rust a20_eventq_t.waiters offset mismatch");
_Static_assert(offsetof(a20_eventq_t, watches) == 24, "Rust a20_eventq_t.watches offset mismatch");
_Static_assert(offsetof(a20_eventq_t, watch_count) == 32, "Rust a20_eventq_t.watch_count offset mismatch");
_Static_assert(offsetof(a20_eventq_t, ring) == 40, "Rust a20_eventq_t.ring offset mismatch");
_Static_assert(offsetof(a20_eventq_t, ring_cap) == 48, "Rust a20_eventq_t.ring_cap offset mismatch");
_Static_assert(offsetof(a20_eventq_t, ring_head) == 52, "Rust a20_eventq_t.ring_head offset mismatch");
_Static_assert(offsetof(a20_eventq_t, ring_tail) == 56, "Rust a20_eventq_t.ring_tail offset mismatch");
_Static_assert(offsetof(a20_eventq_t, ring_count) == 60, "Rust a20_eventq_t.ring_count offset mismatch");
_Static_assert(sizeof(a20_eventq_t) == 64, "Rust a20_eventq_t size mismatch");

_Static_assert(offsetof(a20_pending_event_t, source) == 0, "Rust a20_pending_event_t.source offset mismatch");
_Static_assert(offsetof(a20_pending_event_t, type) == 4, "Rust a20_pending_event_t.type offset mismatch");
_Static_assert(offsetof(a20_pending_event_t, events) == 8, "Rust a20_pending_event_t.events offset mismatch");
_Static_assert(offsetof(a20_pending_event_t, user_data) == 16, "Rust a20_pending_event_t.user_data offset mismatch");
_Static_assert(offsetof(a20_pending_event_t, data0) == 24, "Rust a20_pending_event_t.data0 offset mismatch");
_Static_assert(offsetof(a20_pending_event_t, data1) == 32, "Rust a20_pending_event_t.data1 offset mismatch");
_Static_assert(offsetof(a20_pending_event_t, data2) == 40, "Rust a20_pending_event_t.data2 offset mismatch");
_Static_assert(sizeof(a20_pending_event_t) == 48, "Rust a20_pending_event_t size mismatch");

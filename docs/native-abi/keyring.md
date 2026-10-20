# Linux keyrings

## Existing subsystem contract

`kernel/abi/linux/sys_keyring.c` decodes Linux syscall arguments, bounds and
copies input strings/payloads, and performs output usercopy. It calls the
`keyring_*` API in `kernel/include/ipc/keyring.h`. The selectable C reference
in `kernel/ipc/keyring.c` owns a global serial map of key/keyring objects, a
coarse irqsave lock, keyring memberships and references, task session-ring
hooks, and a persistent per-uid ring cache.

The currently implemented commands are add or replace, request, get keyring
id, join session, chown, set permission mask, set timeout, link, unlink,
search, describe, and read. Fork shares the task's session-ring reference and
exit releases it. Request searches the session ring and then the user ring,
and may best-effort link a found key into a requested destination ring. Search
walks nested rings to the configured depth and ignores expired keys. Duplicate
links succeed without adding another reference; unlinking a non-member returns
`-ENOENT`. Attribute changes require the current effective uid to own the
object or `CAP_SYS_ADMIN`. The stored permission mask is not yet used to gate
read/search/link operations. `KEYCTL_CLEAR`, `KEYCTL_INVALIDATE`, and other
unimplemented commands continue to return `-EOPNOTSUPP`; request callout data
is ignored.

One legacy detail is kept distinct from the Linux-format description
contract: when the kernel helper is queried with `buf == NULL`, the C
reference estimates length using eleven bytes for each uid/gid field. For
ordinary decimal ids this estimate is larger than the rendered string; a
rendered call returns the actual byte count. The Rust API mirrors this
helper-only estimate to preserve the current syscall's length-query result.
Guest tests should record it as a compatibility quirk, not as a correctness
oracle. A future behavior change should update both selectors and the syscall
contract tests together.

Timeout conversion saturates multiplication and addition at `u64::MAX`, so a
very large nonzero timeout cannot wrap into immediate expiry; zero clears the
expiry. Lazy special-ring creation still cannot expose a distinct `ENOMEM`
result because its public helper returns only a null pointer, so callers map
that failure to `ENOKEY`. This is an existing API limit. The stored permission
mask is retained and can be changed, but full `KEYCTL` permission-bit
enforcement is not implemented; the implemented owner/`CAP_SYS_ADMIN`
attribute checks remain active. These limits mean this module does not claim
complete Linux keyring semantics.

## Rust responsibilities in this batch

`kernel/ipc/keyring.rs` owns all semantic state and decisions:
key and ring allocations, serial map and serial allocation, object references,
many-to-many membership nodes, special serial resolution, session and
persistent user-ring lifecycle, expiry, lookup, search depth, owner/capability
checks, and all API operation results. It exports `a20_rust_keyring_*` core
entry points. `kernel/ipc/keyring_rust_bridge.c` forwards the established
public `keyring_*` ABI and fork/exit hooks to Rust.

The bridge contains only mechanical kernel access: current task and credential
fields, capability lookup, session-slot get/set, irqsave lock/unlock,
monotonic nanoseconds, and slab allocation/free. The Linux syscall file
continues to own syscall wire decoding and user pointers; it does not duplicate
object policy.

The Rust core keeps the C reference selectable for comparison. The source is
written without architecture-specific instructions; only RV64 is the initial
declared production validation target. Do not infer that other target builds
or deployment profiles have passed from the source's architecture-neutral
form.

The Rust safety contract is enforced under one irqsave lock. The serial map is
a non-owning index; each mapped live object has one or more references from a
task session slot, persistent-user pin, or containing-ring item. Ring items
are unique per parent, and cycle-forming ring links return `-EDEADLK`, so the
ownership graph is a DAG. Final release first removes the object's serial
entry, then iteratively drains its child references using an intrusive
worklist; teardown stack depth is constant. Allocation and payload copying
finish before publication. A failed link rolls back the new key's map entry,
while replacement swaps a fully allocated payload in one locked step. Joining
a replacement session publishes the new ring before dropping the task's old
reference, and the old ring stays serial-resolvable while inherited/member
references remain. Checked reference increments fail with `-ENOSPC`; lifecycle
hooks whose ABI is `void` leave the child slot unchanged if a fork reference
cannot be acquired. Raw pointers are confined to the locked core and the
mechanical bridge; syscall user pointers stay in the ABI adapter. The bridge
functions that access objects are called only while the Rust caller holds the
lock; credential reads and allocation/time primitives are leaf operations.
Direct `object_destroy` is used only before publication for empty objects or
after a candidate key has been removed from the map; published, populated
rings are reclaimed only by the iterative final-put path. Corrupt negative
reference states are not decremented again. `Item` and `Object` storage uses
the kernel slab bridge, whose alignment is sufficient for their C-compatible
layouts; Rust keeps the object layout private and the bridge never inspects it.

Each ring keeps up to eight detached `Item` nodes for reuse. Unlink clears the
child pointer and drops the child's reference before caching the node; linking
reinitializes a cached node before publishing it. Overflow nodes are freed,
and final ring destruction frees its cache. The pool is protected by the same
global lock and holds no object references.

Two intentional C-reference bug fixes accompany the port: ring cycles are
rejected rather than leaking through reference cycles, and a replaced session
ring remains indexed until its last reference disappears. The C reference is
also changed to iterative reclamation so deep acyclic teardown does not recurse
on the kernel stack. These are not presented as byte-for-byte compatibility
with the old C bugs.

## Validation and remaining limits

The module's host test exercises the actual Rust entry points with test-only
kernel primitive stubs. The independent test is
`kernel/ipc/keyring_host_test.sh`; it compiles into the P3 private build tree
through the repository build lease.

Guest validation invoked real Linux syscalls under QEMU SMP=2 with both C and
Rust selectors. It covered successful add/read/describe/
search, owner/capability denial, expiry, link/unlink, invalid and boundary
inputs, and resource release. The self-contained `/bin/keyring_rust_test`
acceptance scenario prints `KEYRING_RUST: PASS` only after all of these checks:
replacement retains the key serial; direct/nested search and read/describe
return expected values; an unprivileged child cannot change the parent's key;
expiry returns `EKEYEXPIRED`; duplicate/missing membership behavior and direct
and three-ring cycle rejection are correct; the old ring/key remains indexed
while an inherited child owns it and is reclaimed after the final unlink or
child exit; oversized/null/missing-destination inputs return the established
errors. The current `user_strncpy` contract silently truncates strings to
`max - 1` bytes and NUL-terminates them, so the guest checks that an overlong
key type is searchable under its truncated spelling, and uses a separate
invalid pointer to exercise `-EFAULT`. It does not present this helper behavior
as Linux's ideal long-name semantics. C and Rust guest correctness logs and
hashes are recorded in
`.git/parallel-rewrite/p23-finish-20261016/p3-keyring-guest-evidence.txt`.

The acceptance binary is correctness-only. A separate fixed-iteration
experiment measured add/update, direct search, read, and link/unlink on
identical guest binaries, alternating selector order over five paired QEMU
boots. The original logs remained outside the repository and are no longer
available here. The recorded summary reported wide timing variation and mixed
phase directions, so it did not establish an overall speedup; it is not part
of current validation.

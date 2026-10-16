//! Shared bounded controller for ext4 depth-0 extent-root truncation.
//!
//! The caller owns the filesystem allocation mutex for the complete call.
//! This module owns validation, free ordering, rollback, and the inode image
//! selected after a failed write. C callbacks are deliberately narrow: free
//! or reserve one physical block, write the current inode image, and poison a
//! journal transaction. The same source is linked into kernel ext4 and UFSD.
//!
//! `host_std` is set only by the P2 host harness
//! (kernel/fs/diskfs/tests/run-partial-trunc-host.sh) so this file can be
//! linked into a hosted C test binary; production builds never set it.
#![cfg_attr(not(any(test, host_std)), no_std)]

use core::ffi::c_void;

#[path = "ext4_mount_state.rs"]
mod mount_state;
#[path = "ext4_sync_plan.rs"]
mod sync_plan;

const EIO: i32 = -5;
const ENOMEM: i32 = -12;
const ENOSPC: i32 = -28;
const EOPNOTSUPP: i32 = -95;
const EINCONSISTENT: i32 = -117;
const EXT4_EXT_MAGIC: u16 = 0xf30a;
const EXT4_EXT_ROOT_MAX: usize = 4;
const EXT4_I_BLOCK_BYTES: usize = 60;
const EXTENT_HEADER_BYTES: usize = 12;
const EXTENT_BYTES: usize = 12;
/* Mirrors EXT4_MAX_FLATEXT: 4 depth-1 leaves of 512 entries each, the
 * flattened shape this driver itself can create. */
const EXT4_MAX_FLATEXT: usize = 4 * 512;

pub type BlockOp = unsafe extern "C" fn(*mut c_void, u64) -> i32;
pub type InodeWriteOp = unsafe extern "C" fn(*mut c_void, *mut u8) -> i32;
pub type PoisonOp = unsafe extern "C" fn(*mut c_void) -> i32;

#[repr(C)]
pub struct ExtentTxnOps {
    pub context: *mut c_void,
    pub free_block: Option<BlockOp>,
    pub reserve_block: Option<BlockOp>,
    pub write_inode: Option<InodeWriteOp>,
    pub poison_journal: Option<PoisonOp>,
    pub journaled: i32,
    pub first_data_block: u64,
    pub blocks_count: u64,
}

#[derive(Clone, Copy, Default)]
struct Extent {
    logical: u32,
    length: u32,
    physical: u64,
    unwritten: bool,
}

unsafe fn get_u16(b: *const u8, off: usize) -> u16 {
    u16::from_le_bytes([b.add(off).read(), b.add(off + 1).read()])
}

unsafe fn get_u32(b: *const u8, off: usize) -> u32 {
    u32::from_le_bytes([b.add(off).read(), b.add(off + 1).read(),
                        b.add(off + 2).read(), b.add(off + 3).read()])
}

unsafe fn put_u16(b: *mut u8, off: usize, v: u16) {
    let bytes = v.to_le_bytes();
    b.add(off).write(bytes[0]);
    b.add(off + 1).write(bytes[1]);
}

unsafe fn put_u32(b: *mut u8, off: usize, v: u32) {
    let bytes = v.to_le_bytes();
    b.add(off).write(bytes[0]);
    b.add(off + 1).write(bytes[1]);
    b.add(off + 2).write(bytes[2]);
    b.add(off + 3).write(bytes[3]);
}

unsafe fn call_block(op: Option<BlockOp>, context: *mut c_void, block: u64) -> i32 {
    match op {
        Some(f) => f(context, block),
        None => EINCONSISTENT,
    }
}

unsafe fn rollback_freed(ops: &ExtentTxnOps, extents: &[Extent; EXT4_EXT_ROOT_MAX],
                         keeps: &[u32; EXT4_EXT_ROOT_MAX],
                         freed: &mut [u32; EXT4_EXT_ROOT_MAX], count: usize) -> bool {
    let mut failed = false;
    for index in (0..count).rev() {
        let freed_slot = freed.as_mut_ptr().add(index);
        let keep = keeps.as_ptr().add(index).read();
        let extent = extents.as_ptr().add(index).read();
        while freed_slot.read() != 0 {
            freed_slot.write(freed_slot.read() - 1);
            let block = extent.physical + keep as u64 + freed_slot.read() as u64;
            if call_block(ops.reserve_block, ops.context, block) < 0 {
                failed = true;
            }
        }
    }
    failed
}

unsafe fn fatal_after_compensation(journaled: i32, poison: Option<PoisonOp>,
                                  context: *mut c_void, original_error: i32) -> i32 {
    if journaled != 0 {
        if let Some(poison) = poison {
            if poison(context) < 0 {
                return EINCONSISTENT;
            }
        } else {
            return EINCONSISTENT;
        }
    }
    original_error
}

/// Trim a depth-0 extent root at `first_kept_lblk`.
///
/// `i_block` points at the inode's 60-byte extent root. The caller holds
/// `alloc_lock` and supplies callbacks whose block operations assume that lock
/// is already held. The write callback receives a separate planned root image;
/// it must not read or mutate that buffer through another alias. A failing
/// free callback must either leave the block allocated or return `-117` when
/// it cannot establish that fact. Every failure is a negative errno.
#[no_mangle]
pub unsafe extern "C" fn a20_ext4_extent_root_trim_txn(
    ino: u32,
    i_block: *mut u8,
    first_kept_lblk: u32,
    ops: *const ExtentTxnOps,
) -> i32 {
    if ino == 0 || i_block.is_null() || ops.is_null() {
        return EINCONSISTENT;
    }
    let ops = &*ops;
    if ops.free_block.is_none() || ops.reserve_block.is_none() ||
       ops.write_inode.is_none() || ops.blocks_count == 0 {
        return EINCONSISTENT;
    }

    let mut original = [0u8; EXT4_I_BLOCK_BYTES];
    core::ptr::copy_nonoverlapping(i_block, original.as_mut_ptr(), EXT4_I_BLOCK_BYTES);
    let magic = get_u16(i_block, 0);
    let entries = get_u16(i_block, 2) as usize;
    let max = get_u16(i_block, 4) as usize;
    let depth = get_u16(i_block, 6);
    if magic != EXT4_EXT_MAGIC || depth != 0 || entries > EXT4_EXT_ROOT_MAX ||
       max != EXT4_EXT_ROOT_MAX {
        return EOPNOTSUPP;
    }

    let mut extents = [Extent::default(); EXT4_EXT_ROOT_MAX];
    let mut previous_end = 0u64;
    for i in 0..entries {
        let off = EXTENT_HEADER_BYTES + i * EXTENT_BYTES;
        let logical = get_u32(i_block, off);
        let encoded_len = get_u16(i_block, off + 4);
        let unwritten = encoded_len > 0x8000;
        let length = (if unwritten { encoded_len - 0x8000 } else { encoded_len }) as u32;
        let physical = get_u32(i_block, off + 8) as u64 |
                       ((get_u16(i_block, off + 6) as u64) << 32);
        let logical_end = match (logical as u64).checked_add(length as u64) {
            Some(end) => end,
            None => return EIO,
        };
        if length == 0 || logical_end > 0x1_0000_0000 ||
           (i != 0 && (logical as u64) < previous_end) ||
           physical < ops.first_data_block || physical >= ops.blocks_count ||
           length as u64 > ops.blocks_count - physical {
            return EIO;
        }
        previous_end = logical_end;
        for j in 0..i {
            let prev = extents.as_ptr().add(j).read();
            let prev_end = prev.physical + prev.length as u64;
            let this_end = physical + length as u64;
            if physical < prev_end && prev.physical < this_end {
                return EIO;
            }
        }
        extents.as_mut_ptr().add(i).write(Extent { logical, length, physical, unwritten });
    }

    let mut keeps = [0u32; EXT4_EXT_ROOT_MAX];
    let mut freed = [0u32; EXT4_EXT_ROOT_MAX];
    let mut kept_count = 0usize;
    let mut planned = original;
    let write = match ops.write_inode {
        Some(f) => f,
        None => return EINCONSISTENT,
    };
    for i in 0..entries {
        let extent = extents.as_ptr().add(i).read();
        let keep = if extent.logical < first_kept_lblk {
            let end = extent.logical as u64 + extent.length as u64;
            (end.min(first_kept_lblk as u64) - extent.logical as u64) as u32
        } else { 0 };
        keeps.as_mut_ptr().add(i).write(keep);
        if keep != 0 {
            let off = EXTENT_HEADER_BYTES + kept_count * EXTENT_BYTES;
            let src = EXTENT_HEADER_BYTES + i * EXTENT_BYTES;
            core::ptr::copy_nonoverlapping(original.as_ptr().add(src), planned.as_mut_ptr().add(off), EXTENT_BYTES);
            let len_with_flag = keep as u16 | if extent.unwritten { 0x8000 } else { 0 };
            put_u16(planned.as_mut_ptr(), off + 4, len_with_flag);
            kept_count += 1;
        }
        for offset in keep..extent.length {
            let result = call_block(ops.free_block, ops.context,
                                    extent.physical + offset as u64);
            if result < 0 {
                let rollback_failed = rollback_freed(ops, &extents, &keeps,
                                                      &mut freed, i + 1);
                if rollback_failed {
                    put_u16(planned.as_mut_ptr(), 2, kept_count as u16);
                    for j in EXTENT_HEADER_BYTES + kept_count * EXTENT_BYTES..EXT4_I_BLOCK_BYTES {
                        planned.as_mut_ptr().add(j).write(0);
                    }
                    let publish = write(ops.context, planned.as_mut_ptr());
                    if publish >= 0 { raw_copy_to(i_block, &planned); }
                    return fatal_after_compensation(ops.journaled, ops.poison_journal,
                                                    ops.context, EINCONSISTENT);
                }
                return fatal_after_compensation(ops.journaled, ops.poison_journal,
                                                ops.context, result);
            }
            let slot = freed.as_mut_ptr().add(i);
            slot.write(slot.read() + 1);
        }
    }

    put_u16(planned.as_mut_ptr(), 2, kept_count as u16);
    for j in EXTENT_HEADER_BYTES + kept_count * EXTENT_BYTES..EXT4_I_BLOCK_BYTES {
        planned.as_mut_ptr().add(j).write(0);
    }
    let write_result = write(ops.context, planned.as_mut_ptr());
    if write_result >= 0 {
        raw_copy_to(i_block, &planned);
        return 0;
    }

    if ops.journaled != 0 {
        return fatal_after_compensation(ops.journaled, ops.poison_journal,
                                        ops.context, write_result);
    }

    let rollback_failed = rollback_freed(ops, &extents, &keeps, &mut freed, entries);
    if !rollback_failed {
        let mut restore = original;
        let restore_result = write(ops.context, restore.as_mut_ptr());
        if restore_result < 0 {
            return EINCONSISTENT;
        }
        return write_result;
    }

    /* Some released blocks could not be made unavailable again. Keep only
     * extents for blocks we know were reserved in the original tree, publish
     * that safe subset if possible, and force the caller to quarantine. */
    let safe_write = write(ops.context, planned.as_mut_ptr());
    if safe_write >= 0 { raw_copy_to(i_block, &planned); }
    if safe_write < 0 { return EINCONSISTENT; }
    EINCONSISTENT
}

unsafe fn raw_copy_to(destination: *mut u8, image: &[u8; EXT4_I_BLOCK_BYTES]) {
    core::ptr::copy_nonoverlapping(image.as_ptr(), destination, EXT4_I_BLOCK_BYTES);
}

/* ================================================================
 * Partial truncate transaction (depth-0/1 extent trees)
 * ================================================================
 *
 * The legacy C partial truncate (`ext4_extent_truncate_at`) derived its
 * block-reclaim ranges from a working copy whose lengths had already been
 * shrunk for the surviving tree, so the tail of the extent spanning the new
 * EOF was never returned to the allocator: the blocks stayed marked
 * allocated in the group bitmap with no owner, a silent space leak.  It
 * also freed data blocks before the replacement tree image was published,
 * leaving a crash window where the still-published old tree referenced
 * blocks the allocator could already hand out again.
 *
 * This transaction owns the whole decision and side-effect sequence for a
 * partial truncate of the depth-0/1 trees this driver emits:
 *
 *   1. Validate the caller-collected flat extent map (sorted, no logical or
 *      physical overlap, in range) and the surviving-extent / reclaim-range
 *      decision, computed from the ORIGINAL lengths.
 *   2. Plan the replacement root image (depth-0, or depth-1 with staged
 *      leaf blocks) without touching the disk.
 *   3. Stage: allocate and write the leaf blocks the depth-1 plan needs.
 *      Staged leaves are referenced by nothing published, so a failure here
 *      only has to return them to the allocator; the old tree is intact.
 *   4. Publish: write the new root image.  Only after this point does any
 *      block the old tree referenced become unreachable, so reclaim starts
 *      here — never earlier.  A publish failure leaves the old tree fully
 *      backed and staged leaves are unwound.
 *   5. Reclaim: free the dropped data-block ranges one at a time, recording
 *      them for rollback.  A failing free rolls every earlier free back in
 *      reverse while alloc_lock is still held; if a rollback reservation
 *      fails, the published (safe, never-referencing-freed-blocks) image
 *      stands and the caller must quarantine the mount.
 *   6. Free the old tree's leaf blocks (depth-1 only); after publication
 *      nothing references them.  A failing free is compensated back while
 *      the lock is held, else the mount is quarantined.
 *
 * The caller owns alloc_lock for the complete call; C callbacks are the
 * narrow locked bitmap/descriptor/inode/tree-block primitives. */

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Ext4FlatExtent {
    pub start: u32,
    pub len: u32,
    pub phys: u64,
}

pub type AllocOp = unsafe extern "C" fn(*mut c_void) -> u64;
pub type TreeBlockWriteOp = unsafe extern "C" fn(*mut c_void, u64, *const u8) -> i32;
pub type PartialInodeWriteOp = unsafe extern "C" fn(*mut c_void, *mut u8, u64) -> i32;

#[repr(C)]
pub struct ExtentPartialOps {
    pub context: *mut c_void,
    pub alloc_block: Option<AllocOp>,
    pub free_block: Option<BlockOp>,
    pub reserve_block: Option<BlockOp>,
    pub write_inode: Option<PartialInodeWriteOp>,
    pub write_tree_block: Option<TreeBlockWriteOp>,
    pub poison_journal: Option<PoisonOp>,
    pub journaled: i32,
    pub first_data_block: u64,
    pub blocks_count: u64,
    pub block_size: u32,
    pub old_size: u64,
    pub new_size: u64,
    /* Caller-owned scratch of block_size bytes used to serialize one leaf
     * image at a time; never aliased with on-disk metadata. */
    pub tree_scratch: *mut u8,
    /* Caller-owned heap scratch. Each array has EXT4_MAX_FLATEXT entries.
     * This keeps the bounded transaction state off the kernel stack. */
    pub keeps_scratch: *mut u32,
    pub freed_scratch: *mut u32,
}

struct PartialPlan {
    keeps: *mut u32,
    kept_count: usize,
    /* Staged leaf physical blocks for a depth-1 plan. */
    leaves: [u64; EXT4_EXT_ROOT_MAX],
    leaf_count: usize,
    depth1: bool,
    planned: [u8; EXT4_I_BLOCK_BYTES],
}

unsafe fn partial_validate(ops: &ExtentPartialOps, flat: &[Ext4FlatExtent],
                           count: usize) -> bool {
    let mut previous_logical_end = 0u64;
    for i in 0..count {
        let e = flat.as_ptr().add(i).read();
        if e.len == 0 {
            return false;
        }
        let logical_end = match (e.start as u64).checked_add(e.len as u64) {
            Some(end) => end,
            None => return false,
        };
        if logical_end > 0x1_0000_0000 ||
           (i != 0 && (e.start as u64) < previous_logical_end) {
            return false;
        }
        previous_logical_end = logical_end;
        if e.phys < ops.first_data_block || e.phys >= ops.blocks_count ||
           e.len as u64 > ops.blocks_count - e.phys {
            return false;
        }
        for j in 0..i {
            let prev = flat.as_ptr().add(j).read();
            let prev_end = prev.phys + prev.len as u64;
            let this_end = e.phys + e.len as u64;
            if e.phys < prev_end && prev.phys < this_end {
                return false;
            }
        }
    }
    true
}

unsafe fn partial_plan(ops: &ExtentPartialOps, flat: &[Ext4FlatExtent],
                       count: usize, first_kept_lblk: u32,
                       plan: &mut PartialPlan) -> i32 {
    let mut kept_count = 0usize;
    for i in 0..count {
        let e = flat.as_ptr().add(i).read();
        let keep = if e.start < first_kept_lblk {
            let end = e.start as u64 + e.len as u64;
            (end.min(first_kept_lblk as u64) - e.start as u64) as u32
        } else {
            0
        };
        plan.keeps.add(i).write(keep);
        if keep != 0 {
            kept_count += 1;
        }
    }
    plan.kept_count = kept_count;

    let epb = if ops.block_size >= EXTENT_HEADER_BYTES as u32 + EXTENT_BYTES as u32 {
        (ops.block_size as usize - EXTENT_HEADER_BYTES) / EXTENT_BYTES
    } else {
        0
    };
    if epb == 0 {
        return EIO;
    }
    plan.depth1 = kept_count > EXT4_EXT_ROOT_MAX;
    if plan.depth1 {
        plan.leaf_count = (kept_count + epb - 1) / epb;
        if plan.leaf_count > EXT4_EXT_ROOT_MAX {
            return EOPNOTSUPP;
        }
    } else {
        plan.leaf_count = 0;
    }

    /* Root header: same magic/max, entries replaced below. */
    let mut planned = [0u8; EXT4_I_BLOCK_BYTES];
    put_u16(planned.as_mut_ptr(), 0, EXT4_EXT_MAGIC);
    put_u16(planned.as_mut_ptr(), 4, EXT4_EXT_ROOT_MAX as u16);
    put_u16(planned.as_mut_ptr(), 6, if plan.depth1 { 1 } else { 0 });

    if !plan.depth1 {
        put_u16(planned.as_mut_ptr(), 2, kept_count as u16);
        let mut slot = 0usize;
        for i in 0..count {
            let e = flat.as_ptr().add(i).read();
            let keep = plan.keeps.add(i).read();
            if keep == 0 {
                continue;
            }
            let off = EXTENT_HEADER_BYTES + slot * EXTENT_BYTES;
            put_u32(planned.as_mut_ptr(), off, e.start);
            put_u16(planned.as_mut_ptr(), off + 4, keep as u16);
            put_u16(planned.as_mut_ptr(), off + 6, (e.phys >> 32) as u16);
            put_u32(planned.as_mut_ptr(), off + 8, e.phys as u32);
            slot += 1;
        }
        plan.planned = planned;
        return 0;
    }

    /* Depth-1 plan: index entries now, leaf images written at stage time.
     * Leaf i holds kept extents [i*epb, min((i+1)*epb, kept_count)). */
    put_u16(planned.as_mut_ptr(), 2, plan.leaf_count as u16);
    let mut leaf = 0usize;
    let mut in_leaf = 0usize;
    for i in 0..count {
        let keep = plan.keeps.add(i).read();
        if keep == 0 {
            continue;
        }
        if in_leaf == 0 {
            let e = flat.as_ptr().add(i).read();
            let off = EXTENT_HEADER_BYTES + leaf * EXTENT_BYTES;
            /* Index entry: first logical key now; leaf number is patched in
             * after staging allocates it (ei_leaf_lo@+4, ei_leaf_hi@+8). */
            put_u32(planned.as_mut_ptr(), off, e.start);
            put_u32(planned.as_mut_ptr(), off + 4, 0);
            put_u16(planned.as_mut_ptr(), off + 8, 0);
        }
        in_leaf += 1;
        if in_leaf == epb {
            leaf += 1;
            in_leaf = 0;
        }
    }
    plan.planned = planned;
    0
}

/* Free every staged leaf; a failing free cannot be left half-owned by an
 * unpublished tree, so any failure is fatal for the mount state. */
unsafe fn partial_unwind_staged(ops: &ExtentPartialOps, plan: &PartialPlan) -> i32 {
    let mut ret = 0;
    for i in 0..plan.leaf_count {
        let block = plan.leaves.as_ptr().add(i).read();
        if block == 0 {
            continue;
        }
        if call_block(ops.free_block, ops.context, block) < 0 {
            ret = EINCONSISTENT;
        }
    }
    ret
}

/* Roll back completed data-block frees in reverse order while the caller's
 * alloc_lock still keeps every block unavailable to other allocators. */
unsafe fn partial_rollback_freed(ops: &ExtentPartialOps, flat: &[Ext4FlatExtent],
                                 plan: &PartialPlan, count: usize,
                                 freed: *mut u32) -> bool {
    let mut failed = false;
    for index in (0..count).rev() {
        let e = flat.as_ptr().add(index).read();
        let keep = plan.keeps.add(index).read();
        let slot = freed.add(index);
        while slot.read() != 0 {
            slot.write(slot.read() - 1);
            let block = e.phys + keep as u64 + slot.read() as u64;
            if call_block(ops.reserve_block, ops.context, block) < 0 {
                failed = true;
            }
        }
    }
    failed
}

/// Partial-truncate a depth-0/1 extent tree at `first_kept_lblk`.
///
/// `flat` is a read-only, caller-collected flattening of every extent the
/// current tree references (root entries for depth 0, leaf entries for
/// depth 1); `old_leaves` lists the leaf blocks the old root index
/// references (empty for depth 0). The caller holds the filesystem
/// allocation mutex for the complete call. On success the replacement root
/// image is published, every block at or beyond `first_kept_lblk` is back
/// with the allocator, and `i_block` holds the new image. Every failure is
/// a negative errno; `-117` means the mount must be quarantined.
#[no_mangle]
pub unsafe extern "C" fn a20_ext4_extent_partial_truncate_txn(
    ino: u32,
    i_block: *mut u8,
    flat: *const Ext4FlatExtent,
    flat_count: usize,
    old_leaves: *const u64,
    old_leaf_count: usize,
    first_kept_lblk: u32,
    ops: *const ExtentPartialOps,
) -> i32 {
    if ino == 0 || i_block.is_null() || flat.is_null() || ops.is_null() {
        return EINCONSISTENT;
    }
    if flat_count > EXT4_MAX_FLATEXT {
        return EIO;
    }
    let ops = &*ops;
    if ops.alloc_block.is_none() || ops.free_block.is_none() ||
       ops.reserve_block.is_none() || ops.write_inode.is_none() ||
       ops.write_tree_block.is_none() || ops.blocks_count == 0 ||
       ops.block_size == 0 || ops.tree_scratch.is_null() ||
       ops.keeps_scratch.is_null() || ops.freed_scratch.is_null() {
        return EINCONSISTENT;
    }
    if old_leaf_count > EXT4_EXT_ROOT_MAX {
        return EIO;
    }
    if old_leaf_count != 0 && old_leaves.is_null() {
        return EINCONSISTENT;
    }

    let magic = get_u16(i_block, 0);
    let root_entries = get_u16(i_block, 2) as usize;
    let root_max = get_u16(i_block, 4) as usize;
    let depth = get_u16(i_block, 6);
    if magic != EXT4_EXT_MAGIC || depth > 1 {
        return EOPNOTSUPP;
    }
    if root_entries > EXT4_EXT_ROOT_MAX || root_max != EXT4_EXT_ROOT_MAX ||
       (depth == 0 && old_leaf_count != 0) ||
       (depth == 0 && root_entries != flat_count) ||
       (depth == 1 && old_leaf_count != root_entries) {
        return EIO;
    }

    let flat_slice = core::slice::from_raw_parts(flat, flat_count);
    if !partial_validate(ops, flat_slice, flat_count) {
        return EIO;
    }
    for i in 0..old_leaf_count {
        let leaf = old_leaves.add(i).read();
        let index_off = EXTENT_HEADER_BYTES + i * EXTENT_BYTES;
        let indexed_leaf = get_u32(i_block, index_off + 4) as u64 |
                           ((get_u16(i_block, index_off + 8) as u64) << 32);
        if leaf == 0 || leaf != indexed_leaf || leaf < ops.first_data_block ||
           leaf >= ops.blocks_count {
            return EIO;
        }
        for j in 0..i {
            if old_leaves.add(j).read() == leaf {
                return EIO;
            }
        }
        for j in 0..flat_count {
            let e = flat_slice.as_ptr().add(j).read();
            if leaf >= e.phys && leaf < e.phys + e.len as u64 {
                return EIO;
            }
        }
    }
    let mut original = [0u8; EXT4_I_BLOCK_BYTES];
    core::ptr::copy_nonoverlapping(i_block, original.as_mut_ptr(), EXT4_I_BLOCK_BYTES);

    let write = match ops.write_inode {
        Some(f) => f,
        None => return EINCONSISTENT,
    };

    core::ptr::write_bytes(ops.freed_scratch, 0, EXT4_MAX_FLATEXT);
    let mut plan = PartialPlan {
        keeps: ops.keeps_scratch,
        kept_count: 0,
        leaves: [0; EXT4_EXT_ROOT_MAX],
        leaf_count: 0,
        depth1: false,
        planned: [0; EXT4_I_BLOCK_BYTES],
    };
    let planned_result = partial_plan(ops, flat_slice, flat_count,
                                      first_kept_lblk, &mut plan);
    if planned_result != 0 {
        return planned_result;
    }

    let epb = (ops.block_size as usize - EXTENT_HEADER_BYTES) / EXTENT_BYTES;

    /* ---- stage leaf blocks for a depth-1 plan ---- */
    if plan.depth1 {
        let alloc = match ops.alloc_block {
            Some(f) => f,
            None => return EINCONSISTENT,
        };
        let mut staged = 0usize;
        let mut staged_fail = false;
        for leaf in 0..plan.leaf_count {
            let phys = alloc(ops.context);
            if phys == 0 {
                staged_fail = true;
                break;
            }
            plan.leaves.as_mut_ptr().add(leaf).write(phys);
            staged += 1;
        }
        if staged_fail {
            plan.leaf_count = staged;
            let unwind = partial_unwind_staged(ops, &plan);
            return if unwind != 0 { unwind } else { ENOSPC };
        }
        /* Serialize and write each leaf image. */
        for leaf in 0..plan.leaf_count {
            let base = leaf * epb;
            let take = core::cmp::min(epb, plan.kept_count - base);
            core::ptr::write_bytes(ops.tree_scratch, 0, ops.block_size as usize);
            put_u16(ops.tree_scratch, 0, EXT4_EXT_MAGIC);
            put_u16(ops.tree_scratch, 2, take as u16);
            put_u16(ops.tree_scratch, 4, epb as u16);
            put_u16(ops.tree_scratch, 6, 0);
            let mut slot = 0usize;
            let mut seen = 0usize;
            for i in 0..flat_count {
                let keep = ops.keeps_scratch.add(i).read();
                if keep == 0 {
                    continue;
                }
                if seen >= base && seen < base + take {
                    let e = flat_slice.as_ptr().add(i).read();
                    let off = EXTENT_HEADER_BYTES + slot * EXTENT_BYTES;
                    put_u32(ops.tree_scratch, off, e.start);
                    put_u16(ops.tree_scratch, off + 4, keep as u16);
                    put_u16(ops.tree_scratch, off + 6, (e.phys >> 32) as u16);
                    put_u32(ops.tree_scratch, off + 8, e.phys as u32);
                    slot += 1;
                }
                seen += 1;
                if seen >= base + take {
                    break;
                }
            }
            let phys = plan.leaves.as_ptr().add(leaf).read();
            let wr = ops.write_tree_block.unwrap();
            if wr(ops.context, phys, ops.tree_scratch) < 0 {
                let unwind = partial_unwind_staged(ops, &plan);
                return if unwind != 0 { unwind } else { EIO };
            }
            /* Patch the root index entry with the staged leaf number. */
            let off = EXTENT_HEADER_BYTES + leaf * EXTENT_BYTES;
            put_u32(plan.planned.as_mut_ptr(), off + 4, phys as u32);
            put_u16(plan.planned.as_mut_ptr(), off + 8, (phys >> 32) as u16);
        }
    }

    /* ---- publish the replacement root; reclaim only after this ---- */
    if write(ops.context, plan.planned.as_mut_ptr(), ops.new_size) < 0 {
        /* An inode write may have reached storage before reporting an error.
         * Restore both the old tree and size before returning a retryable
         * error; failure to restore makes the mount state uncertain. */
        if write(ops.context, original.as_mut_ptr(), ops.old_size) < 0 {
            return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
        }
        raw_copy_to(i_block, &original);
        if plan.depth1 && partial_unwind_staged(ops, &plan) != 0 {
            return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
        }
        return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EIO);
    }

    /* ---- reclaim dropped data blocks ---- */
    for i in 0..flat_count {
        let e = flat_slice.as_ptr().add(i).read();
        let keep = ops.keeps_scratch.add(i).read();
        for offset in keep..e.len {
            let result = call_block(ops.free_block, ops.context,
                                    e.phys + offset as u64);
            if result < 0 {
                let rollback_failed = partial_rollback_freed(
                    ops, flat_slice, &plan, i + 1, ops.freed_scratch);
                if rollback_failed {
                    /* The published planned image never references freed
                     * blocks, so it stays as the safe subset. */
                    return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
                }
                /* Bitmap compensation restored every dropped block. Restore
                 * the old root before returning so the caller can retain the
                 * old size and retry without observing a half-commit. */
                if write(ops.context, original.as_mut_ptr(), ops.old_size) < 0 {
                    return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
                }
                raw_copy_to(i_block, &original);
                if plan.depth1 && partial_unwind_staged(ops, &plan) != 0 {
                    return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
                }
                /* No journal state remains in doubt: the failed allocator
                 * operation compensated internally, and this controller
                 * restored every earlier free and the old inode image. */
                return result;
            }
            let slot = ops.freed_scratch.add(i);
            slot.write(slot.read() + 1);
        }
    }

    /* ---- free the old tree's leaf blocks; nothing references them now ---- */
    let mut old_leaf_freed = [false; EXT4_EXT_ROOT_MAX];
    for i in 0..old_leaf_count {
        let block = old_leaves.add(i).read();
        if block == 0 {
            continue;
        }
        let result = call_block(ops.free_block, ops.context, block);
        if result < 0 {
            let mut compensation_failed = false;
            for previous in (0..i).rev() {
                if old_leaf_freed[previous] && call_block(
                    ops.reserve_block, ops.context, old_leaves.add(previous).read()) < 0 {
                    compensation_failed = true;
                }
            }
            if partial_rollback_freed(ops, flat_slice, &plan, flat_count,
                                      ops.freed_scratch) {
                compensation_failed = true;
            }
            if compensation_failed {
                return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
            }
            if write(ops.context, original.as_mut_ptr(), ops.old_size) < 0 {
                return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
            }
            raw_copy_to(i_block, &original);
            if plan.depth1 && partial_unwind_staged(ops, &plan) != 0 {
                return fatal_after_compensation(ops.journaled, ops.poison_journal, ops.context, EINCONSISTENT);
            }
            return result;
        }
        old_leaf_freed[i] = true;
    }

    raw_copy_to(i_block, &plan.planned);
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn test_put_u16(raw: &mut [u8], off: usize, value: u16) {
        unsafe { put_u16(raw.as_mut_ptr(), off, value); }
    }

    struct Fake {
        freed: Vec<u64>,
        reserved: Vec<u64>,
        write_count: usize,
        write_fail_at: Option<usize>,
        free_fail_at: Option<usize>,
        reserve_fail_at: Option<usize>,
        poison_count: usize,
    }

    impl Fake {
        fn new() -> Self {
            Self { freed: Vec::new(), reserved: Vec::new(), write_count: 0,
                   write_fail_at: None, free_fail_at: None,
                   reserve_fail_at: None, poison_count: 0 }
        }
    }

    unsafe fn fake<'a>(ctx: *mut c_void) -> &'a mut Fake { &mut *(ctx as *mut Fake) }

    unsafe extern "C" fn free_block(ctx: *mut c_void, block: u64) -> i32 {
        let f = fake(ctx);
        let n = f.freed.len() + 1;
        if f.free_fail_at == Some(n) { return EIO; }
        f.freed.push(block);
        0
    }

    unsafe extern "C" fn reserve_block(ctx: *mut c_void, block: u64) -> i32 {
        let f = fake(ctx);
        let n = f.reserved.len() + 1;
        if f.reserve_fail_at == Some(n) { return EIO; }
        f.reserved.push(block);
        0
    }

    unsafe extern "C" fn write_inode(ctx: *mut c_void, _raw: *mut u8) -> i32 {
        let f = fake(ctx);
        f.write_count += 1;
        if f.write_fail_at == Some(f.write_count) { EIO } else { 0 }
    }

    unsafe extern "C" fn poison(ctx: *mut c_void) -> i32 {
        fake(ctx).poison_count += 1;
        0
    }

    fn tree(physical: u32, length: u16) -> [u8; EXT4_I_BLOCK_BYTES] {
        let mut raw = [0u8; EXT4_I_BLOCK_BYTES];
        test_put_u16(&mut raw, 0, EXT4_EXT_MAGIC);
        test_put_u16(&mut raw, 2, 1);
        test_put_u16(&mut raw, 4, EXT4_EXT_ROOT_MAX as u16);
        test_put_u16(&mut raw, 6, 0);
        raw[EXTENT_HEADER_BYTES..EXTENT_HEADER_BYTES + EXTENT_BYTES].fill(0);
        raw[EXTENT_HEADER_BYTES..EXTENT_HEADER_BYTES + 4]
            .copy_from_slice(&0u32.to_le_bytes());
        test_put_u16(&mut raw, EXTENT_HEADER_BYTES + 4, length);
        raw[EXTENT_HEADER_BYTES + 8..EXTENT_HEADER_BYTES + 12]
            .copy_from_slice(&physical.to_le_bytes());
        raw
    }

    fn ops(fake: &mut Fake, journaled: bool) -> ExtentTxnOps {
        ExtentTxnOps { context: fake as *mut Fake as *mut c_void,
            free_block: Some(free_block), reserve_block: Some(reserve_block),
            write_inode: Some(write_inode), poison_journal: Some(poison),
            journaled: journaled as i32, first_data_block: 1, blocks_count: 32768 }
    }

    fn entries(raw: &[u8]) -> u16 { unsafe { get_u16(raw.as_ptr(), 2) } }
    fn first_len(raw: &[u8]) -> u16 { unsafe { get_u16(raw.as_ptr(), EXTENT_HEADER_BYTES + 4) } }

    #[test]
    fn partial_trim_releases_only_tail_and_publishes_prefix() {
        let mut f = Fake::new();
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 8);
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 3, &op) }, 0);
        assert_eq!(entries(&raw), 1);
        assert_eq!(first_len(&raw), 3);
        assert_eq!(f.freed, vec![2003, 2004, 2005, 2006, 2007]);
        assert!(f.reserved.is_empty());
    }

    #[test]
    fn free_failure_rolls_back_completed_blocks_in_reverse() {
        let mut f = Fake::new();
        f.free_fail_at = Some(3);
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 8);
        let original = raw;
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 3, &op) }, EIO);
        assert_eq!(raw, original);
        assert_eq!(f.reserved, vec![2004, 2003]);
    }

    #[test]
    fn failed_reservation_makes_state_fatal() {
        let mut f = Fake::new();
        f.free_fail_at = Some(3);
        f.reserve_fail_at = Some(1);
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 8);
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 3, &op) },
                   EINCONSISTENT);
        assert_eq!(entries(&raw), 1);
    }

    #[test]
    fn failed_inode_write_restores_blocks_and_original_root() {
        let mut f = Fake::new();
        f.write_fail_at = Some(1);
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 8);
        let original = raw;
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 3, &op) }, EIO);
        assert_eq!(raw, original);
        assert_eq!(f.reserved, vec![2007, 2006, 2005, 2004, 2003]);
        assert_eq!(f.write_count, 2);
    }

    #[test]
    fn failed_rollback_publishes_safe_prefix_then_returns_fatal() {
        let mut f = Fake::new();
        f.write_fail_at = Some(1);
        f.reserve_fail_at = Some(1);
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 8);
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 3, &op) },
                   EINCONSISTENT);
        assert_eq!(entries(&raw), 1);
        assert_eq!(first_len(&raw), 3);
        assert_eq!(f.write_count, 2);
    }

    #[test]
    fn journaled_inode_error_poison_is_propagated() {
        let mut f = Fake::new();
        f.write_fail_at = Some(1);
        let op = ops(&mut f, true);
        let mut raw = tree(2000, 8);
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 3, &op) }, EIO);
        assert_eq!(f.poison_count, 1);
        assert_eq!(f.reserved.len(), 0);
    }

    #[test]
    fn overlapping_physical_extents_reject_before_mutation() {
        let mut f = Fake::new();
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 8);
        test_put_u16(&mut raw, 2, 2);
        let second = EXTENT_HEADER_BYTES + EXTENT_BYTES;
        raw[second..second + 4].copy_from_slice(&8u32.to_le_bytes());
        test_put_u16(&mut raw, second + 4, 4);
        raw[second + 8..second + 12].copy_from_slice(&2004u32.to_le_bytes());
        assert_eq!(unsafe { a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 0, &op) }, EIO);
        assert!(f.freed.is_empty());
    }

    #[test]
    fn later_extent_free_failure_restores_complete_inode_root() {
        let mut f = Fake::new();
        f.free_fail_at = Some(4);
        let op = ops(&mut f, false);
        let mut raw = tree(2000, 3);
        test_put_u16(&mut raw, 2, 2);
        let second = EXTENT_HEADER_BYTES + EXTENT_BYTES;
        raw[second..second + 4].copy_from_slice(&4u32.to_le_bytes());
        test_put_u16(&mut raw, second + 4, 3);
        raw[second + 8..second + 12].copy_from_slice(&3000u32.to_le_bytes());
        let original = raw;

        assert_eq!(unsafe {
            a20_ext4_extent_root_trim_txn(14, raw.as_mut_ptr(), 0, &op)
        }, EIO);
        assert_eq!(raw, original, "failed later extent must not publish a partial root");
        assert_eq!(f.freed, vec![2000, 2001, 2002]);
        assert_eq!(f.reserved, vec![2002, 2001, 2000]);
        assert_eq!(f.write_count, 0);
    }

    /* ---- partial truncate transaction ---- */

    struct PFake {
        freed: Vec<u64>,
        reserved: Vec<u64>,
        allocated: Vec<u64>,
        tree_writes: Vec<u64>,
        events: Vec<(u8, u64)>, /* 1=alloc 2=tree-write 3=inode-write 4=free 5=reserve */
        write_sizes: Vec<u64>,
        write_count: usize,
        journaled: i32,
        write_fail_at: Option<usize>,
        free_fail_at: Option<usize>,
        reserve_fail_at: Option<usize>,
        alloc_fail_at: Option<usize>,
        tree_write_fail_at: Option<usize>,
        poison_count: usize,
        next_alloc: u64,
        scratch: Vec<u8>,
        txn_scratch: Vec<u32>,
        persisted_root: [u8; EXT4_I_BLOCK_BYTES],
        persisted_size: u64,
        write_fail_after_persist_at: Option<usize>,
    }

    impl PFake {
        fn new() -> Self {
            Self { freed: Vec::new(), reserved: Vec::new(), allocated: Vec::new(),
                   tree_writes: Vec::new(), events: Vec::new(),
                   write_sizes: Vec::new(),
                   write_count: 0, journaled: 0,
                   write_fail_at: None, free_fail_at: None,
                   reserve_fail_at: None, alloc_fail_at: None,
                   tree_write_fail_at: None, poison_count: 0,
                   next_alloc: 9000, scratch: vec![0u8; 4096],
                   txn_scratch: vec![0u32; EXT4_MAX_FLATEXT * 2],
                   persisted_root: [0; EXT4_I_BLOCK_BYTES], persisted_size: 8192,
                   write_fail_after_persist_at: None }
        }
        fn free_events(&self) -> Vec<u64> {
            self.events.iter().filter(|e| e.0 == 4).map(|e| e.1).collect()
        }
    }

    unsafe fn pfake<'a>(ctx: *mut c_void) -> &'a mut PFake { &mut *(ctx as *mut PFake) }

    unsafe extern "C" fn p_alloc(ctx: *mut c_void) -> u64 {
        let f = pfake(ctx);
        f.allocated.push(f.next_alloc);
        f.events.push((1, f.next_alloc));
        let n = f.allocated.len();
        if f.alloc_fail_at == Some(n) { return 0; }
        let b = f.next_alloc;
        f.next_alloc += 1;
        b
    }

    unsafe extern "C" fn p_free(ctx: *mut c_void, block: u64) -> i32 {
        let f = pfake(ctx);
        let n = f.freed.len() + 1;
        if f.free_fail_at == Some(n) { return EIO; }
        f.freed.push(block);
        f.events.push((4, block));
        0
    }

    unsafe extern "C" fn p_reserve(ctx: *mut c_void, block: u64) -> i32 {
        let f = pfake(ctx);
        let n = f.reserved.len() + 1;
        if f.reserve_fail_at == Some(n) { return EIO; }
        f.reserved.push(block);
        f.events.push((5, block));
        0
    }

    unsafe extern "C" fn p_write_inode(ctx: *mut c_void, raw: *mut u8,
                                         size: u64) -> i32 {
        let f = pfake(ctx);
        f.write_count += 1;
        f.write_sizes.push(size);
        f.events.push((3, f.write_count as u64));
        if f.write_fail_at == Some(f.write_count) { return EIO; }
        core::ptr::copy_nonoverlapping(raw, f.persisted_root.as_mut_ptr(),
                                       EXT4_I_BLOCK_BYTES);
        f.persisted_size = size;
        if f.write_fail_after_persist_at == Some(f.write_count) { EIO } else { 0 }
    }

    unsafe extern "C" fn p_write_tree(ctx: *mut c_void, phys: u64,
                                      _buf: *const u8) -> i32 {
        let f = pfake(ctx);
        f.tree_writes.push(phys);
        f.events.push((2, phys));
        let n = f.tree_writes.len();
        if f.tree_write_fail_at == Some(n) { EIO } else { 0 }
    }

    unsafe extern "C" fn p_poison(ctx: *mut c_void) -> i32 {
        pfake(ctx).poison_count += 1;
        0
    }

    fn pops(f: &mut PFake, block_size: u32) -> ExtentPartialOps {
        ExtentPartialOps { context: f as *mut PFake as *mut c_void,
            alloc_block: Some(p_alloc), free_block: Some(p_free),
            reserve_block: Some(p_reserve), write_inode: Some(p_write_inode),
            write_tree_block: Some(p_write_tree), poison_journal: Some(p_poison),
            journaled: f.journaled, first_data_block: 1, blocks_count: 32768,
            block_size, old_size: 8192, new_size: 4097,
            tree_scratch: f.scratch.as_mut_ptr(),
            keeps_scratch: f.txn_scratch.as_mut_ptr(),
            freed_scratch: f.txn_scratch.as_mut_ptr().wrapping_add(EXT4_MAX_FLATEXT) }
    }

    fn proot(entries: &[(u32, u16, u64)]) -> [u8; EXT4_I_BLOCK_BYTES] {
        let mut raw = [0u8; EXT4_I_BLOCK_BYTES];
        test_put_u16(&mut raw, 0, EXT4_EXT_MAGIC);
        test_put_u16(&mut raw, 2, entries.len() as u16);
        test_put_u16(&mut raw, 4, EXT4_EXT_ROOT_MAX as u16);
        test_put_u16(&mut raw, 6, 0);
        for (i, (start, len, phys)) in entries.iter().enumerate() {
            let off = EXTENT_HEADER_BYTES + i * EXTENT_BYTES;
            unsafe {
                put_u32(raw.as_mut_ptr(), off, *start);
                put_u16(raw.as_mut_ptr(), off + 4, *len);
                put_u16(raw.as_mut_ptr(), off + 6, (*phys >> 32) as u16);
                put_u32(raw.as_mut_ptr(), off + 8, *phys as u32);
            }
        }
        raw
    }

    fn pflat(entries: &[(u32, u32, u64)]) -> Vec<Ext4FlatExtent> {
        entries.iter().map(|(s, l, p)| Ext4FlatExtent {
            start: *s, len: *l, phys: *p }).collect()
    }

    fn ptxn_sized_bs(f: &mut PFake, raw: &mut [u8; EXT4_I_BLOCK_BYTES],
                     flat: &[(u32, u32, u64)], leaves: &[u64], lblk: u32,
                     block_size: u32, old_size: u64, new_size: u64) -> i32 {
        f.persisted_root = *raw;
        f.persisted_size = old_size;
        let v = pflat(flat);
        let mut op = pops(f, block_size);
        op.old_size = old_size;
        op.new_size = new_size;
        unsafe {
            a20_ext4_extent_partial_truncate_txn(14, raw.as_mut_ptr(),
                v.as_ptr(), v.len(), leaves.as_ptr(), leaves.len(), lblk, &op)
        }
    }

    fn ptxn_bs(f: &mut PFake, raw: &mut [u8; EXT4_I_BLOCK_BYTES],
               flat: &[(u32, u32, u64)], leaves: &[u64], lblk: u32,
               block_size: u32) -> i32 {
        ptxn_sized_bs(f, raw, flat, leaves, lblk, block_size, 8192, 4097)
    }

    fn ptxn(f: &mut PFake, raw: &mut [u8; EXT4_I_BLOCK_BYTES],
            flat: &[(u32, u32, u64)], leaves: &[u64], lblk: u32) -> i32 {
        ptxn_bs(f, raw, flat, leaves, lblk, 4096)
    }

    #[test]
    fn partial_missing_transaction_scratch_fails_before_mutation() {
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 8, 1000)]);
        let original = raw;
        let flat = pflat(&[(0, 8, 1000)]);
        let mut op = pops(&mut f, 4096);
        op.keeps_scratch = core::ptr::null_mut();
        assert_eq!(unsafe {
            a20_ext4_extent_partial_truncate_txn(14, raw.as_mut_ptr(),
                flat.as_ptr(), flat.len(), core::ptr::null(), 0, 3, &op)
        }, EINCONSISTENT);
        assert_eq!(raw, original);
        assert!(f.freed.is_empty() && f.reserved.is_empty() && f.allocated.is_empty());
        assert_eq!(f.write_count, 0);
    }

    /* Build the depth-1 old-tree root the C collector would see for a flat
     * map held in one leaf block: one index entry per leaf, depth 1. */
    fn pdepth1_root(leaves: &[u64], first_keys: &[u32]) -> [u8; EXT4_I_BLOCK_BYTES] {
        let mut raw = [0u8; EXT4_I_BLOCK_BYTES];
        test_put_u16(&mut raw, 0, EXT4_EXT_MAGIC);
        test_put_u16(&mut raw, 2, leaves.len() as u16);
        test_put_u16(&mut raw, 4, EXT4_EXT_ROOT_MAX as u16);
        test_put_u16(&mut raw, 6, 1);
        for (i, leaf) in leaves.iter().enumerate() {
            let off = EXTENT_HEADER_BYTES + i * EXTENT_BYTES;
            unsafe {
                put_u32(raw.as_mut_ptr(), off, first_keys[i]);
                put_u32(raw.as_mut_ptr(), off + 4, *leaf as u32);
                put_u16(raw.as_mut_ptr(), off + 8, (*leaf >> 32) as u16);
            }
        }
        raw
    }

    #[test]
    fn partial_spanning_extent_tail_is_reclaimed() {
        /* Regression: the C working copy was shrunk in place, so the tail of
         * the extent spanning the new EOF was never freed (bitmap leak). */
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 8, 1000)]);
        assert_eq!(ptxn(&mut f, &mut raw, &[(0, 8, 1000)], &[], 3), 0);
        assert_eq!(f.free_events(), vec![1003, 1004, 1005, 1006, 1007]);
        assert!(f.reserved.is_empty());
        assert_eq!(entries(&raw), 1);
        assert_eq!(first_len(&raw), 3);
        assert_eq!(f.write_count, 1);
        assert_eq!(f.persisted_size, 4097);
        assert_eq!(f.persisted_root, raw);
    }

    #[test]
    fn partial_multi_extent_drops_whole_extents_and_spanning_tail() {
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 4, 1000), (4, 4, 2000), (8, 4, 3000)]);
        assert_eq!(ptxn(&mut f, &mut raw,
                        &[(0, 4, 1000), (4, 4, 2000), (8, 4, 3000)], &[], 6), 0);
        assert_eq!(f.free_events(), vec![2002, 2003, 3000, 3001, 3002, 3003]);
        assert_eq!(entries(&raw), 2);
        assert_eq!(first_len(&raw), 4);
        unsafe {
            let second = EXTENT_HEADER_BYTES + EXTENT_BYTES;
            assert_eq!(get_u16(raw.as_ptr(), second + 4), 2);
            assert_eq!(get_u32(raw.as_ptr(), second + 8), 2000);
        }
    }

    #[test]
    fn partial_full_drop_publishes_empty_root() {
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 4, 1000)]);
        assert_eq!(ptxn(&mut f, &mut raw, &[(0, 4, 1000)], &[], 0), 0);
        assert_eq!(f.free_events(), vec![1000, 1001, 1002, 1003]);
        assert_eq!(entries(&raw), 0);
    }

    #[test]
    fn full_size_zero_truncate_removes_depth1_data_and_leaf() {
        let mut f = PFake::new();
        let mut raw = pdepth1_root(&[777], &[0]);
        assert_eq!(ptxn_sized_bs(&mut f, &mut raw, &[(0, 4, 1000)], &[777],
                                 0, 4096, 8192, 0), 0);
        assert_eq!(f.free_events(), vec![1000, 1001, 1002, 1003, 777]);
        assert_eq!(unsafe { get_u16(raw.as_ptr(), 6) }, 0);
        assert_eq!(entries(&raw), 0);
        assert_eq!(f.persisted_size, 0);
    }

    #[test]
    fn partial_boundary_keep_all_is_a_noop_republish() {
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 4, 1000)]);
        let original = raw;
        assert_eq!(ptxn(&mut f, &mut raw, &[(0, 4, 1000)], &[], 4), 0);
        assert!(f.free_events().is_empty());
        assert_eq!(raw, original);
    }

    #[test]
    fn partial_depth1_plan_stages_publishes_then_reclaims_and_drops_old_leaves() {
        /* Six surviving extents with epb=4 (60-byte tree blocks) need two
         * leaves; the old tree leaf 777 must be freed only after the new
         * root is published. */
        let flat: Vec<(u32, u32, u64)> = (0..6)
            .map(|i: u64| ((i * 4) as u32, 4, 1000 + i * 10)).collect();
        let mut f = PFake::new();
        let mut raw = pdepth1_root(&[777], &[0]);
        assert_eq!(ptxn_bs(&mut f, &mut raw, &flat, &[777], 20, 60), 0);
        assert_eq!(f.allocated, vec![9000, 9001]);
        assert_eq!(f.tree_writes, vec![9000, 9001]);
        assert_eq!(entries(&raw), 2);
        assert_eq!(unsafe { get_u16(raw.as_ptr(), 6) }, 1); /* depth 1 */
        unsafe {
            let idx0 = EXTENT_HEADER_BYTES + 4;
            let idx1 = EXTENT_HEADER_BYTES + EXTENT_BYTES + 4;
            assert_eq!(get_u32(raw.as_ptr(), idx0 as usize), 9000);
            assert_eq!(get_u32(raw.as_ptr(), idx1 as usize), 9001);
        }
        /* Publish (event 3) must precede every free (event 4), and the old
         * leaf 777 must be freed after all data frees. */
        let mut first_free = None;
        let mut old_leaf_pos = None;
        for (i, e) in f.events.iter().enumerate() {
            if e.0 == 4 && first_free.is_none() { first_free = Some(i); }
            if e.0 == 4 && e.1 == 777 { old_leaf_pos = Some(i); }
        }
        let publish_pos = f.events.iter().position(|e| e.0 == 3).unwrap();
        assert!(first_free.unwrap() > publish_pos);
        assert!(old_leaf_pos.unwrap() > publish_pos);
        assert_eq!(f.free_events().last(), Some(&777));
        assert!(f.reserved.is_empty());
        assert_eq!(f.write_count, 1);
    }

    #[test]
    fn partial_publish_failure_unwinds_staged_leaves_and_keeps_original() {
        let flat: Vec<(u32, u32, u64)> = (0..6)
            .map(|i: u64| ((i * 4) as u32, 4, 1000 + i * 10)).collect();
        let mut f = PFake::new();
        f.write_fail_at = Some(1);
        let mut raw = pdepth1_root(&[777], &[0]);
        let original = raw;
        assert_eq!(ptxn_bs(&mut f, &mut raw, &flat, &[777], 20, 60), EIO);
        assert_eq!(raw, original);
        assert_eq!(f.allocated, vec![9000, 9001]);
        assert_eq!(f.free_events(), vec![9000, 9001]); /* staged leaves unwound */
        assert!(f.reserved.is_empty());
        assert_eq!(f.write_count, 2, "failed publish restores the old inode image");
        assert_eq!(f.persisted_root, original);
        assert_eq!(f.persisted_size, 8192);
    }

    #[test]
    fn partial_write_that_reports_error_after_persist_is_rolled_back() {
        let mut f = PFake::new();
        f.write_fail_after_persist_at = Some(1);
        let mut raw = proot(&[(0, 8, 1000)]);
        let original = raw;
        assert_eq!(ptxn_sized_bs(&mut f, &mut raw, &[(0, 8, 1000)], &[],
                                 3, 4096, 16384, 4097), EIO);
        assert_eq!(raw, original);
        assert_eq!(f.persisted_root, original);
        assert_eq!(f.persisted_size, 16384);
        assert_eq!(f.write_sizes, vec![4097, 16384]);
        assert!(f.free_events().is_empty());
    }

    #[test]
    fn partial_stage_alloc_failure_returns_leaves_and_touches_nothing() {
        let flat: Vec<(u32, u32, u64)> = (0..6)
            .map(|i: u64| ((i * 4) as u32, 4, 1000 + i * 10)).collect();
        let mut f = PFake::new();
        f.alloc_fail_at = Some(2);
        let mut raw = pdepth1_root(&[777], &[0]);
        let original = raw;
        assert_eq!(ptxn_bs(&mut f, &mut raw, &flat, &[777], 20, 60), ENOSPC);
        assert_eq!(raw, original);
        assert_eq!(f.free_events(), vec![9000]); /* only the first staged leaf */
        assert_eq!(f.write_count, 0);
        assert!(f.tree_writes.is_empty());
    }

    #[test]
    fn partial_reclaim_failure_restores_old_root_after_reverse_rollback() {
        let mut f = PFake::new();
        f.journaled = 1;
        f.free_fail_at = Some(3);
        let mut raw = proot(&[(0, 4, 1000), (4, 4, 2000), (8, 4, 3000)]);
        let original = raw;
        assert_eq!(ptxn(&mut f, &mut raw,
                        &[(0, 4, 1000), (4, 4, 2000), (8, 4, 3000)], &[], 6),
                   EIO);
        assert_eq!(f.free_events(), vec![2002, 2003]);
        assert_eq!(f.reserved, vec![2003, 2002]); /* reverse rollback */
        assert_eq!(raw, original, "failed call leaves old tree for safe retry");
        assert_eq!(f.write_count, 2, "publish planned root then restore old root");
        assert_eq!(f.write_sizes, vec![4097, 8192]);
        assert_eq!(f.persisted_root, original);
        assert_eq!(f.persisted_size, 8192);
        assert_eq!(f.poison_count, 0, "fully compensated retry remains available");
        f.free_fail_at = None;
        assert_eq!(ptxn(&mut f, &mut raw,
                        &[(0, 4, 1000), (4, 4, 2000), (8, 4, 3000)], &[], 6), 0,
                   "retry starts from the restored tree");
        assert_eq!(entries(&raw), 2);
    }

    #[test]
    fn partial_reclaim_rollback_failure_is_fatal_but_safe_image_stands() {
        let mut f = PFake::new();
        f.free_fail_at = Some(2);
        f.reserve_fail_at = Some(1);
        let mut raw = proot(&[(0, 4, 1000), (4, 4, 2000)]);
        assert_eq!(ptxn(&mut f, &mut raw,
                        &[(0, 4, 1000), (4, 4, 2000)], &[], 6), EINCONSISTENT);
        assert_eq!(entries(&raw), 2); /* safe subset is on disk, tree view is quarantined */
        unsafe {
            let second = EXTENT_HEADER_BYTES + EXTENT_BYTES;
            assert_eq!(get_u16(f.persisted_root.as_ptr(), second + 4), 2);
        }
        assert_eq!(f.persisted_size, 4097);
    }

    #[test]
    fn partial_old_leaf_failure_restores_data_and_old_tree_for_retry() {
        let mut f = PFake::new();
        /* One data block is reclaimed, then the old leaf release fails. */
        f.free_fail_at = Some(2);
        let mut raw = pdepth1_root(&[777], &[0]);
        let original = raw;
        assert_eq!(ptxn_bs(&mut f, &mut raw, &[(0, 4, 1000)], &[777], 3, 4096), EIO);
        assert_eq!(raw, original);
        assert_eq!(f.reserved, vec![1003]);
        assert_eq!(f.write_count, 2);
    }

    #[test]
    fn partial_old_leaf_compensation_failure_is_fatal() {
        let mut f = PFake::new();
        /* Four dropped data blocks and the first old leaf are freed before
         * the second old-leaf release fails; the first compensation reserve
         * then fails, so the controller must quarantine the mount. */
        f.free_fail_at = Some(6);
        f.reserve_fail_at = Some(1);
        let mut raw = pdepth1_root(&[777, 778], &[0, 4]);
        let flat = [(0, 4, 1000), (4, 4, 2000)];
        assert_eq!(ptxn_bs(&mut f, &mut raw, &flat, &[777, 778], 4, 4096),
                   EINCONSISTENT);
    }

    #[test]
    fn partial_later_old_leaf_failure_reserves_prior_leaf_before_restore() {
        let mut f = PFake::new();
        f.free_fail_at = Some(6);
        let mut raw = pdepth1_root(&[777, 778], &[0, 4]);
        let original = raw;
        let flat = [(0, 2, 1000), (4, 2, 2000)];
        assert_eq!(ptxn_bs(&mut f, &mut raw, &flat, &[777, 778], 0, 4096), EIO);
        assert_eq!(raw, original);
        assert_eq!(f.reserved, vec![777, 2001, 2000, 1001, 1000]);
        assert_eq!(f.write_count, 2);
    }

    #[test]
    fn partial_unsorted_flat_map_rejected_before_side_effects() {
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 4, 1000)]);
        assert_eq!(ptxn(&mut f, &mut raw, &[(4, 4, 2000), (0, 4, 1000)], &[], 2),
                   EIO);
        assert!(f.free_events().is_empty());
        assert_eq!(f.write_count, 0);
    }

    #[test]
    fn partial_overlapping_physical_rejected_before_side_effects() {
        let mut f = PFake::new();
        let mut raw = proot(&[(0, 4, 1000)]);
        assert_eq!(ptxn(&mut f, &mut raw, &[(0, 4, 1000), (4, 4, 1002)], &[], 2),
                   EIO);
        assert!(f.free_events().is_empty());
    }
}

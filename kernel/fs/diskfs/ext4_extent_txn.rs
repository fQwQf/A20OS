//! Shared bounded controller for ext4 depth-0 extent-root truncation.
//!
//! The caller owns the filesystem allocation mutex for the complete call.
//! This module owns validation, free ordering, rollback, and the inode image
//! selected after a failed write. C callbacks are deliberately narrow: free
//! or reserve one physical block, write the current inode image, and poison a
//! journal transaction. The same source is linked into kernel ext4 and UFSD.
#![cfg_attr(not(test), no_std)]

use core::ffi::c_void;

#[path = "ext4_mount_state.rs"]
mod mount_state;

const EIO: i32 = -5;
const EOPNOTSUPP: i32 = -95;
const EINCONSISTENT: i32 = -117;
const EXT4_EXT_MAGIC: u16 = 0xf30a;
const EXT4_EXT_ROOT_MAX: usize = 4;
const EXT4_I_BLOCK_BYTES: usize = 60;
const EXTENT_HEADER_BYTES: usize = 12;
const EXTENT_BYTES: usize = 12;

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

unsafe fn fatal_after_compensation(ops: &ExtentTxnOps, original_error: i32) -> i32 {
    if ops.journaled != 0 {
        if let Some(poison) = ops.poison_journal {
            if poison(ops.context) < 0 {
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
                    return fatal_after_compensation(ops, EINCONSISTENT);
                }
                return fatal_after_compensation(ops, result);
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
        return fatal_after_compensation(ops, write_result);
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
}

//! Checked, bounded planner for ext4 file-scoped sync.
//!
//! Rust owns extent-header validation, checked byte/page ranges, cross-group
//! enumeration and page sorting/deduplication. The caller supplies the one
//! block scratch buffer and narrow callbacks for reading an extent leaf and
//! reading already-loaded group descriptor metadata.
#![cfg_attr(not(test), no_std)]

use core::ffi::c_void;

const EXT4_EXT_MAGIC: u16 = 0xf30a;
const EXT4_EXT_ROOT_MAX: usize = 4;
const EXT4_EXTENT_HEADER_BYTES: usize = 12;
const EXT4_EXTENT_BYTES: usize = 12;
const EIO: i32 = -5;
const EOVERFLOW: i32 = -75;
const FALLBACK: i32 = 1;
const PAGE_SIZE: u64 = 4096;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SyncGroupMeta {
    pub block_bitmap: u64,
    pub inode_bitmap: u64,
    pub inode_table: u64,
    pub descriptor_byte: u64,
}

#[repr(C)]
pub struct SyncPlanInput {
    pub block_size: u32,
    pub blocks_per_group: u32,
    pub groups_count: u32,
    pub inodes_per_group: u32,
    pub inode_size: u32,
    pub inode_number: u32,
    pub first_data_block: u64,
    pub blocks_count: u64,
    pub descriptor_table_byte: u64,
    pub descriptor_size: u32,
    pub leaf_capacity: u32,
    pub has_extents: u32,
    /// File-scoped sync is unsafe for an unjournaled mount until dirty
    /// metadata dependencies are tracked below the cache-page granularity.
    /// Such mounts must use the checked full-cache path instead.
    /// The active cache profile has a real journal sync hook. This is
    /// deliberately stronger than ext4 having a journal superblock pointer.
    pub has_journal_sync: u32,
}

/// Read callback: zero means success; positive means safe full-sync fallback;
/// negative is a read error and must propagate to fsync.
pub type ReadBlockOp = unsafe extern "C" fn(*mut c_void, u64, *mut u8, u32) -> i32;
/// Group callback: zero means success, positive requests safe full-sync
/// fallback, and negative is a real errno that must propagate to fsync.
pub type GroupMetaOp = unsafe extern "C" fn(*mut c_void, u32, *mut SyncGroupMeta) -> i32;
/// Flush the sorted page set produced by the planner. The callback owns the
/// filesystem/cache I/O; Rust owns the choice to use this or full sync.
pub type SyncScopedOp = unsafe extern "C" fn(*mut c_void, *const u64, usize) -> i32;
/// Checked full-cache fallback for plans that cannot safely be scoped.
pub type SyncFullOp = unsafe extern "C" fn(*mut c_void) -> i32;

#[repr(C)]
pub struct SyncPlanOps {
    pub context: *mut c_void,
    pub workspace: *mut u8,
    pub read_block: Option<ReadBlockOp>,
    pub group_meta: Option<GroupMetaOp>,
    pub sync_scoped: Option<SyncScopedOp>,
    pub sync_full: Option<SyncFullOp>,
}

// Keep the repr(C) contract pinned for both ILP32 and LP64 kernel targets.
const _: () = {
    assert!(core::mem::offset_of!(SyncGroupMeta, descriptor_byte) == 24);
    assert!(core::mem::size_of::<SyncGroupMeta>() == 32);
    assert!(core::mem::offset_of!(SyncPlanInput, first_data_block) == 24);
    assert!(core::mem::offset_of!(SyncPlanInput, descriptor_table_byte) == 40);
    assert!(core::mem::offset_of!(SyncPlanInput, has_journal_sync) == 60);
    assert!(core::mem::size_of::<SyncPlanInput>() == 64);
    assert!(core::mem::offset_of!(SyncPlanOps, workspace) == core::mem::size_of::<usize>());
    assert!(core::mem::offset_of!(SyncPlanOps, read_block) == 2 * core::mem::size_of::<usize>());
    assert!(core::mem::offset_of!(SyncPlanOps, group_meta) == 3 * core::mem::size_of::<usize>());
    assert!(core::mem::offset_of!(SyncPlanOps, sync_scoped) == 4 * core::mem::size_of::<usize>());
    assert!(core::mem::offset_of!(SyncPlanOps, sync_full) == 5 * core::mem::size_of::<usize>());
    assert!(core::mem::size_of::<SyncPlanOps>() == 6 * core::mem::size_of::<usize>());
};

#[derive(Clone, Copy, Default)]
struct Extent {
    logical: u32,
    length: u32,
    physical: u64,
}

struct Planner<'a> {
    input: &'a SyncPlanInput,
    ops: &'a SyncPlanOps,
    pages: &'a mut [u64],
    count: usize,
    previous_logical_end: u64,
}

impl Planner<'_> {
    fn add_byte_range(&mut self, start: u64, length: u64) -> Result<(), i32> {
        if length == 0 { return Ok(()); }
        let end = start.checked_add(length).ok_or(EOVERFLOW)?;
        let volume_bytes = self.input.blocks_count
            .checked_mul(self.input.block_size as u64).ok_or(EOVERFLOW)?;
        if end > volume_bytes { return Err(EIO); }
        let first = start / PAGE_SIZE;
        let last = (end - 1) / PAGE_SIZE;
        let mut page = first;
        loop {
            if self.count == self.pages.len() { return Err(FALLBACK); }
            // SAFETY: count is checked against the caller-provided capacity.
            unsafe { *self.pages.get_unchecked_mut(self.count) = page; }
            self.count += 1;
            if page == last { break; }
            page = page.checked_add(1).ok_or(EOVERFLOW)?;
        }
        Ok(())
    }

    fn add_blocks(&mut self, first: u64, count: u64, require_data: bool) -> Result<(), i32> {
        if count == 0 { return Ok(()); }
        let end = first.checked_add(count).ok_or(EOVERFLOW)?;
        if first < self.input.first_data_block || end > self.input.blocks_count {
            return Err(EIO);
        }
        let start_byte = first.checked_mul(self.input.block_size as u64).ok_or(EOVERFLOW)?;
        let byte_len = count.checked_mul(self.input.block_size as u64).ok_or(EOVERFLOW)?;
        self.add_byte_range(start_byte, byte_len)?;
        if require_data {
            let bpg = self.input.blocks_per_group as u64;
            if bpg == 0 { return Err(EIO); }
            let first_group = (first - self.input.first_data_block) / bpg;
            let last_group = (end - 1 - self.input.first_data_block) / bpg;
            let mut group = first_group;
            loop {
                self.add_group(group, 0)?;
                if group == last_group { break; }
                group = group.checked_add(1).ok_or(EOVERFLOW)?;
            }
        }
        Ok(())
    }

    fn add_group(&mut self, group: u64, inode_number: u32) -> Result<(), i32> {
        if group >= self.input.groups_count as u64 { return Err(EIO); }
        let callback = self.ops.group_meta.ok_or(EIO)?;
        let mut meta = SyncGroupMeta::default();
        let status = unsafe { callback(self.ops.context, group as u32, &mut meta) };
        // Positive callback status means metadata is unavailable but a full
        // sync remains safe. Negative status is a real errno/corruption and
        // must not be silently converted into a successful fallback.
        if status > 0 { return Err(FALLBACK); }
        if status < 0 { return Err(status); }
        let table_bytes = (self.input.inodes_per_group as u64)
            .checked_mul(self.input.inode_size as u64).ok_or(EOVERFLOW)?;
        let desc_bytes = self.input.descriptor_size as u64;
        let expected_desc = self.input.descriptor_table_byte
            .checked_add(group.checked_mul(desc_bytes).ok_or(EOVERFLOW)?)
            .ok_or(EOVERFLOW)?;
        let block_size = self.input.block_size as u64;
        if desc_bytes == 0 || block_size == 0 || meta.descriptor_byte != expected_desc {
            return Err(EIO);
        }
        let rounded = table_bytes.checked_add(block_size - 1).ok_or(EOVERFLOW)?;
        let table_blocks = rounded / block_size;
        self.add_blocks(meta.block_bitmap, 1, false)?;
        self.add_blocks(meta.inode_bitmap, 1, false)?;
        let table_end = meta.inode_table.checked_add(table_blocks).ok_or(EOVERFLOW)?;
        if meta.inode_table < self.input.first_data_block ||
           table_end > self.input.blocks_count { return Err(EIO); }
        if inode_number != 0 {
            let inode_index = ((inode_number - 1) % self.input.inodes_per_group) as u64;
            let inode_offset = inode_index.checked_mul(self.input.inode_size as u64)
                .ok_or(EOVERFLOW)?;
            let inode_byte = meta.inode_table.checked_mul(block_size)
                .and_then(|v| v.checked_add(inode_offset)).ok_or(EOVERFLOW)?;
            // Select only this inode record. The 4 KiB page rounding is
            // intentional; unjournaled filesystems fall back to full sync
            // because adjacent inode records may share that cache page.
            if inode_offset.checked_add(self.input.inode_size as u64)
                .ok_or(EOVERFLOW)? > table_bytes { return Err(EIO); }
            self.add_byte_range(inode_byte, self.input.inode_size as u64)?;
        }
        self.add_byte_range(meta.descriptor_byte, desc_bytes)?;
        Ok(())
    }

    fn add_extent(&mut self, extent: Extent) -> Result<(), i32> {
        if extent.length == 0 { return Err(EIO); }
        let logical_end = (extent.logical as u64)
            .checked_add(extent.length as u64).ok_or(EOVERFLOW)?;
        if logical_end > (u32::MAX as u64) + 1 { return Err(EIO); }
        if (extent.logical as u64) < self.previous_logical_end {
            return Err(EIO);
        }
        self.previous_logical_end = logical_end;
        self.add_blocks(extent.physical, extent.length as u64, true)
    }
}

unsafe fn get_u16(bytes: *const u8, offset: usize) -> u16 {
    u16::from_le_bytes([bytes.add(offset).read(), bytes.add(offset + 1).read()])
}

unsafe fn get_u32(bytes: *const u8, offset: usize) -> u32 {
    u32::from_le_bytes([bytes.add(offset).read(), bytes.add(offset + 1).read(),
                        bytes.add(offset + 2).read(), bytes.add(offset + 3).read()])
}

unsafe fn extent_at(bytes: *const u8, offset: usize, blocks_count: u64,
                    first_data_block: u64) -> Result<Extent, i32> {
    let logical = get_u32(bytes, offset);
    let encoded = get_u16(bytes, offset + 4);
    let length = if encoded > 0x8000 { encoded - 0x8000 } else { encoded };
    let physical = get_u32(bytes, offset + 8) as u64 |
                   ((get_u16(bytes, offset + 6) as u64) << 32);
    let end = physical.checked_add(length as u64).ok_or(EOVERFLOW)?;
    if length == 0 || physical < first_data_block || end > blocks_count {
        return Err(EIO);
    }
    Ok(Extent { logical, length: length as u32, physical })
}

unsafe fn parse_leaf(planner: &mut Planner<'_>, block: u64,
                     lower: u32, upper: Option<u32>) -> Result<(), i32> {
    let input = planner.input;
    let read = planner.ops.read_block.ok_or(FALLBACK)?;
    if planner.ops.workspace.is_null() { return Err(FALLBACK); }
    if input.block_size < (EXT4_EXTENT_HEADER_BYTES + EXT4_EXTENT_BYTES) as u32 {
        return Err(EIO);
    }
    let status = read(planner.ops.context, block, planner.ops.workspace, input.block_size);
    if status > 0 { return Err(FALLBACK); }
    if status < 0 { return Err(status); }
    let bytes = planner.ops.workspace as *const u8;
    let magic = get_u16(bytes, 0);
    let entries = get_u16(bytes, 2) as usize;
    let max = get_u16(bytes, 4) as usize;
    let depth = get_u16(bytes, 6);
    let capacity = input.leaf_capacity as usize;
    if magic != EXT4_EXT_MAGIC || depth != 0 || max == 0 || max > capacity ||
       entries == 0 || entries > max ||
       EXT4_EXTENT_HEADER_BYTES.checked_add(max.checked_mul(EXT4_EXTENT_BYTES)
           .ok_or(EOVERFLOW)?).ok_or(EOVERFLOW)? > input.block_size as usize {
        return Err(EIO);
    }
    for i in 0..entries {
        let offset = EXT4_EXTENT_HEADER_BYTES + i * EXT4_EXTENT_BYTES;
        let extent = extent_at(bytes, offset, input.blocks_count, input.first_data_block)?;
        if extent.logical < lower || upper.map_or(false, |limit| extent.logical >= limit) {
            return Err(EIO);
        }
        let logical_end = (extent.logical as u64)
            .checked_add(extent.length as u64).ok_or(EOVERFLOW)?;
        if upper.map_or(false, |limit| logical_end > limit as u64) {
            return Err(EIO);
        }
        planner.add_extent(extent)?;
    }
    Ok(())
}

unsafe fn build_plan(input: &SyncPlanInput, ops: &SyncPlanOps,
                     pages: &mut [u64]) -> Result<usize, i32> {
    if input.block_size < 60 ||
       input.blocks_count == 0 || input.blocks_per_group == 0 ||
       input.groups_count == 0 || input.inodes_per_group == 0 ||
       input.inode_size == 0 || input.inode_number == 0 ||
       input.descriptor_size == 0 || pages.is_empty() {
        return Err(EIO);
    }
    if input.has_extents == 0 { return Err(FALLBACK); }
    // A page selected for this inode can also contain neighbouring inode
    // records. Flushing that page while leaving their directory/data blocks
    // dirty creates allocated, visible inodes with missing contents. Without
    // a journal to recover the dependency set, use the complete checked sync.
    if input.has_journal_sync == 0 { return Err(FALLBACK); }
    if ops.group_meta.is_none() { return Err(FALLBACK); }
    let mut planner = Planner { input, ops, pages, count: 0, previous_logical_end: 0 };
    let inode_group = ((input.inode_number - 1) / input.inodes_per_group) as u64;
    if inode_group >= input.groups_count as u64 { return Err(EIO); }
    planner.add_group(inode_group, input.inode_number)?;

    let workspace = ops.workspace;
    if workspace.is_null() { return Err(FALLBACK); }
    // Preserve the complete on-inode root before callbacks reuse workspace
    // for leaf blocks. The inode root is exactly 60 bytes.
    let mut root = [0u8; 60];
    core::ptr::copy_nonoverlapping(workspace, root.as_mut_ptr(), root.len());
    let root_ptr = root.as_ptr();
    let header_magic = get_u16(root_ptr, 0);
    if header_magic != EXT4_EXT_MAGIC { return Err(EIO); }
    let entries = get_u16(root_ptr, 2) as usize;
    let max = get_u16(root_ptr, 4) as usize;
    let depth = get_u16(root_ptr, 6);
    if max == 0 || max > EXT4_EXT_ROOT_MAX || entries > max { return Err(EIO); }
    if EXT4_EXTENT_HEADER_BYTES.checked_add(max.checked_mul(EXT4_EXTENT_BYTES)
        .ok_or(EOVERFLOW)?).ok_or(EOVERFLOW)? > 60 { return Err(EIO); }
    if depth > 1 { return Err(FALLBACK); }
    if depth == 0 {
        for i in 0..entries {
            let extent = extent_at(root_ptr, EXT4_EXTENT_HEADER_BYTES + i * EXT4_EXTENT_BYTES,
                                   input.blocks_count, input.first_data_block)?;
            planner.add_extent(extent)?;
        }
    } else {
        for i in 0..entries {
            let off = EXT4_EXTENT_HEADER_BYTES + i * EXT4_EXTENT_BYTES;
            let logical = get_u32(root_ptr, off);
            let next_logical = if i + 1 < entries {
                let next_off = EXT4_EXTENT_HEADER_BYTES + (i + 1) * EXT4_EXTENT_BYTES;
                Some(get_u32(root_ptr, next_off))
            } else { None };
            if let Some(next) = next_logical {
                if logical >= next { return Err(EIO); }
            }
            let leaf = get_u32(root_ptr, off + 4) as u64 |
                       ((get_u16(root_ptr, off + 8) as u64) << 32);
            planner.add_blocks(leaf, 1, false)?;
            parse_leaf(&mut planner, leaf, logical, next_logical)?;
        }
    }
    planner.add_byte_range(0, input.block_size as u64)?;
    let mut count = planner.count;
    drop(planner);

    for i in 1..count {
        let key = *pages.get_unchecked(i);
        let mut j = i;
        while j > 0 && *pages.get_unchecked(j - 1) > key {
            let prev = *pages.get_unchecked(j - 1);
            *pages.get_unchecked_mut(j) = prev;
            j -= 1;
        }
        *pages.get_unchecked_mut(j) = key;
    }
    let mut unique = 0usize;
    for i in 0..count {
        let current = *pages.get_unchecked(i);
        if unique == 0 || current != *pages.get_unchecked(unique - 1) {
            *pages.get_unchecked_mut(unique) = current;
            unique += 1;
        }
    }
    count = unique;
    Ok(count)
}

/// FFI safety: all pointers must be valid for their documented extents and
/// mutually non-aliasing. `pages` must point to `capacity` initialized u64s;
/// `out_count` is writable. `workspace` points to at least `input.block_size`
/// writable bytes and may be overwritten by callbacks; callbacks must not
/// mutate `input`, `pages`, or `out_count`. Callback context remains valid for
/// the call. The function preserves the first 60 workspace bytes before reads.
/// Return 0 for a complete sorted plan, 1 for a safe full-sync fallback,
/// and a negative errno for malformed geometry or extent metadata.
#[no_mangle]
pub unsafe extern "C" fn a20_ext4_sync_plan(
    input: *const SyncPlanInput,
    ops: *const SyncPlanOps,
    pages: *mut u64,
    capacity: usize,
    out_count: *mut usize,
) -> i32 {
    if input.is_null() || ops.is_null() || pages.is_null() || out_count.is_null() {
        return EIO;
    }
    out_count.write(0);
    if capacity > (isize::MAX as usize) / core::mem::size_of::<u64>() {
        return EIO;
    }
    if capacity == 0 { return FALLBACK; }
    let input = &*input;
    let ops = &*ops;
    let page_slice = core::slice::from_raw_parts_mut(pages, capacity);
    match build_plan(input, ops, page_slice) {
        Ok(count) => { out_count.write(count); 0 }
        Err(status) => status,
    }
}

/// Build a plan and execute the policy selected by that plan. FALLBACK
/// requests a checked full sync; negative status is returned unchanged and
/// performs no flush. Any other positive status is treated as an invalid
/// planner result and maps to EIO. The callbacks retain all block-cache and
/// filesystem I/O in C, while the Rust planner owns the production dispatch.
///
/// # Safety
/// The pointer requirements are the same as `a20_ext4_sync_plan`: input and
/// ops are valid, pages spans `capacity` initialized u64s, out_count is
/// writable, and callbacks must not mutate the input, pages, or out_count.
/// The callback context must remain valid for the call.
#[no_mangle]
pub unsafe extern "C" fn a20_ext4_sync_run(
    input: *const SyncPlanInput,
    ops: *const SyncPlanOps,
    pages: *mut u64,
    capacity: usize,
    out_count: *mut usize,
) -> i32 {
    if input.is_null() || ops.is_null() || pages.is_null() || out_count.is_null() {
        return EIO;
    }
    let ops = &*ops;
    let status = a20_ext4_sync_plan(input, ops, pages, capacity, out_count);
    dispatch_plan_result(status, ops, pages, out_count)
}

unsafe fn dispatch_plan_result(status: i32, ops: &SyncPlanOps,
                               pages: *const u64, out_count: *const usize) -> i32 {
    match status {
        0 => match ops.sync_scoped {
            Some(sync) => sync(ops.context, pages, out_count.read()),
            None => EIO,
        },
        FALLBACK => match ops.sync_full {
            Some(sync) => sync(ops.context),
            None => EIO,
        },
        error if error < 0 => error,
        _ => EIO,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::{BTreeMap, BTreeSet};

    struct Fake {
        block_size: u32,
        leaves: BTreeMap<u64, Vec<u8>>,
        groups_read: BTreeSet<u32>,
        fail_leaf: bool,
        read_status: i32,
        group_status: i32,
        read_blocks: Vec<u64>,
        scoped_calls: usize,
        full_calls: usize,
        flush_status: i32,
    }

    unsafe extern "C" fn read_block(context: *mut c_void, block: u64,
                                     out: *mut u8, len: u32) -> i32 {
        let fake = &mut *(context as *mut Fake);
        fake.read_blocks.push(block);
        let Some(leaf) = fake.leaves.get(&block) else { return EIO; };
        if fake.fail_leaf { return fake.read_status; }
        if len != fake.block_size { return EIO; }
        core::ptr::copy_nonoverlapping(leaf.as_ptr(), out, len as usize);
        0
    }

    unsafe extern "C" fn group_meta(context: *mut c_void, group: u32,
                                     out: *mut SyncGroupMeta) -> i32 {
        let fake = &mut *(context as *mut Fake);
        fake.groups_read.insert(group);
        if fake.group_status != 0 { return fake.group_status; }
        out.write(SyncGroupMeta {
            block_bitmap: group as u64 * 128 + 2,
            inode_bitmap: group as u64 * 128 + 3,
            inode_table: group as u64 * 128 + 4,
            descriptor_byte: 2048 + group as u64 * 64,
        });
        0
    }

    unsafe extern "C" fn sync_scoped(context: *mut c_void, pages: *const u64,
                                      count: usize) -> i32 {
        let fake = &mut *(context as *mut Fake);
        fake.scoped_calls += 1;
        if count == 0 { return EIO; }
        let slice = core::slice::from_raw_parts(pages, count);
        if !slice.windows(2).all(|pair| pair[0] < pair[1]) { return EIO; }
        fake.flush_status
    }

    unsafe extern "C" fn sync_full(context: *mut c_void) -> i32 {
        let fake = &mut *(context as *mut Fake);
        fake.full_calls += 1;
        fake.flush_status
    }

    fn put16(buf: &mut [u8], off: usize, v: u16) { buf[off..off + 2].copy_from_slice(&v.to_le_bytes()); }
    fn put32(buf: &mut [u8], off: usize, v: u32) { buf[off..off + 4].copy_from_slice(&v.to_le_bytes()); }
    fn header(buf: &mut [u8], depth: u16, entries: u16, max: u16) {
        put16(buf, 0, EXT4_EXT_MAGIC); put16(buf, 2, entries);
        put16(buf, 4, max); put16(buf, 6, depth);
    }
    fn root_extent(root: &mut [u8], logical: u32, len: u16, physical: u64) {
        put32(root, 12, logical); put16(root, 16, len);
        put16(root, 18, (physical >> 32) as u16); put32(root, 20, physical as u32);
    }
    fn root_index(root: &mut [u8], slot: usize, logical: u32, leaf: u64) {
        let off = 12 + slot * 12;
        put32(root, off, logical);
        put32(root, off + 4, leaf as u32);
        put16(root, off + 8, (leaf >> 32) as u16);
    }
    fn leaf_extent(leaf: &mut [u8], slot: usize, logical: u32, len: u16, physical: u64) {
        let off = 12 + slot * 12;
        put32(leaf, off, logical);
        put16(leaf, off + 4, len);
        put16(leaf, off + 6, (physical >> 32) as u16);
        put32(leaf, off + 8, physical as u32);
    }
    fn input(has_extents: u32) -> SyncPlanInput {
        SyncPlanInput { block_size: 4096, blocks_per_group: 128, groups_count: 8,
            inodes_per_group: 128, inode_size: 256, inode_number: 130,
            first_data_block: 0, blocks_count: 1024, descriptor_table_byte: 2048,
            descriptor_size: 64, leaf_capacity: 340, has_extents, has_journal_sync: 1 }
    }
    fn fake() -> Fake { Fake { block_size:4096, leaves:BTreeMap::new(), groups_read:BTreeSet::new(), fail_leaf:false, read_status:EIO, group_status:0, read_blocks:Vec::new(), scoped_calls:0, full_calls:0, flush_status:0 } }
    fn run(input: &SyncPlanInput, root: &[u8], fake: &mut Fake, page_cap: usize) -> (i32, Vec<u64>) {
        let mut workspace = vec![0u8; input.block_size as usize];
        workspace[..root.len()].copy_from_slice(root);
        let ops = SyncPlanOps { context: fake as *mut _ as *mut c_void,
            workspace: workspace.as_mut_ptr(), read_block: Some(read_block),
            group_meta: Some(group_meta), sync_scoped: Some(sync_scoped),
            sync_full: Some(sync_full) };
        let mut pages = vec![0u64; page_cap];
        let mut count = 0usize;
        let status = unsafe { a20_ext4_sync_plan(input, &ops, pages.as_mut_ptr(),
                                                  pages.len(), &mut count) };
        pages.truncate(count);
        (status, pages)
    }

    fn run_dispatch(input: &SyncPlanInput, root: &[u8], fake: &mut Fake,
                    page_cap: usize) -> (i32, usize) {
        let mut workspace = vec![0u8; input.block_size as usize];
        workspace[..root.len()].copy_from_slice(root);
        let ops = SyncPlanOps { context: fake as *mut _ as *mut c_void,
            workspace: workspace.as_mut_ptr(), read_block: Some(read_block),
            group_meta: Some(group_meta), sync_scoped: Some(sync_scoped),
            sync_full: Some(sync_full) };
        let mut pages = vec![0u64; page_cap];
        let mut count = usize::MAX;
        let status = unsafe { a20_ext4_sync_run(input, &ops, pages.as_mut_ptr(),
                                                pages.len(), &mut count) };
        (status, count)
    }

    #[test]
    fn production_dispatch_uses_scoped_callback_for_complete_plan() {
        let mut root = [0u8; 60];
        header(&mut root, 0, 0, 4);
        let mut fake = fake();
        let (status, count) = run_dispatch(&input(1), &root, &mut fake, 2048);
        assert_eq!(status, 0);
        assert!(count > 0);
        assert_eq!((fake.scoped_calls, fake.full_calls), (1, 0));
    }

    #[test]
    fn production_dispatch_uses_full_callback_for_unjournaled_mount() {
        let mut cfg = input(1);
        cfg.has_journal_sync = 0;
        let root = [0u8; 60];
        let mut fake = fake();
        let (status, count) = run_dispatch(&cfg, &root, &mut fake, 2048);
        assert_eq!(status, 0);
        assert_eq!(count, 0);
        assert_eq!((fake.scoped_calls, fake.full_calls), (0, 1));
    }

    #[test]
    fn production_dispatch_does_not_flush_after_malformed_plan() {
        let mut root = [0u8; 60];
        header(&mut root, 0, 5, 4);
        let mut fake = fake();
        let (status, count) = run_dispatch(&input(1), &root, &mut fake, 2048);
        assert_eq!(status, EIO);
        assert_eq!(count, 0);
        assert_eq!((fake.scoped_calls, fake.full_calls), (0, 0));
    }

    #[test]
    fn production_dispatch_propagates_both_flush_errors() {
        let mut root = [0u8; 60];
        header(&mut root, 0, 0, 4);
        let mut fake = fake();
        fake.flush_status = -5;
        let (status, _) = run_dispatch(&input(1), &root, &mut fake, 2048);
        assert_eq!(status, -5);
        assert_eq!((fake.scoped_calls, fake.full_calls), (1, 0));

        let mut cfg = input(1);
        cfg.has_journal_sync = 0;
        fake.flush_status = -117;
        let (status, _) = run_dispatch(&cfg, &root, &mut fake, 2048);
        assert_eq!(status, -117);
        assert_eq!((fake.scoped_calls, fake.full_calls), (1, 1));
    }

    #[test]
    fn production_dispatch_maps_unknown_positive_status_to_eio() {
        let mut fake = fake();
        let ops = SyncPlanOps { context: &mut fake as *mut _ as *mut c_void,
            workspace: core::ptr::null_mut(), read_block: None,
            group_meta: None, sync_scoped: Some(sync_scoped),
            sync_full: Some(sync_full) };
        let pages = [0u64; 1];
        let count = 0usize;
        assert_eq!(unsafe { dispatch_plan_result(2, &ops, pages.as_ptr(), &count) }, EIO);
        assert_eq!((fake.scoped_calls, fake.full_calls), (0, 0));
    }

    #[test]
    fn depth_zero_cross_group_enumerates_start_and_end_groups() {
        let mut root = [0u8; 60]; header(&mut root, 0, 1, 4); root_extent(&mut root, 0, 3, 127);
        let mut fake = fake();
        let (status, pages) = run(&input(1), &root, &mut fake, 2048);
        assert_eq!(status, 0);
        assert!(pages.windows(2).all(|w| w[0] < w[1]));
        assert!(fake.groups_read.contains(&0) && fake.groups_read.contains(&1));
        assert!(pages.contains(&0) && pages.contains(&(127*4096/4096)) && pages.contains(&(128*4096/4096)));
    }

    #[test]
    fn target_inode_selects_only_its_inode_table_page() {
        let mut root = [0u8; 60];
        header(&mut root, 0, 0, 4);
        let mut fake = fake();
        let (status, pages) = run(&input(1), &root, &mut fake, 2048);
        assert_eq!(status, 0);
        // inode 130 is slot 1 in group 1 (table starts at block 132).
        // A full table spans eight blocks, but only the page containing the
        // selected record belongs in this file-scoped plan.
        assert!(pages.contains(&132));
        assert!(!pages.contains(&133));
        assert!(!pages.contains(&139));
    }

    #[test]
    fn root_count_over_capacity_is_corruption() {
        let mut root=[0u8;60]; header(&mut root,0,5,4);
        let mut fake=fake();
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
    }

    #[test]
    fn leaf_entries_and_capacity_are_validated_before_access() {
        let mut root=[0u8;60]; header(&mut root,1,1,4); put32(&mut root,12,0); put32(&mut root,16,10);
        let mut fake=fake(); let mut leaf=vec![0;4096];
        header(&mut leaf,0,2,1); fake.leaves.insert(10,leaf);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
        let mut leaf=vec![0;4096]; header(&mut leaf,0,1,341); fake.leaves.insert(10,leaf);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
    }

    #[test]
    fn negative_leaf_read_propagates_but_deep_tree_falls_back() {
        let mut root=[0u8;60]; header(&mut root,1,1,4); put32(&mut root,16,10);
        let mut fake=fake(); fake.fail_leaf=true; let mut leaf=vec![0;4096]; header(&mut leaf,0,1,1); fake.leaves.insert(10,leaf);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
        fake.fail_leaf=false; fake.read_status=1; fake.fail_leaf=true;
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,FALLBACK);
        header(&mut root,2,0,4);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,FALLBACK);
    }

    #[test]
    fn arithmetic_overflow_is_reported() {
        let mut pages=[0u64;8];
        let mut fake=fake();
        let ops=SyncPlanOps{context:&mut fake as *mut _ as *mut c_void,workspace:core::ptr::null_mut(),read_block:None,group_meta:None,sync_scoped:Some(sync_scoped),sync_full:Some(sync_full)};
        let cfg=input(1);
        let mut planner=Planner{input:&cfg,ops:&ops,pages:&mut pages,count:0,previous_logical_end:0};
        assert_eq!(planner.add_byte_range(u64::MAX-3,8),Err(EOVERFLOW));
    }

    #[test]
    fn no_extent_format_requests_full_sync() {
        let root=[0u8;60];
        let mut fake=fake();
        assert_eq!(run(&input(0),&root,&mut fake,2048).0,FALLBACK);
    }

    #[test]
    fn unjournaled_mount_requests_full_sync_for_shared_inode_page_dependencies() {
        let mut cfg = input(1);
        cfg.has_journal_sync = 0;
        let mut root = [0u8; 60];
        header(&mut root, 0, 1, 4);
        root_extent(&mut root, 0, 0, 100);
        let mut fake = fake();
        assert_eq!(run(&cfg, &root, &mut fake, 2048).0, FALLBACK);
        assert!(fake.groups_read.is_empty());
    }

    #[test]
    fn nonzero_first_data_block_group_math_uses_relative_block_number() {
        let mut cfg = input(1);
        cfg.first_data_block = 1;
        let mut root=[0u8;60]; header(&mut root,0,1,4); root_extent(&mut root,0,3,127);
        let mut fake=fake();
        assert_eq!(run(&cfg,&root,&mut fake,2048).0,0);
        assert!(fake.groups_read.contains(&0));
        assert!(fake.groups_read.contains(&1));
    }

    #[test]
    fn indexed_leaf_extent_must_stay_inside_its_logical_range() {
        let mut root=[0u8;60]; header(&mut root,1,2,4);
        root_index(&mut root,0,0,10); root_index(&mut root,1,8,11);
        let mut fake=fake(); let mut leaf=vec![0;4096];
        header(&mut leaf,0,1,1); leaf_extent(&mut leaf,0,7,2,127); fake.leaves.insert(10,leaf);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
    }

    #[test]
    fn leaf_depth_and_bad_index_order_are_rejected() {
        let mut root=[0u8;60]; header(&mut root,1,1,4); root_index(&mut root,0,0,10);
        let mut fake=fake(); let mut leaf=vec![0;4096]; header(&mut leaf,1,1,1); fake.leaves.insert(10,leaf);
        let mut leaf=vec![0;4096]; header(&mut leaf,1,1,1); fake.leaves.insert(10,leaf);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
        header(&mut root,1,2,4); root_index(&mut root,0,8,10); root_index(&mut root,1,7,11);
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
    }

    #[test]
    fn malformed_descriptor_and_extent_device_ranges_fail_closed() {
        let mut root=[0u8;60]; header(&mut root,0,1,4); root_extent(&mut root,0,4,1022);
        let mut fake=fake();
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,EIO);
        let mut cfg=input(1); cfg.descriptor_table_byte=u64::MAX-16;
        header(&mut root,0,0,4);
        assert_eq!(run(&cfg,&root,&mut fake,2048).0,EOVERFLOW);
    }

    #[test]
    fn depth_one_two_distinct_leaves_use_preserved_root_and_sort_pages() {
        let mut root=[0u8;60]; header(&mut root,1,2,4);
        root_index(&mut root,0,0,10); root_index(&mut root,1,8,11);
        let mut fake=fake();
        let mut a=vec![0;4096]; header(&mut a,0,1,1); leaf_extent(&mut a,0,0,1,20);
        let mut b=vec![0;4096]; header(&mut b,0,1,1); leaf_extent(&mut b,0,8,2,127);
        fake.leaves.insert(10,a); fake.leaves.insert(11,b);
        let (status,pages)=run(&input(1),&root,&mut fake,2048);
        assert_eq!(status,0);
        assert_eq!(fake.read_blocks,vec![10,11]);
        assert!(pages.windows(2).all(|w|w[0]<w[1]));
        assert!(fake.groups_read.contains(&0) && fake.groups_read.contains(&1));
        assert!(pages.contains(&20) && pages.contains(&127) && pages.contains(&128));
    }

    #[test]
    fn group_metadata_negative_errno_is_not_fallback() {
        let mut root=[0u8;60]; header(&mut root,0,0,4);
        let mut fake=fake(); fake.group_status=-117;
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,-117);
        fake.group_status=1;
        assert_eq!(run(&input(1),&root,&mut fake,2048).0,FALLBACK);
    }

    #[test]
    fn ffi_resets_count_on_fallback_and_rejects_null_args() {
        let mut root=[0u8;60]; header(&mut root,2,0,4);
        let mut fake=fake();
        let mut workspace=vec![0;4096]; workspace[..60].copy_from_slice(&root);
        let ops=SyncPlanOps{context:&mut fake as *mut _ as *mut c_void,
            workspace:workspace.as_mut_ptr(),read_block:Some(read_block),group_meta:Some(group_meta),
            sync_scoped:Some(sync_scoped),sync_full:Some(sync_full)};
        let mut pages=[0u64;8]; let mut count=usize::MAX;
        let status=unsafe{a20_ext4_sync_plan(&input(1),&ops,pages.as_mut_ptr(),pages.len(),&mut count)};
        assert_eq!(status,FALLBACK); assert_eq!(count,0);
        assert_eq!(unsafe{a20_ext4_sync_plan(core::ptr::null(),&ops,pages.as_mut_ptr(),pages.len(),&mut count)},EIO);
        assert_eq!(unsafe{a20_ext4_sync_plan(&input(1),&ops,pages.as_mut_ptr(),usize::MAX,&mut count)},EIO);
    }
}

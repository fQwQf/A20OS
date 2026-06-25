#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ptr;

use ffi::{mount_t, vnode_t, FS_TYPE_EXT4, FS_TYPE_FAT32, FS_TYPE_RAMFS, MAX_NAME_LEN};

const VFS_DCACHE_MAX: usize = 512;
const VFS_DCACHE_HASH_BITS: usize = 7;
const VFS_DCACHE_HASH_SIZE: usize = 1 << VFS_DCACHE_HASH_BITS;
const VFS_DCACHE_HASH_MASK: u32 = (VFS_DCACHE_HASH_SIZE as u32) - 1;
const NONE: i32 = -1;

#[derive(Clone, Copy)]
struct DcacheEntry {
    used: bool,
    hash_next: i32,
    hash_prev: i32,
    mnt: *mut mount_t,
    parent_ino: u64,
    name: [u8; MAX_NAME_LEN],
    vn: *mut vnode_t,
    age: u64,
}

impl DcacheEntry {
    const fn empty() -> Self {
        Self {
            used: false,
            hash_next: NONE,
            hash_prev: NONE,
            mnt: ptr::null_mut(),
            parent_ino: 0,
            name: [0; MAX_NAME_LEN],
            vn: ptr::null_mut(),
            age: 0,
        }
    }
}

struct DcacheState {
    entries: [DcacheEntry; VFS_DCACHE_MAX],
    hash_heads: [i32; VFS_DCACHE_HASH_SIZE],
    age: u64,
    free_list: i32,
    free_count: i32,
}

unsafe impl Send for DcacheState {}

impl DcacheState {
    const fn new() -> Self {
        Self {
            entries: [DcacheEntry::empty(); VFS_DCACHE_MAX],
            hash_heads: [NONE; VFS_DCACHE_HASH_SIZE],
            age: 0,
            free_list: 0,
            free_count: VFS_DCACHE_MAX as i32,
        }
    }

    fn alloc_slot(&mut self) -> i32 {
        if self.free_count == 0 {
            let mut oldest = u64::MAX;
            let mut victim = NONE;
            let mut i = 0;
            while i < VFS_DCACHE_MAX {
                let e = &self.entries[i];
                if e.used && e.age < oldest {
                    oldest = e.age;
                    victim = i as i32;
                }
                i += 1;
            }
            return victim;
        }

        let slot = self.free_list;
        if !(0..VFS_DCACHE_MAX as i32).contains(&slot) {
            return NONE;
        }
        self.free_list += 1;
        self.free_count -= 1;
        slot
    }

    fn unlink_hash(&mut self, slot: usize) {
        let prev = self.entries[slot].hash_prev;
        let next = self.entries[slot].hash_next;
        if prev >= 0 {
            self.entries[prev as usize].hash_next = next;
        } else {
            let h = hash_key(
                self.entries[slot].mnt,
                self.entries[slot].parent_ino,
                &self.entries[slot].name,
            ) as usize;
            self.hash_heads[h] = next;
        }
        if next >= 0 {
            self.entries[next as usize].hash_prev = prev;
        }
        self.entries[slot].hash_next = NONE;
        self.entries[slot].hash_prev = NONE;
    }

    fn link_hash(&mut self, slot: usize, h: usize) {
        let head = self.hash_heads[h];
        self.entries[slot].hash_next = head;
        self.entries[slot].hash_prev = NONE;
        if head >= 0 {
            self.entries[head as usize].hash_prev = slot as i32;
        }
        self.hash_heads[h] = slot as i32;
    }

    fn reset_after_flush(&mut self) {
        let mut i = 0;
        while i < VFS_DCACHE_MAX {
            self.entries[i] = DcacheEntry::empty();
            i += 1;
        }
        self.hash_heads = [NONE; VFS_DCACHE_HASH_SIZE];
        self.free_list = 0;
        self.free_count = VFS_DCACHE_MAX as i32;
    }
}

static DCACHE: IrqSaveSpinLock<DcacheState> = IrqSaveSpinLock::new(DcacheState::new());

fn hash_key(mnt: *mut mount_t, ino: u64, name: &[u8; MAX_NAME_LEN]) -> u32 {
    let mut h = (mnt as usize as u32) ^ (ino as u32);
    let mut i = 0;
    while i < MAX_NAME_LEN {
        let ch = name[i];
        if ch == 0 {
            break;
        }
        h = h.wrapping_mul(31).wrapping_add(ch as u32);
        i += 1;
    }
    h & VFS_DCACHE_HASH_MASK
}

unsafe fn dir_key(dir: *mut vnode_t) -> Option<(*mut mount_t, u64)> {
    let mut mnt = ptr::null_mut();
    let mut ino = 0u64;
    if unsafe { ffi::a20_dcache_dir_key(dir, &mut mnt, &mut ino) } == 0 {
        None
    } else {
        Some((mnt, ino))
    }
}

fn mount_type_cacheable(mnt: *mut mount_t) -> bool {
    let ty = unsafe { ffi::a20_dcache_mount_type(mnt) };
    ty == FS_TYPE_RAMFS || ty == FS_TYPE_FAT32 || ty == FS_TYPE_EXT4
}

unsafe fn dcache_enabled_for(dir: *mut vnode_t) -> Option<(*mut mount_t, u64)> {
    let (mnt, ino) = unsafe { dir_key(dir) }?;
    if !mount_type_cacheable(mnt) {
        return None;
    }
    Some((mnt, ino))
}

fn copy_name(dst: &mut [u8; MAX_NAME_LEN], src: *const i8) {
    *dst = [0; MAX_NAME_LEN];
    let mut i = 0;
    unsafe {
        while i + 1 < MAX_NAME_LEN {
            let ch = *src.add(i) as u8;
            if ch == 0 {
                break;
            }
            dst[i] = ch;
            i += 1;
        }
    }
}

fn name_empty(name: *const i8) -> bool {
    if name.is_null() {
        return true;
    }
    unsafe { *name == 0 }
}

fn name_eq(stored: &[u8; MAX_NAME_LEN], other: *const i8) -> bool {
    let mut i = 0;
    unsafe {
        while i < MAX_NAME_LEN {
            let a = stored[i];
            let b = *other.add(i) as u8;
            if a != b {
                return false;
            }
            if a == 0 {
                return true;
            }
            i += 1;
        }
        *other.add(MAX_NAME_LEN - 1) == 0
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_dcache_lookup(dir: *mut vnode_t, name: *const i8) -> *mut vnode_t {
    let (mnt, parent_ino) = match unsafe { dcache_enabled_for(dir) } {
        Some(key) => key,
        None => return ptr::null_mut(),
    };
    if name_empty(name) {
        return ptr::null_mut();
    }

    let mut key_name = [0u8; MAX_NAME_LEN];
    copy_name(&mut key_name, name);
    let h = hash_key(mnt, parent_ino, &key_name) as usize;

    let mut state = DCACHE.lock();
    let mut i = state.hash_heads[h];
    while i >= 0 {
        let idx = i as usize;
        let next = state.entries[idx].hash_next;
        let matched = {
            let e = &state.entries[idx];
            e.used
                && e.mnt == mnt
                && e.parent_ino == parent_ino
                && name_eq(&e.name, name)
                && !e.vn.is_null()
                && unsafe { ffi::vnode_ref_read(e.vn) } > 0
        };
        if matched {
            state.age = state.age.wrapping_add(1);
            let vn = state.entries[idx].vn;
            state.entries[idx].age = state.age;
            unsafe { ffi::vnode_get(vn) };
            return vn;
        }
        i = next;
    }
    ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn vfs_dcache_insert(dir: *mut vnode_t, name: *const i8, vn: *mut vnode_t) {
    let (mnt, parent_ino) = match unsafe { dcache_enabled_for(dir) } {
        Some(key) => key,
        None => return,
    };
    if name_empty(name) || vn.is_null() {
        return;
    }

    let mut key_name = [0u8; MAX_NAME_LEN];
    copy_name(&mut key_name, name);
    let h = hash_key(mnt, parent_ino, &key_name) as usize;

    let mut old_vn = ptr::null_mut();
    {
        let mut state = DCACHE.lock();
        let mut i = state.hash_heads[h];
        while i >= 0 {
            let idx = i as usize;
            let next = state.entries[idx].hash_next;
            let matched = {
                let e = &state.entries[idx];
                e.used && e.mnt == mnt && e.parent_ino == parent_ino && name_eq(&e.name, name)
            };
            if matched {
                state.age = state.age.wrapping_add(1);
                state.entries[idx].age = state.age;
                return;
            }
            i = next;
        }

        let slot = state.alloc_slot();
        if slot < 0 {
            return;
        }
        let slot = slot as usize;

        if state.entries[slot].used {
            old_vn = state.entries[slot].vn;
            state.unlink_hash(slot);
        }

        state.entries[slot] = DcacheEntry::empty();
        state.entries[slot].used = true;
        state.entries[slot].mnt = mnt;
        state.entries[slot].parent_ino = parent_ino;
        state.entries[slot].name = key_name;
        state.entries[slot].vn = vn;
        state.age = state.age.wrapping_add(1);
        state.entries[slot].age = state.age;
        state.link_hash(slot, h);
        unsafe { ffi::vnode_get(vn) };
    }

    if !old_vn.is_null() {
        unsafe { ffi::vnode_put(old_vn) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_dcache_invalidate(dir: *mut vnode_t, name: *const i8) {
    let (mnt, parent_ino) = match unsafe { dir_key(dir) } {
        Some(key) => key,
        None => return,
    };
    if name.is_null() {
        return;
    }

    let mut key_name = [0u8; MAX_NAME_LEN];
    copy_name(&mut key_name, name);
    let h = hash_key(mnt, parent_ino, &key_name) as usize;

    let mut to_put = ptr::null_mut();
    {
        let mut state = DCACHE.lock();
        let mut i = state.hash_heads[h];
        while i >= 0 {
            let idx = i as usize;
            let next = state.entries[idx].hash_next;
            let matched = {
                let e = &state.entries[idx];
                e.used && e.mnt == mnt && e.parent_ino == parent_ino && name_eq(&e.name, name)
            };
            if matched {
                to_put = state.entries[idx].vn;
                state.unlink_hash(idx);
                state.entries[idx] = DcacheEntry::empty();
                break;
            }
            i = next;
        }
    }

    if !to_put.is_null() {
        unsafe { ffi::vnode_put(to_put) };
    }
}

#[no_mangle]
pub extern "C" fn vfs_dcache_invalidate_all() {
    let mut to_put = [ptr::null_mut(); VFS_DCACHE_MAX];
    let mut count = 0usize;
    {
        let mut state = DCACHE.lock();
        let mut i = 0;
        while i < VFS_DCACHE_MAX {
            let e = &state.entries[i];
            if e.used && !e.vn.is_null() {
                to_put[count] = e.vn;
                count += 1;
            }
            i += 1;
        }
        state.reset_after_flush();
    }

    let mut i = 0;
    while i < count {
        unsafe { ffi::vnode_put(to_put[i]) };
        i += 1;
    }
}

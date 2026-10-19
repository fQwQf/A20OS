#![no_std]

//! Process accounting reports.
//!
//! Rust owns the two reports the kernel derives from the task list: the
//! per-process huge-page classification that feeds `/proc/meminfo`, and the
//! `/proc/sys/kernel/pidmap` rendering.  C keeps the tasklist lock primitive,
//! the task-list walk accessors, the page-table leaf query and the pid-id
//! counters.
//!
//! The pidmap text is a faithful port, including its two bounds: the header is
//! truncated to the buffer end, and the per-task loop stops when fewer than
//! sixteen bytes remain so one more ` pid` always fits.

#[cfg(test)]
extern crate std;

pub const PROC_UNUSED: i32 = 0;
pub const PROC_ZOMBIE: i32 = 5;
pub const PAGE_SIZE: u64 = 4096;
pub const VM_SHARED: u64 = 1 << 3;
pub const VM_ANON: u64 = 1 << 4;

/// The C `proc_vm_stats_t`.
#[repr(C)]
pub struct VmStats {
    pub anon_huge_pages: usize,
    pub shmem_huge_pages: usize,
    pub file_huge_pages: usize,
}

/// The fields of one page-table leaf the accounting reads.
#[repr(C)]
pub struct LeafInfo {
    pub level: i32,
    pub base: u64,
    pub size: u64,
}

extern "C" {
    fn a20_tl_tasklist_lock() -> u64;
    fn a20_tl_tasklist_unlock(flags: u64);
    fn a20_tasklist_first_locked() -> *mut core::ffi::c_void;
    fn a20_tasklist_next_locked(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_pid(t: *const core::ffi::c_void) -> i32;
    fn a20_pa_task_state(t: *mut core::ffi::c_void) -> i32;
    fn a20_pa_pid_max() -> i32;
    fn a20_pa_pid_next() -> i32;
    fn a20_pa_task_mm(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_pa_mm_pgdir(mm: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_pa_first_vma(mm: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_pa_next_vma(vma: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_pa_vma_start(vma: *mut core::ffi::c_void) -> u64;
    fn a20_pa_vma_end(vma: *mut core::ffi::c_void) -> u64;
    fn a20_pa_vma_flags(vma: *mut core::ffi::c_void) -> u64;
    fn a20_pa_query_leaf(pgdir: *mut core::ffi::c_void, va: u64,
                         out: *mut LeafInfo) -> i32;
}

fn account_vma(pgdir: *mut core::ffi::c_void, vma: *mut core::ffi::c_void,
               stats: &mut VmStats) {
    if pgdir.is_null() || vma.is_null() {
        return;
    }
    let start = unsafe { a20_pa_vma_start(vma) };
    let end = unsafe { a20_pa_vma_end(vma) };
    let flags = unsafe { a20_pa_vma_flags(vma) };
    let mut va = start;
    while va < end {
        let mut leaf = LeafInfo { level: 0, base: 0, size: 0 };
        if unsafe { a20_pa_query_leaf(pgdir, va, &mut leaf) } != 0 {
            if leaf.level > 0 {
                let pages = (leaf.size / PAGE_SIZE) as usize;
                if flags & VM_ANON != 0 && flags & VM_SHARED != 0 {
                    stats.shmem_huge_pages += pages;
                } else if flags & VM_ANON != 0 {
                    stats.anon_huge_pages += pages;
                } else {
                    stats.file_huge_pages += pages;
                }
                va = leaf.base + leaf.size;
            } else {
                va += PAGE_SIZE;
            }
        } else {
            va += PAGE_SIZE;
        }
    }
}

/// Fill the huge-page counters for every distinct live address space.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_proc_vm_stats(out: *mut VmStats) {
    let stats = unsafe { &mut *out };
    stats.anon_huge_pages = 0;
    stats.shmem_huge_pages = 0;
    stats.file_huge_pages = 0;

    let mut seen: [*mut core::ffi::c_void; 256] = [core::ptr::null_mut(); 256];
    let mut seen_count = 0usize;

    let flags = unsafe { a20_tl_tasklist_lock() };
    let mut t = unsafe { a20_tasklist_first_locked() };
    while !t.is_null() {
        let state = unsafe { a20_pa_task_state(t) };
        let mm = if state == PROC_UNUSED { core::ptr::null_mut() } else { unsafe { a20_pa_task_mm(t) } };
        if !mm.is_null() {
            let mut duplicate = false;
            for i in 0..seen_count {
                if seen[i] == mm {
                    duplicate = true;
                    break;
                }
            }
            if !duplicate {
                if seen_count < seen.len() {
                    seen[seen_count] = mm;
                    seen_count += 1;
                }
                let pgdir = unsafe { a20_pa_mm_pgdir(mm) };
                let mut vma = unsafe { a20_pa_first_vma(mm) };
                while !vma.is_null() {
                    account_vma(pgdir, vma, stats);
                    vma = unsafe { a20_pa_next_vma(vma) };
                }
            }
        }
        t = unsafe { a20_tasklist_next_locked(t) };
    }
    unsafe { a20_tl_tasklist_unlock(flags) };
}

fn put(out: &mut [u8], pos: &mut usize, bytes: &[u8]) {
    out[*pos..*pos + bytes.len()].copy_from_slice(bytes);
    *pos += bytes.len();
}

fn put_i32_at(out: &mut [u8], pos: usize, value: i32) -> usize {
    let mut p = pos;
    put_i32(out, &mut p, value)
}

fn put_i32(out: &mut [u8], pos: &mut usize, value: i32) -> usize {
    let mut digits = [0u8; 11];
    let mut n = 0;
    let mut mag: u32;
    if value < 0 {
        digits[n] = b'-';
        n += 1;
        mag = (-(value as i64)) as u32;
    } else {
        mag = value as u32;
    }
    let mut rev = [0u8; 10];
    let mut count = 0;
    loop {
        rev[count] = b'0' + (mag % 10) as u8;
        count += 1;
        mag /= 10;
        if mag == 0 {
            break;
        }
    }
    while count > 0 {
        count -= 1;
        digits[n] = rev[count];
        n += 1;
    }
    put(out, pos, &digits[..n]);
    *pos
}

/// Render `/proc/sys/kernel/pidmap` and return the number of bytes written.
///
/// # Safety
/// `buf` points at `bufsz` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_proc_format_pidmap(buf: *mut u8, bufsz: usize) -> usize {
    if buf.is_null() || bufsz == 0 {
        return 0;
    }
    let out = unsafe { core::slice::from_raw_parts_mut(buf, bufsz) };

    let flags = unsafe { a20_tl_tasklist_lock() };
    let mut used = 0i32;
    let mut t = unsafe { a20_tasklist_first_locked() };
    while !t.is_null() {
        if unsafe { a20_pa_task_state(t) } != PROC_UNUSED {
            used += 1;
        }
        t = unsafe { a20_tasklist_next_locked(t) };
    }

    // The header is built in a scratch buffer and copied under the same
    // truncation rule the C snprintf path used: a header that does not fit
    // leaves the cursor at bufsz - 1.
    let mut hdr = [0u8; 64];
    let mut hl = 0usize;
    put(&mut hdr, &mut hl, b"pid_max: ");
    hl = put_i32_at(&mut hdr, hl, unsafe { a20_pa_pid_max() });
    put(&mut hdr, &mut hl, b"\nnext_pid: ");
    hl = put_i32_at(&mut hdr, hl, unsafe { a20_pa_pid_next() });
    put(&mut hdr, &mut hl, b"\nused: ");
    hl = put_i32_at(&mut hdr, hl, used);
    put(&mut hdr, &mut hl, b"\npids:");
    let mut off = if hl >= bufsz { bufsz - 1 } else { hl };
    out[..off].copy_from_slice(&hdr[..off]);

    let mut t = unsafe { a20_tasklist_first_locked() };
    while !t.is_null() && off + 16 < bufsz {
        if unsafe { a20_pa_task_state(t) } == PROC_UNUSED {
            t = unsafe { a20_tasklist_next_locked(t) };
            continue;
        }
        let pid = unsafe { a20_tl_pid(t) };
        put(out, &mut off, b" ");
        off = put_i32(out, &mut off, pid);
        t = unsafe { a20_tasklist_next_locked(t) };
    }
    if off + 1 < bufsz {
        out[off] = b'\n';
        off += 1;
    }
    out[if off < bufsz { off } else { bufsz - 1 }] = 0;
    unsafe { a20_tl_tasklist_unlock(flags) };
    off
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::string::String;
    use std::sync::Mutex;
    use std::vec::Vec;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    #[derive(Clone, Copy)]
    struct FakeTask { state: i32, pid: i32, mm: *mut FakeMm, next: *mut FakeTask }
    #[derive(Clone, Copy)]
    struct FakeVma { start: u64, end: u64, flags: u64, next: *mut FakeVma }
    struct FakeMm { pgdir: usize, vma: *mut FakeVma }
    #[derive(Clone, Copy)]
    struct FakeLeaf { va: u64, level: i32, base: u64, size: u64 }

    static mut TASKS: [FakeTask; 4] = [FakeTask { state: 0, pid: 0, mm: core::ptr::null_mut(), next: core::ptr::null_mut() }; 4];
    static mut MMS: [FakeMm; 2] = [FakeMm { pgdir: 0, vma: core::ptr::null_mut() }, FakeMm { pgdir: 0, vma: core::ptr::null_mut() }];
    static mut VMAS: [FakeVma; 4] = [FakeVma { start: 0, end: 0, flags: 0, next: core::ptr::null_mut() }; 4];
    static mut LEAVES: [FakeLeaf; 8] = [FakeLeaf { va: 0, level: 0, base: 0, size: 0 }; 8];
    static mut LEAF_COUNT: usize = 0;
    static mut PID_MAX: i32 = 32768;
    static mut PID_NEXT: i32 = 9;

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe {
            TASKS = [FakeTask { state: 0, pid: 0, mm: core::ptr::null_mut(), next: core::ptr::null_mut() }; 4];
            MMS = [FakeMm { pgdir: 0, vma: core::ptr::null_mut() }, FakeMm { pgdir: 0, vma: core::ptr::null_mut() }];
            VMAS = [FakeVma { start: 0, end: 0, flags: 0, next: core::ptr::null_mut() }; 4];
            LEAVES = [FakeLeaf { va: 0, level: 0, base: 0, size: 0 }; 8];
            LEAF_COUNT = 0;
            PID_MAX = 32768;
            PID_NEXT = 9;
        }
        guard
    }

    fn task(i: usize) -> *mut FakeTask { unsafe { core::ptr::addr_of_mut!(TASKS[i]) } }
    fn mm(i: usize) -> *mut FakeMm { unsafe { core::ptr::addr_of_mut!(MMS[i]) } }
    fn vma(i: usize) -> *mut FakeVma { unsafe { core::ptr::addr_of_mut!(VMAS[i]) } }

    fn chain(tasks: &[usize]) {
        unsafe {
            for (n, &i) in tasks.iter().enumerate() {
                TASKS[i].next = if n + 1 < tasks.len() { task(tasks[n + 1]) } else { core::ptr::null_mut() };
            }
        }
    }

    fn v(
        p: *mut FakeTask,
    ) -> *mut core::ffi::c_void { p as *mut core::ffi::c_void }

    #[no_mangle] unsafe extern "C" fn a20_tl_tasklist_lock() -> u64 { 0 }
    #[no_mangle] unsafe extern "C" fn a20_tl_tasklist_unlock(_f: u64) {}
    #[no_mangle] unsafe extern "C" fn a20_tasklist_first_locked() -> *mut core::ffi::c_void { v(task(0)) }
    #[no_mangle] unsafe extern "C" fn a20_tasklist_next_locked(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(t as *mut FakeTask)).next })
    }
    #[no_mangle] unsafe extern "C" fn a20_tl_pid(t: *const core::ffi::c_void) -> i32 { unsafe { (*(t as *const FakeTask)).pid } }
    #[no_mangle] unsafe extern "C" fn a20_pa_task_state(t: *mut core::ffi::c_void) -> i32 { unsafe { (*(t as *mut FakeTask)).state } }
    #[no_mangle] unsafe extern "C" fn a20_pa_pid_max() -> i32 { unsafe { PID_MAX } }
    #[no_mangle] unsafe extern "C" fn a20_pa_pid_next() -> i32 { unsafe { PID_NEXT } }
    #[no_mangle] unsafe extern "C" fn a20_pa_task_mm(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        unsafe { (*(t as *mut FakeTask)).mm as *mut core::ffi::c_void }
    }
    #[no_mangle] unsafe extern "C" fn a20_pa_mm_pgdir(m: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        unsafe { (*(m as *mut FakeMm)).pgdir as *mut core::ffi::c_void }
    }
    #[no_mangle] unsafe extern "C" fn a20_pa_first_vma(m: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        unsafe { (*(m as *mut FakeMm)).vma as *mut core::ffi::c_void }
    }
    #[no_mangle] unsafe extern "C" fn a20_pa_next_vma(x: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        unsafe { (*(x as *mut FakeVma)).next as *mut core::ffi::c_void }
    }
    #[no_mangle] unsafe extern "C" fn a20_pa_vma_start(x: *mut core::ffi::c_void) -> u64 { unsafe { (*(x as *mut FakeVma)).start } }
    #[no_mangle] unsafe extern "C" fn a20_pa_vma_end(x: *mut core::ffi::c_void) -> u64 { unsafe { (*(x as *mut FakeVma)).end } }
    #[no_mangle] unsafe extern "C" fn a20_pa_vma_flags(x: *mut core::ffi::c_void) -> u64 { unsafe { (*(x as *mut FakeVma)).flags } }
    #[no_mangle] unsafe extern "C" fn a20_pa_query_leaf(_pgdir: *mut core::ffi::c_void, va: u64,
                                                        out: *mut LeafInfo) -> i32 {
        unsafe {
            for i in 0..LEAF_COUNT {
                if LEAVES[i].va == va {
                    (*out).level = LEAVES[i].level;
                    (*out).base = LEAVES[i].base;
                    (*out).size = LEAVES[i].size;
                    return 1;
                }
            }
        }
        0
    }

    fn pidmap(bufsz: usize) -> (usize, String) {
        let mut buf = std::vec![0u8; bufsz];
        let n = unsafe { a20_proc_format_pidmap(buf.as_mut_ptr(), bufsz) };
        let end = n.min(buf.len());
        (n, String::from_utf8_lossy(&buf[..end]).into_owned())
    }

    #[test]
    fn pidmap_lists_the_live_pids_only() {
        let _g = reset();
        unsafe {
            TASKS[0] = FakeTask { state: 2, pid: 1, mm: core::ptr::null_mut(), next: core::ptr::null_mut() };
            TASKS[1] = FakeTask { state: 5, pid: 42, mm: core::ptr::null_mut(), next: core::ptr::null_mut() };
            TASKS[2] = FakeTask { state: 0, pid: 99, mm: core::ptr::null_mut(), next: core::ptr::null_mut() };
            TASKS[3] = FakeTask { state: 1, pid: 7, mm: core::ptr::null_mut(), next: core::ptr::null_mut() };
        }
        chain(&[0, 1, 2, 3]);
        let (n, text) = pidmap(4096);
        assert_eq!(text, "pid_max: 32768\nnext_pid: 9\nused: 3\npids: 1 42 7\n");
        assert_eq!(n, text.len());
    }

    #[test]
    fn pidmap_truncates_a_short_buffer_like_the_c_snprintf_path() {
        let _g = reset();
        unsafe {
            TASKS[0] = FakeTask { state: 2, pid: 1, mm: core::ptr::null_mut(), next: core::ptr::null_mut() };
        }
        chain(&[0]);
        for bufsz in [8usize, 20, 40] {
            let (n, text) = pidmap(bufsz);
            assert_eq!(n, text.len());
            assert_eq!(n, bufsz - 1, "a header that does not fit stops at bufsz-1");
            assert!(!text.is_empty());
        }
        // A buffer that holds the header but leaves under 16 bytes for a pid:
        // the header stays and the trailing newline is appended.
        let (n, text) = pidmap(48);
        assert_eq!(n, 41);
        assert!(text.starts_with("pid_max: 32768\nnext_pid: 9\nused: 1\npids:"));
    }

    #[test]
    fn pidmap_stops_appending_when_less_than_sixteen_bytes_remain() {
        let _g = reset();
        unsafe {
            for i in 0..4 {
                TASKS[i] = FakeTask { state: 2, pid: 10 + i as i32, mm: core::ptr::null_mut(), next: core::ptr::null_mut() };
            }
        }
        chain(&[0, 1, 2, 3]);
        let (_, text) = pidmap(80);
        assert!(text.contains("pids: 10 11 12 13"), "{text}");
        let (_, text) = pidmap(46);
        assert!(text.ends_with('\n'));
        assert!(!text.contains("pids: 10"), "no pid fits in the remaining room: {text}");
        let (_, text) = pidmap(64);
        assert!(text.contains("12") && !text.contains("13"), "{text}");
    }

    fn stats() -> (usize, usize, usize) {
        let mut s = VmStats { anon_huge_pages: 99, shmem_huge_pages: 99, file_huge_pages: 99 };
        unsafe { a20_proc_vm_stats(&mut s) };
        (s.anon_huge_pages, s.shmem_huge_pages, s.file_huge_pages)
    }

    #[test]
    fn vm_stats_starts_from_zero() {
        let _g = reset();
        assert_eq!(stats(), (0, 0, 0));
    }

    #[test]
    fn vm_stats_classifies_huge_leaves_by_vma_flags() {
        let _g = reset();
        unsafe {
            MMS[0].pgdir = 0x1000;
            MMS[0].vma = vma(0);
            VMAS[0] = FakeVma { start: 0x1000, end: 0x9000, flags: VM_ANON, next: core::ptr::null_mut() };
            LEAVES[0] = FakeLeaf { va: 0x1000, level: 2, base: 0x1000, size: 0x4000 };
            LEAVES[1] = FakeLeaf { va: 0x5000, level: 2, base: 0x5000, size: 0x4000 };
            LEAF_COUNT = 2;
            TASKS[0] = FakeTask { state: 2, pid: 1, mm: mm(0), next: core::ptr::null_mut() };
        }
        chain(&[0]);
        assert_eq!(stats(), (8, 0, 0), "two 16 KiB leaves are eight pages");
    }

    #[test]
    fn vm_stats_separates_shared_anon_and_file_leaves() {
        let _g = reset();
        unsafe {
            MMS[0].pgdir = 0x1000;
            MMS[0].vma = vma(0);
            VMAS[0] = FakeVma { start: 0, end: 0x3000, flags: VM_ANON | VM_SHARED, next: vma(1) };
            VMAS[1] = FakeVma { start: 0x3000, end: 0x6000, flags: 1 /* VM_READ: file */, next: core::ptr::null_mut() };
            LEAVES[0] = FakeLeaf { va: 0, level: 2, base: 0, size: 0x2000 };
            LEAVES[1] = FakeLeaf { va: 0x3000, level: 2, base: 0x3000, size: 0x2000 };
            LEAF_COUNT = 2;
            TASKS[0] = FakeTask { state: 2, pid: 1, mm: mm(0), next: core::ptr::null_mut() };
        }
        chain(&[0]);
        assert_eq!(stats(), (0, 2, 2));
    }

    #[test]
    fn vm_stats_skips_base_pages_and_counts_a_shared_mm_once() {
        let _g = reset();
        unsafe {
            MMS[0].pgdir = 0x1000;
            MMS[0].vma = vma(0);
            VMAS[0] = FakeVma { start: 0x2000, end: 0x4000, flags: VM_ANON, next: core::ptr::null_mut() };
            // The first page is a base page (level 0); the walk advances a page.
            LEAVES[0] = FakeLeaf { va: 0x2000, level: 0, base: 0x2000, size: PAGE_SIZE };
            LEAVES[1] = FakeLeaf { va: 0x3000, level: 2, base: 0x3000, size: PAGE_SIZE };
            LEAF_COUNT = 2;
            TASKS[0] = FakeTask { state: 2, pid: 1, mm: mm(0), next: task(2) };
            // A second live task sharing the same mm must not double count.
            TASKS[1] = FakeTask { state: 0, pid: 2, mm: mm(0), next: core::ptr::null_mut() };
            TASKS[2] = FakeTask { state: 2, pid: 3, mm: mm(0), next: core::ptr::null_mut() };
        }
        chain(&[0, 2]);
        assert_eq!(stats(), (1, 0, 0));
    }
}

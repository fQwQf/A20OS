#![no_std]

//! PID id-space allocation for the initial namespace.
//!
//! Rust owns the whole allocator: the occupancy bitmap, the next-id cursor, the
//! `pid_max` limit and the free-id search policy including the wrap.  The C
//! adapter keeps the `pid_lock` spinlock and the `task_t` hash chain, which is
//! storage for task lookup rather than part of the id space.
//!
//! The search mirrors the previous C implementation operation for operation,
//! including the two details that are easy to get wrong: the masked first word
//! when the cursor is mid-word, and the wrap that restarts the scan at id 1
//! when the forward scan finds nothing.

#[cfg(test)]
extern crate std;

const EAGAIN: i32 = 11;
const EINVAL: i32 = 22;

/// Largest id the bitmap can represent; also the bound `clear` accepts.
pub const PID_MAX_LIMIT: i32 = 4_194_304;
const PID_BITMAP_WORDS: usize = (PID_MAX_LIMIT as usize + 63) / 64;
/// Linux's default `pid_max`; the C original clamped the initial value the
/// same way.
const INITIAL_PID_MAX: i32 = if PID_MAX_LIMIT < 32_768 {
    PID_MAX_LIMIT
} else {
    32_768
};

// The bitmap stays a plain zero-initialized array so it lands in .bss.  A
// struct with the non-zero scalar fields next to it would put all 512 KiB in
// .data and grow the kernel image by that much.
static mut BITMAP: [u64; PID_BITMAP_WORDS] = [0; PID_BITMAP_WORDS];
static mut NEXT_PID: i32 = 1;
static mut PID_MAX: i32 = INITIAL_PID_MAX;

#[inline]
fn ctz64(value: u64) -> i32 {
    if value == 0 {
        64
    } else {
        value.trailing_zeros() as i32
    }
}

#[inline]
unsafe fn bitmap_word(idx: usize) -> u64 {
    unsafe { core::ptr::read((core::ptr::addr_of!(BITMAP) as *const u64).add(idx)) }
}

#[inline]
unsafe fn set_bitmap_word(idx: usize, value: u64) {
    unsafe { core::ptr::write((core::ptr::addr_of_mut!(BITMAP) as *mut u64).add(idx), value) };
}

#[inline]
unsafe fn current_pid_max() -> i32 {
    unsafe { core::ptr::read(core::ptr::addr_of!(PID_MAX)) }
}

/// Reset the id space to empty with the cursor at 1.
///
/// # Safety
/// The caller must hold the lock that serializes every other entry point here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pid_alloc_init() {
    let bitmap = core::ptr::addr_of_mut!(BITMAP) as *mut u64;
    for idx in 0..PID_BITMAP_WORDS {
        unsafe { core::ptr::write(bitmap.add(idx), 0) };
    }
    unsafe { core::ptr::write(core::ptr::addr_of_mut!(NEXT_PID), 1) };
    unsafe { core::ptr::write(core::ptr::addr_of_mut!(PID_MAX), INITIAL_PID_MAX) };
}

fn bitmap_set(pid: i32, pid_max: i32) {
    if pid < 1 || pid > pid_max {
        return;
    }
    let idx = (pid - 1) as usize / 64;
    let bit = (pid - 1) as u32 % 64;
    let word = unsafe { bitmap_word(idx) };
    unsafe { set_bitmap_word(idx, word | (1u64 << bit)) };
}

fn bitmap_clear(pid: i32) {
    if pid < 1 || pid > PID_MAX_LIMIT {
        return;
    }
    let idx = (pid - 1) as usize / 64;
    let bit = (pid - 1) as u32 % 64;
    let word = unsafe { bitmap_word(idx) };
    unsafe { set_bitmap_word(idx, word & !(1u64 << bit)) };
}

fn find_free(start: i32, pid_max: i32) -> i32 {
    let limit = if pid_max > 0 { pid_max } else { 1 };
    let word_start = (start - 1) as usize / 64;

    for w in 0..PID_BITMAP_WORDS {
        let wi = (word_start + w) % PID_BITMAP_WORDS;
        let bits = unsafe { bitmap_word(wi) };
        if bits == u64::MAX {
            continue;
        }
        let bit_base = wi * 64;
        let mut inv = !bits;
        if wi == word_start {
            let start_bit = (start - 1) as u32 % 64;
            if start_bit > 0 {
                inv &= !((1u64 << start_bit) - 1);
            }
        }
        let first = ctz64(inv);
        if first == 64 {
            continue;
        }
        let candidate = bit_base as i32 + first + 1;
        if candidate >= 1 && candidate <= limit {
            return candidate;
        }
    }
    -1
}

/// Reserve one id, or return `-EAGAIN` when the id space is exhausted.
///
/// # Safety
/// The caller must hold the lock that serializes every other entry point here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pid_alloc() -> i32 {
    let pid_max = unsafe { current_pid_max() };
    let limit = if pid_max > 0 { pid_max } else { 1 };
    let mut next_pid = unsafe { core::ptr::read(core::ptr::addr_of!(NEXT_PID)) };

    let mut pid = find_free(next_pid, pid_max);
    if pid < 0 && next_pid > 1 {
        pid = find_free(1, pid_max);
    }
    if pid >= 1 && pid <= limit {
        bitmap_set(pid, pid_max);
        next_pid = pid + 1;
        if next_pid > limit {
            next_pid = 1;
        }
        unsafe { core::ptr::write(core::ptr::addr_of_mut!(NEXT_PID), next_pid) };
        return pid;
    }
    -EAGAIN
}

/// Return one id to the space.  Ids outside the representable range are
/// ignored, matching the previous bitmap helper.
///
/// # Safety
/// The caller must hold the lock that serializes every other entry point here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pid_release(pid: i32) {
    bitmap_clear(pid);
}

/// Current `pid_max`.
///
/// # Safety
/// Read-only; safe to call without the lock, as the C accessor was.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pid_max() -> i32 {
    unsafe { current_pid_max() }
}

/// Current next-id cursor.
///
/// # Safety
/// Read-only; safe to call without the lock, as the C accessor was.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pid_next_value() -> i32 {
    unsafe { core::ptr::read(core::ptr::addr_of!(NEXT_PID)) }
}

/// Set `pid_max`, resetting the cursor when it now points outside the range.
///
/// # Safety
/// The caller must hold the lock that serializes every other entry point here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pid_set_max(value: i32) -> i32 {
    if value < 1 || value > PID_MAX_LIMIT {
        return -EINVAL;
    }
    unsafe { core::ptr::write(core::ptr::addr_of_mut!(PID_MAX), value) };
    let next_pid = unsafe { core::ptr::read(core::ptr::addr_of!(NEXT_PID)) };
    if next_pid < 1 || next_pid > value {
        unsafe { core::ptr::write(core::ptr::addr_of_mut!(NEXT_PID), 1) };
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every test drives the one static id space, so they must not run
    /// concurrently.  The guard also resets the space before the test body.
    static TEST_LOCK : std::sync::Mutex<()> = std::sync::Mutex::new (());

    fn reset() -> std::sync::MutexGuard <'static, ()> { let guard =
        TEST_LOCK.lock().unwrap_or_else(| e | e.into_inner());
    unsafe{a20_pid_alloc_init()};
    guard
}

#[test]
fn fresh_space_hands_out_ids_from_one_in_order() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_alloc()}, 1);
    assert_eq !(unsafe{a20_pid_alloc()}, 2);
    assert_eq !(unsafe{a20_pid_alloc()}, 3);
    assert_eq !(unsafe{a20_pid_next_value()}, 4);
}

#[test]
fn exhaustion_wraps_the_scan_back_to_the_freed_id() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(3)}, 0);
    assert_eq !(unsafe{a20_pid_alloc()}, 1);
    assert_eq !(unsafe{a20_pid_alloc()}, 2);
    assert_eq !(unsafe{a20_pid_alloc()}, 3);
    assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
    unsafe{a20_pid_release(2)};
    // The cursor wrapped to 1, so only the wrap scan finds the gap at 2.
    assert_eq !(unsafe{a20_pid_alloc()}, 2);
}

#[test]
fn shrinking_pid_max_rehomes_the_cursor_and_bounds_allocation() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(4)}, 0);
        for
            expected in 1..=4 {
                assert_eq !(unsafe{a20_pid_alloc()}, expected);
            }
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);

        assert_eq !(unsafe{a20_pid_set_max(6)}, 0);
        assert_eq !(unsafe{a20_pid_alloc()}, 5);
        assert_eq !(unsafe{a20_pid_alloc()}, 6);
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);

        unsafe{a20_pid_release(1)};
        assert_eq !(unsafe{a20_pid_set_max(2)}, 0);
        assert_eq !(unsafe{a20_pid_next_value()}, 1);
        assert_eq !(unsafe{a20_pid_alloc()}, 1);
}

#[test]
fn pid_max_rejects_values_outside_the_representable_range() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(0)}, -EINVAL);
    assert_eq !(unsafe{a20_pid_set_max(-1)}, -EINVAL);
    assert_eq !(unsafe{a20_pid_set_max(PID_MAX_LIMIT + 1)}, -EINVAL);
    assert_eq !(unsafe{a20_pid_max()}, INITIAL_PID_MAX);
    assert_eq !(unsafe{a20_pid_set_max(PID_MAX_LIMIT)}, 0);
    assert_eq !(unsafe{a20_pid_max()}, PID_MAX_LIMIT);
}

#[test]
fn the_last_id_of_a_bitmap_word_is_usable_and_recycled() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(64)}, 0);
        for
            expected in 1..=64 {
                assert_eq !(unsafe{a20_pid_alloc()}, expected);
            }
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
        unsafe{a20_pid_release(64)};
        assert_eq !(unsafe{a20_pid_alloc()}, 64);
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
}

#[test]
fn the_cursor_mask_skips_ids_before_the_cursor_in_the_first_word() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(70)}, 0);
        for
            expected in 1..=66 {
                assert_eq !(unsafe{a20_pid_alloc()}, expected);
            }
        // Free two ids inside the second bitmap word, then move past them.
        unsafe{a20_pid_release(65)};
        unsafe{a20_pid_release(66)};
        assert_eq !(unsafe{a20_pid_next_value()}, 67);
        for
            expected in 67..=70 {
                assert_eq !(unsafe{a20_pid_alloc()}, expected);
            }
        // The forward scan is exhausted, so the wrap reaches the two ids the
        // masked first word hid, and only then is the space full.
        assert_eq !(unsafe{a20_pid_alloc()}, 65);
        assert_eq !(unsafe{a20_pid_alloc()}, 66);
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
}

#[test]
fn allocation_never_exceeds_pid_max_and_foreign_releases_are_noops() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(5)}, 0);
    let mut seen = [0i32; 5];
        for
            slot in seen.iter_mut() {
                *slot = unsafe{a20_pid_alloc()};
                assert !(*slot >= 1 && *slot <= 5);
            }
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
        unsafe{a20_pid_release(0)};
        unsafe{a20_pid_release(-5)};
        unsafe{a20_pid_release(PID_MAX_LIMIT + 1)};
        assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
}

#[test]
fn release_after_pid_max_shrank_still_frees_the_bit() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(3)}, 0);
    assert_eq !(unsafe{a20_pid_alloc()}, 1);
    assert_eq !(unsafe{a20_pid_alloc()}, 2);
    assert_eq !(unsafe{a20_pid_alloc()}, 3);
    unsafe{a20_pid_release(3)};
    assert_eq !(unsafe{a20_pid_alloc()}, 3);
}

#[test]
fn repeated_allocation_and_release_cycles_keep_the_space_consistent() {
    let _guard = reset();
    assert_eq !(unsafe{a20_pid_set_max(16)}, 0);
        for
            _ in 0..200 {
                let mut held = [0i32; 16];
            for
                slot in held.iter_mut() {
                    *slot = unsafe{a20_pid_alloc()};
                    assert !(*slot >= 1 && *slot <= 16);
                }
            assert_eq !(unsafe{a20_pid_alloc()}, -EAGAIN);
            for
                pid in held {
                    unsafe{a20_pid_release(pid)};
                }
            }
        assert_eq !(unsafe{a20_pid_alloc()}, 1, "cursor returns to the start");
}
}

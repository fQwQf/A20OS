#![no_std]

//! User-space address-space layout randomisation policy.
//!
//! A per-process random layout offset is generated at exec time from the same
//! entropy pool as `getrandom(2)` and `AT_RANDOM`.  The number of entropy bits
//! is bounded by the fixed user layout, and each bound is a deliberate
//! property of the policy rather than a magic number:
//!
//!   - the stack top is capped at `USER_STACK_TOP + PAGE`, and just below it
//!     the vDSO/vvar block is fixed, so stack randomisation can only pick a
//!     page offset inside the 8 MiB window above the stack floor.  To keep at
//!     least ~4 MiB of usable stack, 64-bit takes 10 bits (a 4 MiB range) and
//!     32-bit takes 8 (a 1 MiB range);
//!   - the mmap fallback base grows upward from `MMAP_BASE_ADDR`, so 64-bit
//!     takes 20 bits (a 4 GiB window) and 32-bit takes 10;
//!   - the brk start offset is capped by `USER_TLS_BASE` and bounded again by
//!     the end of the image, so 64-bit takes at most 13 bits (32 MiB) and
//!     32-bit takes 8.
//!
//! fork matches Linux: the child inherits the mm fields through `*child =
//! *parent` and is not re-randomised, so nothing here runs on a fork.
//!
//! NOMMU reuses none of it: the address IS the physical address, so every
//! offset is zero and the mmap base is the constant.
//!
//! What stays C is two layout constants that an arch or board may override and
//! the NOMMU question, neither of which this file can name.  The entropy
//! source is the existing `random_u64()`.

const PAGE_SIZE: u64 = 4096;
const PAGE_SIZE_BITS: u64 = 12;

#[cfg(target_pointer_width = "64")]
mod bits {
    pub const STACK: u32 = 10;
    pub const MMAP: u32 = 20;
    pub const BRK: u32 = 13;
}
#[cfg(not(target_pointer_width = "64"))]
mod bits {
    pub const STACK: u32 = 8;
    pub const MMAP: u32 = 10;
    pub const BRK: u32 = 8;
}

unsafe extern "C" {
    fn random_u64() -> u64;
    fn a20_aslr_mmap_base_addr() -> usize;
    fn a20_aslr_user_tls_base() -> usize;
    fn a20_aslr_is_nommu() -> i32;
}

#[inline]
fn rnd_pages(bits: u32) -> u64 {
    (unsafe { random_u64() }) & ((1u64 << bits) - 1)
}

#[no_mangle]
pub unsafe extern "C" fn mm_aslr_mmap_base() -> usize {
    let base = unsafe { a20_aslr_mmap_base_addr() } as u64;
    if unsafe { a20_aslr_is_nommu() } != 0 {
        return base as usize;
    }
    (base + rnd_pages(bits::MMAP) * PAGE_SIZE) as usize
}

#[no_mangle]
pub unsafe extern "C" fn mm_aslr_stack_offset() -> usize {
    if unsafe { a20_aslr_is_nommu() } != 0 {
        return 0;
    }
    (rnd_pages(bits::STACK) * PAGE_SIZE) as usize
}

/// The brk offset is capped twice: by the distance from the image end to the
/// fixed TLS block, and by the entropy budget.  A base at or above the TLS
/// block has no room and gets no offset at all, which is what keeps the
/// `% max_pages` below from being a division by zero.
#[no_mangle]
pub unsafe extern "C" fn mm_aslr_brk_offset(brk_base: usize) -> usize {
    if unsafe { a20_aslr_is_nommu() } != 0 {
        return 0;
    }
    let tls = unsafe { a20_aslr_user_tls_base() } as u64;
    let base = brk_base as u64;
    let max_pages = if base < tls {
        (tls - base) >> PAGE_SIZE_BITS
    } else {
        0
    };
    let cap = (1u64 << bits::BRK) - 1;
    let max_pages = if max_pages > cap { cap } else { max_pages };
    if max_pages == 0 {
        return 0;
    }
    (((unsafe { random_u64() }) % max_pages) * PAGE_SIZE) as usize
}

#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;

/// Internal RNG state protected by an irqsave spinlock.
///
/// The C implementation used `spin_lock_irqsave` around every access to the
/// global `rng_s` array.  This Rust version makes the lock part of the static
/// state, so the data can only be accessed through the RAII guard returned by
/// `RNG_STATE.lock()`.
struct RngState {
    s: [u64; 4],
    ready: bool,
    generation: u64,
}

impl RngState {
    const fn new() -> Self {
        Self {
            s: [0; 4],
            ready: false,
            generation: 0,
        }
    }

    fn seed_from(&mut self, seed: u64) {
        let mut x = seed;
        for i in 0..4 {
            self.s[i] = splitmix64(&mut x);
        }
    }

    fn mix_entropy(&mut self) {
        let mut x = unsafe { ffi::a20_random_entropy_sample() };
        for i in 0..4 {
            self.s[i] ^= splitmix64(&mut x);
        }
    }

    fn next_u64(&mut self) -> u64 {
        let result = rotl64(self.s[1].wrapping_mul(5), 7).wrapping_mul(9);
        let t = self.s[1] << 17;

        self.s[2] ^= self.s[0];
        self.s[3] ^= self.s[1];
        self.s[1] ^= self.s[2];
        self.s[0] ^= self.s[3];
        self.s[2] ^= t;
        self.s[3] = rotl64(self.s[3], 45);

        result
    }
}

static RNG_STATE: IrqSaveSpinLock<RngState> = IrqSaveSpinLock::new(RngState::new());

const fn rotl64(x: u64, k: u32) -> u64 {
    (x << k) | (x >> (64 - k))
}

fn splitmix64(x: &mut u64) -> u64 {
    let mut z = *x + 0x9e3779b97f4a7c15u64;
    *x = z;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9u64);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111ebu64);
    z ^ (z >> 31)
}

#[no_mangle]
pub unsafe extern "C" fn random_init() {
    let seed = 0xa20f00d5eed12345u64 ^ unsafe { ffi::a20_random_entropy_sample() };
    let mut guard = RNG_STATE.lock();
    guard.seed_from(seed);
    guard.ready = true;
    guard.generation = 0;
}

#[no_mangle]
pub unsafe extern "C" fn random_reseed(seed: u64) {
    let mut guard = RNG_STATE.lock();
    let mixed = seed ^ unsafe { ffi::a20_random_entropy_sample() };
    let mut x = mixed;
    for i in 0..4 {
        guard.s[i] ^= splitmix64(&mut x);
    }
    guard.ready = true;
    guard.generation = guard.generation.wrapping_add(1);
}

#[no_mangle]
pub unsafe extern "C" fn random_u64() -> u64 {
    {
        let mut guard = RNG_STATE.lock();
        if guard.ready {
            guard.generation = guard.generation.wrapping_add(1);
            if guard.generation % 64 == 0 {
                guard.mix_entropy();
            }
            return guard.next_u64();
        }
    }

    unsafe { random_init() };

    let mut guard = RNG_STATE.lock();
    guard.generation = guard.generation.wrapping_add(1);
    if guard.generation % 64 == 0 {
        guard.mix_entropy();
    }
    guard.next_u64()
}

#[no_mangle]
pub unsafe extern "C" fn random_fill(buf: *mut u8, len: usize) {
    if buf.is_null() || len == 0 {
        return;
    }
    let mut p = buf;
    let mut remaining = len;
    while remaining > 0 {
        let r = unsafe { random_u64() };
        let n = core::cmp::min(remaining, core::mem::size_of::<u64>());
        unsafe {
            core::ptr::copy_nonoverlapping(
                &r as *const u64 as *const u8,
                p,
                n,
            );
        }
        p = unsafe { p.add(n) };
        remaining -= n;
    }
}

//! FFI bindings for timekeeping module.

#[repr(C)]
pub struct spinlock_t {
    pub locked: i32,
}

pub const SPINLOCK_INIT: spinlock_t = spinlock_t { locked: 0 };

extern "C" {
    pub fn a20_arch_timer_freq() -> u64;
    pub fn timer_get_ticks() -> u64;
    pub fn a20_irqsave_lock(lock: *mut spinlock_t) -> u64;
    pub fn a20_irqsave_unlock(lock: *mut spinlock_t, flags: u64);
}

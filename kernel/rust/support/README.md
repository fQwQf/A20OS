# kernel/rust/support

Shared support code for all Rust kernel modules.

- `panic_handler.rs`: kernel-wide `#[panic_handler]`.
- `irqsave_lock.c`: C wrappers for `spin_lock_irqsave` / `spin_unlock_irqrestore`
  so Rust modules can use irqsave locks without needing inline arch functions.
- `arch_info.c`: architecture-neutral helper exposing `ARCH_TIMER_FREQ` to Rust.

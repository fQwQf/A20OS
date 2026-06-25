# kernel/rust/support

Shared support code for all Rust kernel modules.

- `panic_handler.rs`: kernel-wide `#[panic_handler]`.
- `irqsave_lock.c`: C wrappers for `spin_lock_irqsave` / `spin_unlock_irqrestore`
  so Rust modules can use irqsave locks without needing inline arch functions.
- `arch_info.c`: architecture-neutral helper exposing `ARCH_TIMER_FREQ` to Rust.
- `page_cache_helpers.c`: C wrappers for frame/PFN/vnode writeback helpers.
- `block_cache_helpers.c`: C wrappers for `block_dev_t` read/write helpers.
- `xattr_helpers.c`: C wrappers for capability and spinlock helpers used by xattr.
- `time_helpers.c`: C wrapper for `build_unix_time` used by timekeeping.
- `sync_helpers.c`: C wrappers for `proc_current`, `proc_make_ready`, `sched`,
  and task-state helpers used by the Rust sync module.
- `slab_helpers.c`: C wrappers for buddy allocator access, PFN/virtual-address
  conversion, and frame metadata used by the Rust slab module.
- `stat_perm_helpers.c`: C wrappers for vnode key/stat/credential access used by
  the Rust stat_perm module.
- `proc_list_helpers.c`: C wrappers for task field access used by the Rust
  proc_list module.
- `random_helpers.c`: C trampoline for entropy sampling used by the Rust random
  module.
- `eventfd_helpers.c`: C wrappers for vfile allocation/free/installation used by the
  Rust eventfd module.
- `timerfd_helpers.c`: C wrappers for vfile allocation/free/installation and
  ops-pointer matching used by the Rust timerfd module.

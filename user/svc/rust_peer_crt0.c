/*
 * rust_peer_crt0.c — A20OS Native startup for the Rust IDL peer.
 *
 * user/svc/echod.c and user/svc/svcman.c satisfy the crt0_rv64.S `call
 * _start_c` by including liba20rt/crt0_a20.h into their own translation
 * unit.  The Rust peer (user/svc/rust_idl_peer.rs) is a separate object with
 * no C translation unit of its own, so this file supplies exactly that header
 * and nothing else:
 *
 *     _start_c()                 — the real startup, also used by the C programs
 *     a20_get_start_info()       — the real accessor the peer calls
 *     __a20_libc_init()          — weak no-op hook
 *     __a20_exit_run_hooks()     — weak no-op hook
 *
 * Rust supplies main() with C linkage.  Nothing in this file touches the
 * service protocol, the IDL, or the syscall ABI: it exists so the peer links
 * against the same crt0 as every other Native program, which is what makes it
 * a real native binary rather than a freestanding harness object.
 */
#include "liba20rt/crt0_a20.h"
#include <stddef.h>

/* Keep the Rust protocol logic architecture-neutral; the SDK owns syscall
 * register placement and the trap instruction for each Native target. */
int64_t a20_rust_peer_syscall6(uint64_t nr, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3, uint64_t a4,
                               uint64_t a5) {
    return a20_syscall6(nr, a0, a1, a2, a3, a4, a5);
}

#if defined(__riscv_xlen) && __riscv_xlen == 32
/* rustc may lower slice equality to memcmp on RV32. Keep this freestanding
 * compiler primitive local to the Rust IDL executables instead of expanding
 * the C runtime ABI for every Native program. */
int memcmp(const void *lhs, const void *rhs, size_t n) {
    const unsigned char *a = lhs;
    const unsigned char *b = rhs;
    while (n--) {
        if (*a != *b) return (int)*a - (int)*b;
        ++a; ++b;
    }
    return 0;
}
#endif

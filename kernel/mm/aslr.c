#include "core/types.h"
#include "mm/user_layout.h"

/*
 * User-space address-space layout randomisation.
 *
 * The bit counts, the three offsets and the brk cap live in
 * kernel/mm/aslr_policy.rs.  What is left here is what Rust cannot name: the
 * two layout constants, which an arch or board may override, and the NOMMU
 * question, whose answer changes the offsets to zero rather than making them
 * small.
 */

uintptr_t a20_aslr_mmap_base_addr(void) { return MMAP_BASE_ADDR; }
uintptr_t a20_aslr_user_tls_base(void) { return USER_TLS_BASE; }

int a20_aslr_is_nommu(void)
{
#ifdef CONFIG_NOMMU
    return 1;
#else
    return 0;
#endif
}

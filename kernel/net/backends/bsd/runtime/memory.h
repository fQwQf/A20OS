#ifndef A20_BSD_RUNTIME_MEMORY_H
#define A20_BSD_RUNTIME_MEMORY_H
#include "core/types.h"
/* Charges are requested backing bytes, including alignment and private headers;
 * slab size-class rounding and the BSD static image are reported separately. */
struct bsd_memory_stats {
    size_t limit, used, peak, allocations, failures;
};
void port_memory_snapshot(struct bsd_memory_stats *out);
#endif

#ifndef A20_SHARED_MMIO_H
#define A20_SHARED_MMIO_H
#include "core/types.h"

/* Shared-address-space device registers. Owner is a logical CPU endpoint;
 * address must be a kernel mapping valid on that endpoint. Width is 1/2/4/8
 * bytes and naturally aligned. These calls do not transfer DMA ownership or
 * replace driver locks. Invalid calls fail closed rather than losing writes. */
uint64_t shared_mmio_read(unsigned owner, const volatile void *address, unsigned width);
void shared_mmio_write(unsigned owner, volatile void *address, unsigned width, uint64_t value);
#endif

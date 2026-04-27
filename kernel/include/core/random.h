#ifndef _CORE_RANDOM_H
#define _CORE_RANDOM_H

#include "core/types.h"

void     random_init(void);
void     random_reseed(uint64_t seed);
uint64_t random_u64(void);
void     random_fill(void *buf, size_t len);

#endif /* _CORE_RANDOM_H */

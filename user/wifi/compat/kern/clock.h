#ifndef A20_RTW_CLOCK_H
#define A20_RTW_CLOCK_H
#include <stdint.h>
void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *);
#endif

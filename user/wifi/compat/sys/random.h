#ifndef A20_RTW_RANDOM_H
#define A20_RTW_RANDOM_H
#include <stddef.h>
#include <sys/types.h>
ssize_t getrandom(void *, size_t, unsigned);
void read_random(void *, unsigned);
#endif
